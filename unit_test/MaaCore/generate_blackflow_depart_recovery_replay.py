"""Compose cleanup, complete inventory scan, direct-depart recovery and real Session/logger.

Device/OCR/wait boundaries are controlled. The captured modal uses production OCR
and page recognition; no synthetic capacity is supplied for its obscured background.
"""
from pathlib import Path
import argparse
import re
import subprocess
import sys
from generate_blackflow_interaction_replay import extract

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--source-ref')
a = p.parse_args()
here = Path(__file__).resolve().parent
repo = here.parents[1]
out = a.output.parent
out.mkdir(parents=True, exist_ok=True)
args = [sys.executable, str(here / 'generate_blackflow_overload_replay.py'), str(out / 'overload.cpp')]
if a.source_ref:
    args += ['--source-ref', a.source_ref]
subprocess.run(args, check=True)
subprocess.run([sys.executable, str(here / 'generate_blackflow_interaction_replay.py'), str(out / 'interaction.cpp')], check=True)
s = (out / 'overload.cpp').read_text('utf-8').split('int main(int argc,char** argv)')[0]
includes, s = s.split('using namespace asst;', 1)
s = 'using namespace asst;' + s
s = s.replace('namespace asst::blackflow {', 'namespace boundary {')
s = s.replace('struct Session {std::string profile(){return "automation_collection";}int run_revision(){return 1;}json::object run_log_state(){return {{"profile","automation_collection"},{"floor",4}};}};',
              'using Session = ::asst::blackflow::BlackFlowSession;')
s = s.replace('bool disk_ok=true;', 'std::function<bool(const RunLogEvent&,std::shared_ptr<cv::Mat>)> sink;\n    bool disk_ok=true;')
s = s.replace('if(!disk_ok||e.phase==fail_phase)return false;',
              'if(!disk_ok||e.phase==fail_phase)return false;if(sink&&!sink(e,image))return false;')
s = s.replace('bool prompt_stays=false;', 'cv::Mat prompt_image;\n    bool prompt_stays=false;')
s = s.replace('cv::Mat result(720,1280,CV_8UC3,', 'if(visible==OverloadPrompt&&!prompt_image.empty())return prompt_image.clone();\n        cv::Mat result(720,1280,CV_8UC3,', 1)
s = s.replace('int page=image.at<cv::Vec3b>(0,0)[0];bool detail=', 'int page=device->page==OverloadPrompt?OverloadPrompt:image.at<cv::Vec3b>(0,0)[0];bool detail=', 1)
s = s.replace('if(task.ends_with("MovementInventoryOverloadPrompt")&&page==OverloadPrompt)row("零件箱已满");',
              'if(task.ends_with("MovementInventoryOverloadPrompt")&&page==OverloadPrompt){::asst::OCRer real(image);real.set_task_info(task);if(auto found=real.analyze())return *found;return {};}')
s = s.replace('InventoryOverloadObservation recognize_inventory_overload(const cv::Mat& image)\n{',
              'InventoryOverloadObservation recognize_inventory_overload(const cv::Mat& image)\n{\n    if(device->page==OverloadPrompt)return ::asst::blackflow::recognize_inventory_overload(image);', 1)
imports = '''using boundary::recognize_inventory_overload;
using boundary::movement_inventory_page_visible;
using boundary::recognize_movement_inventory_remaining_uses;
'''
start = s.index('bool BlackFlowTaskPort::cleanup_overloaded_inventory(')
s = s[:start] + imports + s[start:]
if a.source_ref and 'bool keep_inventory_open)' not in s:
    s = s.replace('bool cleanup_overloaded_inventory(bool,std::string*,bool=false);', 'bool cleanup_overloaded_inventory(bool,std::string*);')
whole = extract((out / 'interaction.cpp').read_text('utf-8'), 'namespace whole_inventory_replay {')
whole = whole.replace('int page=0, last_page=0;', 'std::vector<int> close_states;std::size_t close_frame_index=0;\n    int page=0, last_page=0;')
whole = whole.replace('        return image;', '        if(panel_state!=0&&!close_states.empty())image.at<cv::Vec3b>(0,0)[2]=close_states.at(std::min(close_frame_index++,close_states.size()-1));\n        return image;', 1)
movement_path = 'src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowMovementTaskPlugin.cpp'
movement = subprocess.check_output(['git', 'show', a.source_ref + ':' + movement_path], cwd=repo).decode('utf-8') if a.source_ref else (repo / movement_path).read_text('utf-8')
names = ['HuntedDepartTask', 'StageEncounterBattleDepartTask', 'StageEnterBattleAgainTask', 'HuntedResumeTask',
         'EncounterBattleResumeTask', 'BattleReenterResumeTask', 'DirectDepartOverloadAction', 'RecoveryFailedTask']
constants = '\n'.join(re.search(r'constexpr [^;]+\b' + name + r'\s*=[^;]+;', movement).group() for name in names)
template = (here / 'BlackFlowDepartRecoveryReplay.cpp.in').read_text('utf-8')
template = template.replace('/*CONSTANTS*/', constants)
for marker, method in [('CLEANUP', 'cleanup_direct_depart_overload'), ('RESCAN', 'refresh_inventory_after_direct_depart')]:
    signature = 'bool BlackFlowMovementTaskPlugin::' + method + '('
    body = extract(movement, signature) if signature in movement else 'bool BlackFlowMovementTaskPlugin::' + method + '(std::string*) { return false; }'
    template = template.replace('/*' + marker + '*/', body)
template = template.replace('/*INVENTORY_EVIDENCE*/', extract(movement, 'void BlackFlowMovementTaskPlugin::record_inventory_evidence('))
extra = '''
#include "Config/ResourceLoader.h"
#include "Config/TaskData.h"
#include "MaaUtils/ImageIo.h"
#include "Task/ProcessTask.h"
#include "Task/Roguelike/BlackFlow/BlackFlowLifecycleTaskPlugin.h"
#include "Task/Roguelike/BlackFlow/BlackFlowSession.h"
#include "Task/Roguelike/BlackFlow/BlackFlowTaskPort.h"
#include "Task/Roguelike/BlackFlow/BlackFlowMapObservationSource.h"
#include "Utils/WorkingDir.hpp"
#include "Vision/OCRer.h"
#include <filesystem>
#include <chrono>
#undef Log
#undef LogTraceFunction
'''
helpers = '''
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int vertical_center(const Rect& r){return r.y+r.height/2;}
'''
a.output.write_text(includes + extra + '\nnamespace recovery_replay {\n' + s + helpers + whole + template, 'utf-8')
