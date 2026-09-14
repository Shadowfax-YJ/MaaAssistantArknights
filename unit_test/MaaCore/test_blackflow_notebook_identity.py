"""Replay the production notebook resolution method with map/session inputs pinned.

The same method feeds both finalization and the node-resolution event. C++ merge
tests separately cover later weak observations. No screenshot or archive is edited.
"""
import shutil
import subprocess
import tempfile
from pathlib import Path

from generate_blackflow_evidence_replay import method


def main():
    root = Path(__file__).resolve().parents[2]
    core = root / 'src/MaaCore/Task/Roguelike/BlackFlow'
    resolve = method(core / 'BlackFlowSession.cpp', 'Node BlackFlowSession::resolve_entered_node_identity(')
    context = method(core / 'BlackFlowSession.h', 'enum class PageExecutionStage') + ';\n'
    context += method(core / 'BlackFlowSession.h', 'struct PageExecutionContext') + ';\n'
    source = r'''
#include <iostream>
#include <meojson/json.hpp>
#include "Task/Roguelike/BlackFlow/BlackFlowNodeExecutionTypes.h"
#include "Task/Roguelike/BlackFlow/BlackFlowDeterministicPrediction.h"
#include "Task/Roguelike/BlackFlow/BlackFlowTaskPort.h"
using namespace asst::blackflow;
''' + context + r'''
struct ReplaySession {
    NormalizedMap m_map, m_exploration_notebook;
    std::string m_utopia_ideology;
    std::optional<GridPosition> m_ideal_source;
    std::optional<std::uint64_t> m_ideal_source_generation;
    Node resolve_entered_node_identity(const PageExecutionContext&) const;
};
''' + resolve.replace('BlackFlowSession::', 'ReplaySession::', 1) + r'''
int main(int argc, char** argv) {
    if(argc!=2)return 2;
    const auto fixture=json::open(std::filesystem::path(argv[1]));
    if(!fixture)return 2;
    int failures=0;
    auto check=[&](bool ok,const char* label){std::cout<<(ok?"PASS ":"FAIL ")<<label<<'\n';failures+=!ok;};
    for (const std::string mode : {"center", "expired-center", "center-after-empty-map", "center-no-prediction-frame", "known-elite", "known-elite-after-prediction",
                                  "known-event", "hopeful-soil", "unknown", "other-floor", "wrong-generation", "resident"}) {
        ReplaySession s;
        bool resident=mode=="resident", confirmed=mode.starts_with("known-elite")||mode=="known-event"||resident;
        bool inferred=mode=="center"||mode=="expired-center"||mode=="center-after-empty-map";
        ObservedNode n;n.position={0,0};n.type=resident?NodeType::BattleNormal:NodeType::BattleElite;
        n.name=resident?"虫虫游戏厅":"灌水贤者";
        if(mode=="known-event"){n.type=NodeType::Incident;n.name="桑尼的邀请";}
        n.identity_revealed=confirmed;n.identity_from_prediction=inferred;
        n.identity_source=inferred?"ideal_source_emergency_prediction":resident?"move_preview_stage_name":"ocr";
        n.prediction_rule=inferred?"non_hopeful_ideal_source_is_emergency_battle":"";
        MapObservationBatch b;b.floor=(mode=="other-floor"||resident)?2:1;b.nodes={n};
        if(!s.m_map.merge(b,MapMergePurpose::CurrentObservation)||!s.m_exploration_notebook.merge(b,MapMergePurpose::ExplorationNotebook))return 2;
        if(mode=="known-elite-after-prediction"){
            b.nodes.front().identity_from_prediction=true;b.nodes.front().identity_revealed=false;
            b.nodes.front().identity_source="ideal_source_emergency_prediction";
            b.nodes.front().prediction_rule="non_hopeful_ideal_source_is_emergency_battle";
            if(!s.m_map.merge(b,MapMergePurpose::CurrentObservation))return 2;
        }
        if(mode=="center-after-empty-map"){
            b.nodes.front().type=NodeType::Empty;b.nodes.front().name="林间空地";
            b.nodes.front().identity_revealed=true;b.nodes.front().identity_from_prediction=false;
            b.nodes.front().identity_source="node_resolution_becomes_empty";b.nodes.front().prediction_rule="";
            if(!s.m_map.merge(b,MapMergePurpose::CurrentObservation))return 2;
        }
        PageExecutionContext c;c.floor=b.floor;c.node=*make_stable_node_id(b.floor,n.position);c.map_generation=7;
        c.node_type=confirmed?*n.type:NodeType::HideBattle;c.node_name=*n.name;
        c.identity_from_event_name=mode=="known-event";
        c.battle=NodeBattleRecord{resident?"强买强卖":"灌水贤者",17};
        if(resident){Node original=*s.m_map.snapshot().find_node(c.node);original.name="作战";c.resident_occupied_node=original;}
        if(mode!="unknown"&&mode!="expired-center"&&mode!="center-after-empty-map"){
            s.m_utopia_ideology=mode=="hopeful-soil"?"hopeful-soil":"diffused-mist";
            s.m_ideal_source=n.position;s.m_ideal_source_generation=mode=="wrong-generation"?6:7;
        }
        const Node result=s.resolve_entered_node_identity(c);
        if(resident||mode=="center"){
            const auto& expected=fixture->at("events").at(resident?0:1).at("details");
            check(result.name==expected.get("event_name",std::string{})&&
                  std::string(to_string(result.type))==expected.get("node_type",std::string{})&&
                  result.identity_from_prediction==expected.get("identity_from_prediction",false)&&
                  result.identity_source==expected.get("identity_source",std::string{})&&
                  result.prediction_rule==expected.get("prediction_rule",std::string{})&&
                  result.battle->stage_name==expected.get("battle","stage_name",std::string{}),
                  "producer identity agrees with shared consumer fixture");
        }
        if(mode=="center"||mode=="expired-center"||mode=="center-after-empty-map"||mode=="center-no-prediction-frame") {
            check(result.type==NodeType::BattleElite&&!result.identity_revealed&&result.identity_from_prediction&&
                  result.identity_source=="ideal_source_emergency_prediction"&&result.prediction_rule=="non_hopeful_ideal_source_is_emergency_battle"&&
                  result.name=="灌水贤者",mode.c_str());
        } else if(confirmed) {
            check(result.type==*n.type&&result.name==*n.name&&result.identity_revealed&&!result.identity_from_prediction,mode.c_str());
            if(resident)check(result.battle&&result.battle->stage_name=="强买强卖","resident battle is distinct from original stage");
        } else check(result.type==NodeType::HideBattle&&!result.identity_revealed&&!result.identity_from_prediction,mode.c_str());
    }
    return failures?1:0;
}
'''
    compiler = shutil.which('g++') or shutil.which('clang++')
    if not compiler:
        raise RuntimeError('A C++20 compiler is required')
    with tempfile.TemporaryDirectory(prefix='maa-notebook-identity-') as temp:
        cpp = Path(temp) / 'replay.cpp'
        exe = cpp.with_suffix('.exe')
        cpp.write_text(source, 'utf-8')
        subprocess.run([compiler, '-std=c++20', '-ffunction-sections', '-fdata-sections',
                        '-I', str(root / 'src/MaaCore'), '-I', str(root / 'src/MaaUtils/include'),
                        str(cpp), str(core / 'BlackFlowModel.cpp'), '-Wl,--gc-sections', '-o', str(exe)], check=True)
        return subprocess.run([str(exe), str(root / 'unit_test/MaaCore/fixtures/blackflow-notebook-resolution.json')]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
