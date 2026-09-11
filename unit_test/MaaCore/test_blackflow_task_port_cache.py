"""Regression for BlackFlowTaskPort caches across the real run reset.

Run with Python and a C++17 compiler: python unit_test/MaaCore/test_blackflow_task_port_cache.py
Optional: --compiler /path/to/clang++ (g++ is also supported).

The controller-dependent port is not linked into the lightweight C++ test target.
Compile its actual reset and inspection methods with only OCR/controller I/O mocked;
do not duplicate the cache/reset implementation in the test.
"""
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path



def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=shutil.which('clang++') or shutil.which('g++'))
    args = parser.parse_args()
    if not args.compiler:
        parser.error('a C++17 compiler is required; pass --compiler')
    workspace = tempfile.TemporaryDirectory(prefix='maa-tree-hole-cache-')
    OUT = Path(workspace.name)
    REPO = Path(__file__).resolve().parents[2]

    def function(path,signature):
        text=path.read_text(encoding='utf-8')
        start=text.index(signature); opening=text.index('{',start); depth=1; end=opening+1
        while depth:
            depth += (text[end]=='{')-(text[end]=='}'); end+=1
        return text[start:end]

    method=function(REPO/'src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowTaskPort.cpp',
                    'bool BlackFlowTaskPort::inspect_tree_hole_effect(')
    reset=function(REPO/'src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowTaskPort.cpp',
                   'void BlackFlowTaskPort::reset_run(')
    color=function(REPO/'src/MaaCore/Vision/Roguelike/BlackFlow/BlackFlowFloor.h',
                   '[[nodiscard]] constexpr std::string_view tree_hole_mist_color(')
    source=r'''
    #include <string>
    #include <string_view>
    #include <optional>
    #include <vector>
    #include <array>
    #include <utility>
    #include <initializer_list>
    #include <cstdint>
    #include <iostream>
    #include <memory>
    namespace cv { struct Mat { std::string title,description; }; }
    struct FakeContext {
     cv::Mat panel;
     int toggle_calls=0;
     bool execute(std::initializer_list<std::string>,std::string*) { ++toggle_calls;return true; }
     cv::Mat capture() { return panel; }
     bool capture_stable_map(cv::Mat&,std::string*) { return true; }
     void take_pending_pursuit(){}
    };
    struct FakeMapSource {void reset_run(){}};
    struct FakePopups {
     std::vector<int> pending,pending_node_evidence;
     struct {void reset(){}} recruitment_choices;
     std::string prepared_choice_id;
    };
    struct OCRer {
     cv::Mat panel;std::string task;
     struct Line {std::string text;};
     OCRer(cv::Mat value):panel(value){}
     void set_task_info(std::string value){task=value;}
     bool analyze(){return true;}
     std::vector<Line> get_result(){return {{task.find("Description")!=std::string::npos?panel.description:panel.title}};}
    };
    struct Logger { template<class... T>void info(T&&...){} } Log;
    constexpr auto UtopiaPanelToggleTask="toggle";
    void set_error(std::string* p,std::string s){if(p)*p=s;}
    class BlackFlowTaskPort {
    public:
     FakeContext* m_task_context;
     std::optional<std::uint64_t> m_tree_effect_generation;
     std::string m_tree_effect,m_tree_effect_description;
     bool m_tree_return_pending=false;
     std::optional<std::uint64_t> m_utopia_generation,m_burn_utopia_inspected_generation;
     int m_utopia_observation=0;
     std::optional<int> m_last_stable_map_image,m_battle_preview_map_reference,m_pending_stable_map_image;
     FakePopups* m_collection_popup_state=nullptr;
     FakeMapSource* m_map_source=nullptr;
     explicit BlackFlowTaskPort(FakeContext& c):m_task_context(&c){}
     bool inspect_tree_hole_effect(std::uint64_t,cv::Mat&,std::string*);
     void reset_run();
    };
    '''+method+'\n'+reset+'\n'+color+r'''
    int main(int argc,char**argv){
     const std::string mode=argc>1?argv[1]:"same-generation";
     FakeContext context; BlackFlowTaskPort port(context); cv::Mat image; std::string error;
     // Previous run: generation 6 is a green tree-hole with the recorded OCR typo.
     context.panel={u8"“朱亡者遗怨”",u8"previous green effect"};
     if(!port.inspect_tree_hole_effect(6,image,&error))return 2;
     // Execute the actual production lifecycle reset, not a no-op replacement.
     if(mode!="same-map")port.reset_run();
     // A new run reuses the port, resets its map-generation counter, and reaches 6 again.
     context.panel={u8"“巨人摇篮”",u8"new red effect"};
     const auto before=context.toggle_calls;
     const auto generation=mode=="different-generation"?7:6;
     if(!port.inspect_tree_hole_effect(generation,image,&error))return 2;
     const auto observed_color=tree_hole_mist_color(port.m_tree_effect);
     std::cout<<"mode="<<mode<<" new_panel_toggle_calls="<<context.toggle_calls-before
              <<" cached_title="<<port.m_tree_effect<<" mapped_color="<<observed_color<<"\n";
     const bool fresh=mode=="same-map"
         ? port.m_tree_effect.find(u8"朱亡者遗怨")!=std::string::npos && context.toggle_calls==before
         : port.m_tree_effect.find(u8"巨人摇篮")!=std::string::npos &&
           port.m_tree_effect_description=="new red effect " && context.toggle_calls-before==2;
     std::cout<<(fresh?"PASS":"FAIL")<<": tree-hole cache must be reused only within the same run and map\n";
     return fresh?0:1;
    }
    '''
    (OUT/'cache-repro.cpp').write_text(source,encoding='utf-8')
    exe = OUT / 'cache-repro.exe'
    subprocess.run([args.compiler, '-std=c++17', '-finput-charset=UTF-8', '-fexec-charset=UTF-8',
                    str(OUT / 'cache-repro.cpp'), '-o', str(exe)], check=True)
    failed = False
    for mode in ('same-map', 'same-generation', 'different-generation'):
        result = subprocess.run([str(exe), mode], capture_output=True)
        sys.stdout.buffer.write(result.stdout)
        sys.stderr.buffer.write(result.stderr)
        failed |= result.returncode != 0
    workspace.cleanup()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
