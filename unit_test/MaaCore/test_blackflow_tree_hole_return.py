"""Replay the floor-title callback from MAA-002's 2026-09-11 return stall.

Compile the production callback branch, mocking controller/session I/O only.
The recorded map button scored 0.879787 (<0.9), so map routing never runs.
The return must visit the menu from the title callback, before that gate.
"""

import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def block(source, marker):
    start = source.index(marker)
    opening = source.index("{", start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    source = (ROOT / "src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowLifecycleTaskPlugin.cpp").read_text(encoding="utf-8")
    callback = block(source, "if (work == PendingWork::RecordCurrentFloor")
    verify = block(source, "if (msg == AsstMsg::SubTaskCompleted &&")
    verify_retry = block(source, 'if (msg == AsstMsg::SubTaskStart && task == "BlackFlow@Roguelike@TreeHoleReturnResumeRetry")')
    port_source = (ROOT / "src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowTaskPort.cpp").read_text(encoding="utf-8")
    resume = block(port_source, "bool BlackFlowTaskPort::resume_pending_tree_hole_return(")
    continue_exploration = block(port_source, "bool BlackFlowTaskPort::resume_exploration_after_tree_hole(")
    harness = r'''
#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
const std::string p="BlackFlow@Roguelike@";
const std::string RecoveryFailedTask=p+"RecoveryFailed";
enum class FailureDisposition {RestartRun};
struct OcrTaskInfo {std::vector<std::string> text={"F1","F2","F3","F4","F5","TH"};};
struct Tasks {
    std::map<std::string,std::string> bases;
    template<class T> auto get(std::string) {return std::make_shared<OcrTaskInfo>();}
    bool set_task_base(std::string name,std::string base) {bases[name]=base;return true;}
} Task;
struct Logger {template<class... T> void info(T&&...){} template<class... T> void error(T&&...){} } Log;
struct Session {
    int floor=6,outer_ap=3;bool failed=false;
    std::optional<int> current_floor(){return floor;}
    void clear_current_floor(){floor=0;}
    void fail(std::string,std::string,FailureDisposition){failed=true;}
    bool set_current_floor(int n,std::string*){floor=n;return true;}
};
struct Port {
    Session* session;bool succeeds=true,pursuit=false,pending=true;int calls=0;
    bool resume_pending_tree_hole_return(int floor,std::string*){
        if(!pending)return true;
        if(session->floor!=3||floor!=3||session->outer_ap!=3)return false;
        ++calls;if(succeeds)pending=false;return succeeds;
    }
    bool take_pending_pursuit(){return pursuit;}
    bool has_pending_pursuit(){return pursuit;}
};
struct Details {
    std::string trigger;
    std::string get(const char*,const char*,const char*,const char*)const{
        return trigger==p+"TreeHoleReturnResumeRetry"?"":"F3";
    }
    std::string get(const char*,const char*,const char*)const{return trigger;}
};
enum class PendingWork {RecordCurrentFloor,RetryTreeHoleReturn};
enum class AsstMsg {SubTaskCompleted,SubTaskStart};
struct Lifecycle {
    Session* m_session;Port* m_port;
    PendingWork m_pending;Details m_pending_details;
    std::string m_terminal_trigger,m_terminal_pre_task;
    void report_outputs(){}
    bool verify(const Details& details){
        const auto msg=details.trigger==p+"TreeHoleReturnResumeRetry"?AsstMsg::SubTaskStart:AsstMsg::SubTaskCompleted;
        const std::string task=details.trigger;
''' + verify_retry + '\n' + verify + r'''
        return false;
    }
    bool run(const Details& details){
        const auto work=m_pending;
''' + callback + r'''
        return false;
    }
};
namespace cv {struct Mat {std::string title="F3";};}
void set_error(std::string* out,std::string error){if(out)*out=error;}
struct Context {
    std::string ending=p+"TreeHoleResumeMapReady",title="F3";
    int menus=0,captures=0;
    bool execute(std::initializer_list<std::string> tasks,std::string*){
        if(*tasks.begin()!=p+"TreeHoleResumeMenuWait")return false;
        ++menus;return true;
    }
    const std::string& last_task(){return ending;}
    bool capture_stable_map(cv::Mat& image,std::string*){++captures;image.title=title;return menus>0;}
};
struct BlackFlowTaskPort {
    Context* m_task_context;
    bool m_tree_return_pending=true;
    std::unique_ptr<cv::Mat> m_pending_stable_map_image,m_last_stable_map_image,m_battle_preview_map_reference;
    std::optional<std::string> recognize_text(const cv::Mat& image,std::string){return image.title;}
    bool resume_pending_tree_hole_return(int,std::string*);
    bool resume_exploration_after_tree_hole(int,cv::Mat&,std::string*);
};
''' + resume + '\n' + continue_exploration + r'''
int main(){
    int failures=0;
    for(const std::string mode:{"return","ordinary-floor","pursuit","retry","retry-from-menu"}){
        Session session;Port port{&session};Lifecycle lifecycle{&session,&port};
        if(mode=="retry-from-menu")session.floor=3;
        port.succeeds=mode!="pursuit"&&mode!="retry";port.pursuit=mode=="pursuit";
        Task.bases.clear();
        const Details event{p+(mode=="ordinary-floor"?"NextLevel":mode=="retry-from-menu"?"TreeHoleReturnResumeRetry":"TreeHoleReturnTitle")};
        const bool accepted=lifecycle.verify(event);
        if(accepted)lifecycle.run(event);
        const int expected=mode=="ordinary-floor"?0:1;
        bool ok=accepted&&port.calls==expected&&session.floor==3&&session.outer_ap==3;
        if(mode!="ordinary-floor"){
            const auto expected_route=p+(mode=="pursuit"?"HuntedWait":mode=="retry"?"RecoveryFailed":"NextLevel-Enter");
            ok=ok&&Task.bases[p+"TreeHoleReturnResumeAction"]==expected_route;
        }
        std::cout<<(ok?"PASS ":"FAIL ")<<mode<<" menu_calls="<<port.calls
                 <<" route="<<Task.bases[p+"TreeHoleReturnResumeAction"]<<"\n";
        failures+=!ok;
    }
    for(const std::string mode:{"success-once","timeout","wrong-floor","late-pursuit"}){
        Context context;BlackFlowTaskPort port{&context};std::string error;
        if(mode=="timeout")context.ending=p+"TreeHoleResumeMenuWait";
        if(mode=="late-pursuit")context.ending=p+"TreeHoleResumeHunted";
        if(mode=="wrong-floor")context.title="F4";
        const bool result=port.resume_pending_tree_hole_return(3,&error);
        bool ok;
        if(mode=="success-once"){
            ok=result&&!port.m_tree_return_pending&&!port.m_pending_stable_map_image;
            ok=ok&&port.resume_pending_tree_hole_return(3,&error)&&context.menus==1;
        }else{
            ok=!result&&port.m_tree_return_pending&&!port.m_pending_stable_map_image;
        }
        std::cout<<(ok?"PASS ":"FAIL ")<<mode<<" menus="<<context.menus<<"\n";
        failures+=!ok;
    }
    return failures?1:0;
}
'''
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        raise RuntimeError("A C++20 compiler is required")
    with tempfile.TemporaryDirectory(prefix="maa-tree-return-") as directory:
        cpp = Path(directory) / "replay.cpp"
        exe = cpp.with_suffix(".exe")
        cpp.write_text(harness, encoding="utf-8")
        subprocess.run([compiler, "-std=c++20", str(cpp), "-o", str(exe)], check=True)
        return subprocess.run([str(exe)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
