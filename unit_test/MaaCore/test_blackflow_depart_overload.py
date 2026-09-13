"""Replay the production overload handler with the archived callback source.

Only session, task-resource writes, and inventory I/O are replaced by fakes.
Exit 1 means a resource-supported overload source never reaches inventory cleanup.
"""
import json
import re
import subprocess
import sys
import shutil
import tempfile
from pathlib import Path

def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ROOT = Path(__file__).resolve().parents[2]
    _workspace = tempfile.TemporaryDirectory(prefix="maa-depart-overload-")
    OUT = Path(_workspace.name)
    PREFIX = "BlackFlow@Roguelike@"
    source = (ROOT / "src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowMovementTaskPlugin.cpp").read_text("utf-8")
    marker = "bool BlackFlowMovementTaskPlugin::cleanup_direct_depart_overload("
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    method = source[start:end]
    constants = "\n".join(re.findall(r"constexpr std::string_view\s+\w+\s*=\s*\"[^\"]*\";", source))
    # Archived run-20260913-195846-828939, callback #3525: the prompt arrived
    # after two passive observations, not immediately after the departure click.
    actual_source = PREFIX + "StageEncounterBattleDepartObserve"
    tasks = json.loads((ROOT / "resource/tasks/Roguelike/BlackFlow.json").read_text("utf-8"))
    sources = [name for name, info in tasks.items() if PREFIX + "DirectDepartInventoryOverloadPrompt" in info.get("next", [])]
    assert actual_source in sources
    harness = r'''
    #include <iostream>
    #include <map>
    #include <string>
    #include <string_view>
    #include <vector>
    #include <utility>
    namespace json {using object=std::map<std::string,std::string>;}
    enum class FailureDisposition {StopTask};
    enum class RunLogLevel {Info,Error};
    struct Logger {template<class... A> void error(A&&...){} template<class... A> void info(A&&...){} } Log;
    struct Tasks {
        std::map<std::string,std::string> bases;
        void set_task_base(std::string name,std::string base){bases[name]=base;}
    } Task;
    struct Session {
        bool failed=false, invalidated=false;std::string code,message;
        void fail(std::string c,std::string m,FailureDisposition){failed=true;code=c;message=m;}
        void invalidate_movement_inventory(){invalidated=true;}
    };
    struct Port {
        int cleanup_calls=0;
        bool succeeds=true;
        bool cleanup_depart_inventory_overload(std::string* error){++cleanup_calls;if(!succeeds)*error="cleanup failed";return succeeds;}
    };
    struct BlackFlowMovementTaskPlugin {
        Session* m_session;Port* m_port;
        template<class... A> void record_run_event(A&&...){}
        void report_outputs(){}
        bool cleanup_direct_depart_overload(std::string_view);
    };
    ''' + constants + "\n" + method + r'''
    int main(int argc,char** argv){
        int failures=0;
        for(int i=1;i+1<argc;i+=2){
            Session session;Port port;BlackFlowMovementTaskPlugin plugin{&session,&port};
            Task.bases.clear();
            plugin.cleanup_direct_depart_overload(argv[i]);
            const bool ok=!session.failed&&port.cleanup_calls==1&&session.invalidated&&
                Task.bases[std::string(DirectDepartOverloadAction)]==argv[i+1];
            std::cout<<(ok?"PASS":"FAIL")<<" source="<<argv[i]<<" cleanup_calls="<<port.cleanup_calls
                     <<" successor="<<Task.bases[std::string(DirectDepartOverloadAction)]
                     <<" error="<<session.code<<" message="<<session.message<<'\n';
            failures+=!ok;
        }
        for(const std::string source : {"BlackFlow@Roguelike@StageEncounterBattleDepartDestination", "unknown"}) {
            Session session;Port port;BlackFlowMovementTaskPlugin plugin{&session,&port};
            plugin.cleanup_direct_depart_overload(source);
            const bool ok=session.failed&&port.cleanup_calls==0&&!session.invalidated;
            std::cout<<(ok?"PASS":"FAIL")<<" reject="<<source<<'\n';failures+=!ok;
        }
        Session failed_session;Port failed_port;failed_port.succeeds=false;
        BlackFlowMovementTaskPlugin failed_plugin{&failed_session,&failed_port};
        failed_plugin.cleanup_direct_depart_overload("BlackFlow@Roguelike@StageEncounterBattleDepart");
        const bool failed_ok=failed_session.failed&&failed_port.cleanup_calls==1&&!failed_session.invalidated&&
            Task.bases[std::string(DirectDepartOverloadAction)]==RecoveryFailedTask;
        std::cout<<(failed_ok?"PASS":"FAIL")<<" cleanup failure stops without resuming\n";
        failures+=!failed_ok;
        return failures?1:0;
    }
    '''
    cpp = OUT / "depart_overload_replay.cpp"
    exe = OUT / "depart_overload_replay.exe"
    cpp.write_text(harness, "utf-8")
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        raise RuntimeError("A C++20 compiler is required")
    subprocess.run([compiler, "-std=c++20", "-O0", str(cpp), "-o", str(exe)], check=True)
    print("ACTUAL_CALLBACK", actual_source, flush=True)
    resumes = {
        PREFIX + "HuntedDepart": PREFIX + "DirectDepartHuntedResume",
        PREFIX + "StageEncounterBattleDepart": PREFIX + "DirectDepartEncounterResume",
        PREFIX + "StageEnterBattleAgain": PREFIX + "DirectDepartBattleReenterResume",
    }
    arguments = []
    for item in [actual_source, *[s for s in sources if s != actual_source]]:
        resume = next(value for key, value in resumes.items() if item.startswith(key))
        arguments.extend([item, resume])
    result = subprocess.run([str(exe), *arguments], capture_output=True, text=True, encoding="utf-8")
    print(result.stdout, end="")
    _workspace.cleanup()
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
