"""Replay the real entry classifier and identity resolver with controller/OCR I/O faked.

Run: python unit_test/MaaCore/test_blackflow_hidden_combat_identity.py
The quick-formation template proves combat, not a normal/elite/savage subtype.
"""
import shutil
import subprocess
import tempfile
from pathlib import Path

from generate_blackflow_evidence_replay import method


def main():
    repo = Path(__file__).resolve().parents[2]
    core = repo / 'src/MaaCore/Task/Roguelike/BlackFlow'
    classifier = method(core / 'BlackFlowTaskPort.cpp', 'bool BlackFlowTaskPort::classify_entered_page(')
    resolver = method(core / 'BlackFlowPageIdentity.cpp', 'PageIdentityResolution resolve_page_identity(')
    source = r'''
#include <algorithm>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
enum class NodeType {Unknown,Empty,HideInvisible,HideBattle,BattleNormal,BattleElite,BattleSavage,BattleBoss,Incident,Shop,Final};
bool is_combat_node_type(NodeType t){return t==NodeType::HideBattle||t==NodeType::BattleNormal||t==NodeType::BattleElite||t==NodeType::BattleSavage||t==NodeType::BattleBoss;}
struct MovePreview {NodeType displayed_type=NodeType::Unknown;std::string displayed_name;bool identity_revealed=false;};
struct EnteredPageObservation {std::optional<NodeType> classified_type;std::optional<std::string> event_name;std::vector<std::string> matched_texts;bool classification_conflict=false,inventory_overloaded=false,combat_operator_selection_open=false,map_visible=false;};
struct PageIdentityResolution {NodeType type;std::string name;};
namespace cv {struct Mat {bool quick_formation=true,map_template=false,action_points=false,fixed_title=false;};}
struct OcrTaskInfo {};
struct Tasks {template<class T>std::shared_ptr<T> get(std::string_view)const{return std::make_shared<T>();}} Task;
struct OCRer {cv::Mat frame;struct Line{std::string text;};OCRer(const cv::Mat& im):frame(im){}void set_task_info(std::shared_ptr<OcrTaskInfo>){}std::optional<std::vector<Line>> analyze(){if(frame.fixed_title)return std::vector<Line>{{"final"}};return std::nullopt;}};
struct Context {bool execute(std::initializer_list<std::string>,std::string*){return true;}cv::Mat capture(){return {};}};
struct Logger {template<class... T>void warn(T&&...)const{}} Log;
constexpr std::string_view EnteredPageClassificationTask="page",EnteredPageClassificationInterruptionTask="interrupt",EnteredPageClassificationCombatTask="combat",EnteredPageClassificationCombatOperatorConfirmTask="operator-confirm",EnteredPageClassificationCombatOperatorBackTask="back",StageEncounterOcrTask="event",EnteredPageClassificationRetryWaitTask="wait",EnteredPageClassificationRewardPrepareTask="reward",EnteredPageClassificationEncounterPrepareTask="prepare";
constexpr int EnteredPageCombatOperatorMaximumBacks=2,EnteredPageClassificationRetryTimes=2,EnteredPageRewardPrepareTimes=2;
constexpr std::string_view MapReadyTask="map";
int event_title_reads=0;
bool matches_template(const cv::Mat& im,std::string_view task){return (im.quick_formation&&task==EnteredPageClassificationCombatTask)||(im.map_template&&task==MapReadyTask);}
std::optional<std::string> recognize_text(const cv::Mat&,std::string_view){++event_title_reads;return "informant";}
std::optional<int> recognize_action_points(const cv::Mat& im){if(im.action_points)return 1;return std::nullopt;}
void set_error(std::string* out,std::string s){if(out)*out=std::move(s);}
EnteredPageObservation classify_entered_page_texts(std::vector<std::string> texts){EnteredPageObservation result;if(!texts.empty())result.classified_type=NodeType::Final;return result;}
EnteredPageObservation classify_entered_event_name(std::string name){EnteredPageObservation result;result.classified_type=NodeType::Incident;result.event_name=name;return result;}
struct BlackFlowTaskPort {Context* m_task_context;bool classify_entered_page(const cv::Mat&,EnteredPageObservation&,std::string*)const;};
''' + classifier + '\n' + resolver + r'''
int main(){
    Context context;BlackFlowTaskPort port{&context};EnteredPageObservation entered;std::string error;
    if(!port.classify_entered_page({},entered,&error)){std::cerr<<error;return 2;}
    int failures=0;
    auto check=[&](bool ok,const char* label){std::cout<<(ok?"PASS ":"FAIL ")<<label<<'\n';failures+=!ok;};
    check(entered.classified_type==NodeType::HideBattle,"quick formation preserves unknown combat subtype");
    MovePreview hidden{NodeType::HideBattle,"unknown combat",false};
    auto unknown=resolve_page_identity(NodeType::HideBattle,"unknown combat",&hidden,entered);
    check(unknown.type==NodeType::HideBattle,"direct hidden battle is not promoted to normal");
    MovePreview elite{NodeType::BattleElite,"elite",true};
    check(resolve_page_identity(NodeType::HideBattle,"unknown combat",&elite,entered).type==NodeType::BattleElite,"explicit preview remains elite");
    for(auto known:{NodeType::BattleNormal,NodeType::BattleElite,NodeType::BattleSavage,NodeType::BattleBoss}){
        check(resolve_page_identity(known,"known battle",&hidden,entered).type==known,"generic combat does not overwrite known map subtype");
    }
    auto random=resolve_page_identity(NodeType::HideInvisible,"unknown event",nullptr,entered);
    check(random.type==NodeType::HideBattle,"random unresolved landing remains unknown combat");
    EnteredPageObservation explicit_page;explicit_page.classified_type=NodeType::Shop;
    check(resolve_page_identity(NodeType::HideInvisible,"unknown",nullptr,explicit_page).type==NodeType::Shop,"explicit noncombat classification still resolves");
    for(auto map:{cv::Mat{false,true,true,true},cv::Mat{false,true,true,false},cv::Mat{false,true,false,true}}){
        event_title_reads=0;
        check(port.classify_entered_page(map,entered,&error)&&entered.map_visible&&!entered.classified_type&&!entered.event_name,
              "map return cannot become a final or informant event");
        check(event_title_reads==0,"map labels never enter fuzzy event-title recognition");
    }
    for(auto frame:{cv::Mat{false,false,true,false},cv::Mat{false,false,false,false}}){
        check(port.classify_entered_page(frame,entered,&error)&&!entered.map_visible&&entered.classified_type==NodeType::Incident,
              "numeric OCR without the map template does not discard a real event page");
    }
    return failures?1:0;
}
'''
    compiler = shutil.which('g++') or shutil.which('clang++')
    if not compiler:
        raise RuntimeError('A C++20 compiler is required')
    with tempfile.TemporaryDirectory(prefix='maa-hidden-combat-') as tmp:
        cpp = Path(tmp) / 'replay.cpp'
        exe = Path(tmp) / 'replay.exe'
        cpp.write_text(source, encoding='utf-8')
        subprocess.run([compiler, '-std=c++20', '-finput-charset=UTF-8', '-fexec-charset=UTF-8', str(cpp), '-o', str(exe)], check=True)
        return subprocess.run([str(exe)]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
