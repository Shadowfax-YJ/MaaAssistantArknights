"""Replay the real planned-deployment confirmation and preparation recovery methods.

The 2026-09-13 frame has a direction selector, count 0/1, and no battle flag.
Controller/OCR boundaries are simulated; the production call site decides whether
to retry, accept, or stop. Real-image recognition is checked separately.
"""
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def block(source, marker):
    start = source.index(marker)
    end, depth = source.index("{", start) + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    source = (ROOT / "src/MaaCore/Task/Roguelike/RoguelikeBattleTaskPlugin.cpp").read_text("utf-8")
    rules = (ROOT / "src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowBattleRules.h").read_text("utf-8")
    start = source.index("            if (!deploy_oper(deploy_plan.role")
    end = source.index("            battle::OperNameTag oper_tag { deploy_plan.role, deploy_plan.oper_name };", start)
    confirmation = source[start:end]
    recovery = ""
    if "std::optional<bool> asst::RoguelikeBattleTaskPlugin::confirm_preparation_deployment(" in source:
        recovery = block(source, "std::optional<bool> asst::RoguelikeBattleTaskPlugin::confirm_preparation_deployment(")
    rule_functions = rules[rules.index("[[nodiscard]] inline constexpr bool deployment_attempt_confirmed("):rules.index("// 等待中的虚拟装置")]
    if "parse_preparation_deployment_count(" in rules:
        rule_functions += block(rules, "[[nodiscard]] inline std::optional<std::pair<int, int>> parse_preparation_deployment_count(")
    harness = r'''
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <compare>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
struct Point {
    int x=0,y=0;auto operator<=>(const Point&)const=default;
    Point operator+(Point b)const{return{x+b.x,y+b.y};}
    Point operator*(int n)const{return{x*n,y*n};}
    static Point right(){return{1,0};}static Point left(){return{-1,0};}
    static Point up(){return{0,-1};}static Point down(){return{0,1};}
};
namespace std {template<> struct hash<Point>{size_t operator()(Point p)const{return p.x*100+p.y;}};}
struct Rect {int x=473,y=229,width=72,height=27;};
struct Frame {bool pending=true,known=true,preparation=true;int count=0,total=1;};
namespace cv {using Mat=Frame;}
struct Controller {
    Frame frame;int swipes=0,cancels=0,top_clicks=0,ticks=0;
    int drops=0;bool frozen=false,stopped=false;Point last_end;
    cv::Mat get_image(){return frame;}
    std::pair<int,int> get_scale_size(){return{1280,720};}
    bool swipe(Point,Point end,int){
        ++swipes;last_end=end;if(stopped)return false;
        if(!frozen&&swipes>drops){frame.pending=false;++frame.count;}
        return true;
    }
    bool click(Rect){++cancels;if(stopped)return false;if(!frozen)frame.pending=false;return true;}
}* active;
struct TaskInfo {std::vector<int> special_params={400,100,2,0,300};int post_delay=150;};
struct Tasks {TaskInfo info;TaskInfo* get(std::string){return &info;}}Task;
struct OCRer {
    Frame image;std::string task;
    struct Result {std::string text;Rect rect;};
    std::vector<Result> results;
    OCRer(Frame f):image(f){}
    void set_task_info(std::string t){task=t;}
    bool analyze(){
        results.clear();if(!image.known||!image.preparation)return false;
        if(task.ends_with("BattlePreparationStart"))results.push_back({"start",{}});
        else if(task.ends_with("BattlePreparationDeployCancel")&&image.pending)results.push_back({"cancel",{}});
        else if(task.ends_with("BattlePreparationDeployCount"))results.push_back({std::to_string(image.count)+"/"+std::to_string(image.total),{}});
        return !results.empty();
    }
    const auto& get_result(){return results;}
};
struct Logger {template<class...A>void error(A&&...){}template<class...A>void warn(A&&...){}template<class...A>void info(A&&...){} }Log;
namespace asst {
namespace blackflow {
''' + rule_functions + r'''
}
namespace battle {
enum class DeployDirection {None,Right,Down,Left,Up};
struct OperNameTag {int role;std::string name;auto operator<=>(const OperNameTag&)const=default;};
}
struct Oper {int role=3;std::string name="mechanic";bool cooling=false,available=true;};
struct BattleHelper {
    static bool cancel_oper_selection(){++active->top_clicks;return !active->stopped;}
};
struct Config {std::string get_theme(){return "BlackFlow";}};
class RoguelikeBattleTaskPlugin {
public:
    struct DeployPlanInfo {int role=3;std::string oper_name="mechanic";Point placed{4,3};battle::DeployDirection direction=battle::DeployDirection::Up;};
    struct Tile {Point pos{684,399};};
    struct Plan {Point location{4,3};};
    std::map<Point,Tile> m_side_tile_info={{{4,3},{}}};
    std::map<std::string,std::vector<Plan>> m_preparation_deploy_plan={{"mechanic",{{}}}};
    std::map<Point,battle::OperNameTag> m_used_tiles;
    std::map<battle::OperNameTag,int> m_battlefield_opers,m_last_use_skill_time;
    std::vector<Oper> m_cur_deployment_opers={{}};
    Config config;Config* m_config=&config;std::string m_stage_name="duel";
    bool send_ok=true;
    Controller* ctrler(){return active;}
    bool need_exit(){return active->stopped;}
    bool sleep(int){return ++active->ticks<30&&!need_exit();}
    bool update_deployment(bool){return active->frame.known&&!active->frame.pending;}
    bool deploy_oper(int role,std::string name,Point point,battle::DeployDirection){
        if(!send_ok)return false;
        m_used_tiles[point]={role,name};m_battlefield_opers[{role,name}]=1;m_last_use_skill_time[{role,name}]=1;return true;
    }
    std::optional<bool> confirm_preparation_deployment(const DeployPlanInfo&);
    bool attempt(bool wait_for_confirmation=true){DeployPlanInfo deploy_plan;
''' + confirmation + r'''
        return true;
    }
};
}
using namespace asst::battle;
''' + recovery + r'''
int main(){
    int failures=0;
    for(const std::string mode:{"pending-direction","lost-direction-once","cancel-and-retry","never-closes","unknown-frame","already-deployed","wrong-count","wrong-page","user-stop","combat","co-op-first","co-op-second"}){
        Controller controller;active=&controller;asst::RoguelikeBattleTaskPlugin plugin;
        if(mode.starts_with("co-op-")){
            controller.frame.total=2;plugin.m_preparation_deploy_plan["other"]={{{4,2}}};
            if(mode=="co-op-second"){controller.frame.count=1;plugin.m_used_tiles[{4,2}]={3,"other"};}
        }
        if(mode=="lost-direction-once")controller.drops=1;
        if(mode=="cancel-and-retry")controller.drops=9;
        if(mode=="never-closes")controller.frozen=true;
        if(mode=="unknown-frame")controller.frame.known=false;
        if(mode=="already-deployed"){controller.frame.pending=false;controller.frame.count=1;}
        if(mode=="wrong-count"){controller.frame.pending=false;controller.frame.total=2;controller.frame.count=1;}
        if(mode=="wrong-page")controller.frame.preparation=false;
        if(mode=="user-stop")controller.stopped=true;
        if(mode=="combat"){controller.frame.pending=false;plugin.m_cur_deployment_opers.clear();}
        const bool result=plugin.attempt(mode!="combat");
        bool ok=false;
        if(mode.starts_with("co-op-"))ok=result&&controller.frame.count==(mode=="co-op-first"?1:2)&&plugin.m_used_tiles.size()==controller.frame.count&&controller.swipes==1&&controller.cancels==0;
        else if(mode=="pending-direction"||mode=="lost-direction-once")ok=result&&controller.frame.count==1&&plugin.m_used_tiles.size()==1&&controller.swipes==(mode=="pending-direction"?1:2)&&controller.cancels==0&&controller.last_end.y<399;
        else if(mode=="cancel-and-retry")ok=result&&controller.frame.count==0&&plugin.m_used_tiles.empty()&&plugin.m_battlefield_opers.empty()&&controller.cancels==1;
        else if(mode=="already-deployed"||mode=="combat")ok=result&&plugin.m_used_tiles.size()==1&&controller.swipes==0&&controller.cancels==0;
        else ok=!result&&controller.swipes<=2&&controller.cancels<=2;
        if(mode=="unknown-frame"||mode=="wrong-count"||mode=="wrong-page"||mode=="user-stop")ok=ok&&controller.swipes==0&&controller.cancels==0;
        std::cout<<(ok?"PASS ":"FAIL ")<<mode<<" result="<<result<<" swipes="<<controller.swipes<<" cancels="<<controller.cancels<<" used="<<plugin.m_used_tiles.size()<<'\n';failures+=!ok;
    }
    for(const std::string text:{"", "1", "1/0", "2/1", "1/2/3", "1/1 trailing", "999999999999999999/1", "-1/1"}){
        const bool ok=!asst::blackflow::parse_preparation_deployment_count(text).has_value();
        std::cout<<(ok?"PASS ":"FAIL ")<<"reject count="<<text<<'\n';failures+=!ok;
    }
    return failures?1:0;
}
'''
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        raise RuntimeError("A C++20 compiler is required")
    with tempfile.TemporaryDirectory(prefix="maa-preparation-") as directory:
        cpp = Path(directory) / "replay.cpp"
        exe = cpp.with_suffix(".exe")
        cpp.write_text(harness, "utf-8")
        subprocess.run([compiler, "-std=c++20", str(cpp), "-o", str(exe)], check=True)
        return subprocess.run([str(exe)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
