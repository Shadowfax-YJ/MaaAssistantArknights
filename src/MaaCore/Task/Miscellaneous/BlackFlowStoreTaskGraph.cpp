#include "BlackFlowStoreTaskGraph.h"

#ifdef ASST_BUILD_SMOKE_TEST

#include "BlackFlowStoreOrchestrator.hpp"
#include "Config/TaskData.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <string_view>

namespace
{
// Independent TaskData oracle from MaaRelease 9a526b5e4a; only the passive InvestSystem seam differs.
constexpr std::string_view Begin = "MiniGame@BlackFlow@Begin";
constexpr std::string_view StartExplore = "MiniGame@BlackFlow@StartExplore";
constexpr std::string_view SpecialMissionVideo = "MiniGame@BlackFlow@SpecialMissionVideo";
constexpr std::string_view ChooseSpecialSquad = "MiniGame@BlackFlow@ChooseSpecialSquad";
constexpr std::string_view ChooseSpecialSquadConfirm = "MiniGame@BlackFlow@ChooseSpecialSquad-Confirm";
constexpr std::string_view ChooseStrategy = "MiniGame@BlackFlow@ChooseStrategy";
constexpr std::string_view ChooseStrategyConfirm = "MiniGame@BlackFlow@ChooseStrategy-Confirm";
constexpr std::string_view ChooseRecruitVoucher = "MiniGame@BlackFlow@ChooseRecruitVoucher";
constexpr std::string_view ChooseMechanic = "MiniGame@BlackFlow@ChooseMechanic";
constexpr std::string_view ConfirmRecruit = "MiniGame@BlackFlow@ConfirmRecruit";
constexpr std::string_view WaitInitialRecruit = "MiniGame@BlackFlow@WaitInitialRecruit";
constexpr std::string_view InitialRecruit = "MiniGame@BlackFlow@InitialRecruit";
constexpr std::string_view ChooseCasterVoucher = "MiniGame@BlackFlow@ChooseCasterVoucher";
constexpr std::string_view GiveUpRecruit = "MiniGame@BlackFlow@GiveUpRecruit";
constexpr std::string_view GiveUpRecruitConfirm = "MiniGame@BlackFlow@GiveUpRecruitConfirm";
constexpr std::string_view ChooseSniperVoucher = "MiniGame@BlackFlow@ChooseSniperVoucher";
constexpr std::string_view GiveUpSniperRecruit = "MiniGame@BlackFlow@GiveUpSniperRecruit";
constexpr std::string_view GiveUpSniperRecruitConfirm = "MiniGame@BlackFlow@GiveUpSniperRecruitConfirm";
constexpr std::string_view ChooseTreeSea = "MiniGame@BlackFlow@ChooseTreeSea";
constexpr std::string_view ActionPoint = "MiniGame@BlackFlow@ActionPoint";
constexpr std::string_view StructuralPrinciple = "MiniGame@BlackFlow@StructuralPrinciple";
constexpr std::string_view BolivarSkin = "MiniGame@BlackFlow@BolivarSkin";
constexpr std::string_view UnknownMystery = "MiniGame@BlackFlow@UnknownMystery";
constexpr std::string_view DepartFor = "MiniGame@BlackFlow@DepartFor";
constexpr std::string_view InvestSystem = "MiniGame@BlackFlow@InvestSystem";
constexpr std::string_view ExitThenAbandon = "MiniGame@BlackFlow@ExitThenAbandon";
constexpr std::string_view StopAt = "MiniGame@BlackFlow@StopAt";
constexpr std::string_view AbandonConfirm = "MiniGame@BlackFlow@AbandonConfirm";
constexpr std::string_view WaitForExploreRestart = "MiniGame@BlackFlow@WaitForExploreRestart";
constexpr std::string_view WaitInitializing = "MiniGame@BlackFlow@WaitInitializing";
constexpr std::string_view Stop = "Stop";

struct TaskContract
{
    std::string_view name;
    asst::AlgorithmType algorithm;
    asst::ProcessTaskAction action = asst::ProcessTaskAction::DoNothing;
    std::initializer_list<std::string_view> next;
    std::initializer_list<std::string_view> exceeded_next;
    std::string_view repeated_reduce_task = {};
    size_t repeated_reduce_count = 0;
    asst::Rect roi = {};
    asst::Rect specific_rect = {};
    int post_delay = 0;
    int max_times = std::numeric_limits<int>::max();
};

bool task_list_equals(const asst::TaskList& actual, std::initializer_list<std::string_view> expected) noexcept
{
    if (actual.size() != expected.size()) {
        return false;
    }

    return std::equal(actual.begin(), actual.end(), expected.begin(), [](const auto& lhs, std::string_view rhs) {
        return lhs == rhs;
    });
}

bool repeated_task_list_equals(
    const asst::TaskList& actual,
    std::string_view expected_task,
    size_t expected_count) noexcept
{
    return actual.size() == expected_count &&
           std::ranges::all_of(actual, [expected_task](const auto& task) { return task == expected_task; });
}

bool report_contract_failure(std::string_view task_name, std::string_view field)
{
    std::cerr << "BlackFlow MaaRelease outer-graph contract mismatch: " << task_name << " (" << field << ')'
              << std::endl;
    return false;
}

bool require_task(const TaskContract& expected)
{
    const auto actual = asst::Task.get(expected.name);
    if (!actual) {
        std::cerr << "BlackFlow MaaRelease outer-graph node is missing: " << expected.name << std::endl;
        return false;
    }

    if (actual->name != expected.name || actual->algorithm != expected.algorithm || actual->action != expected.action) {
        return report_contract_failure(expected.name, "identity/algorithm/action");
    }
    if (!task_list_equals(actual->next, expected.next) || !actual->sub.empty() || !actual->on_error_next.empty() ||
        !task_list_equals(actual->exceeded_next, expected.exceeded_next) ||
        !repeated_task_list_equals(
            actual->reduce_other_times,
            expected.repeated_reduce_task,
            expected.repeated_reduce_count)) {
        return report_contract_failure(expected.name, "control-flow edges");
    }
    if (actual->sub_error_ignored || actual->max_times != expected.max_times ||
        actual->specific_rect != expected.specific_rect || actual->high_resolution_swipe_fix ||
        actual->pre_delay != 0 || actual->post_delay != expected.post_delay ||
        actual->retry_times != std::numeric_limits<int>::max() || actual->roi != expected.roi ||
        !actual->rect_move.empty() || actual->cache || !actual->special_params.empty() || !actual->input_text.empty()) {
        return report_contract_failure(expected.name, "timing/region/limits");
    }
    return true;
}

bool require_ocr(const TaskContract& expected, std::initializer_list<std::string_view> expected_text)
{
    if (!require_task(expected)) {
        return false;
    }

    const auto actual = asst::Task.get<asst::OcrTaskInfo>(expected.name);
    if (!actual || !task_list_equals(actual->text, expected_text) || actual->full_match || actual->is_ascii ||
        actual->without_det || actual->replace_full || !actual->use_raw || !actual->replace_map.empty() ||
        actual->bin_threshold != std::array<int, 2> { 140, 255 }) {
        return report_contract_failure(expected.name, "OCR recognition");
    }
    return true;
}

bool require_match(const TaskContract& expected, std::initializer_list<std::string_view> expected_templates)
{
    if (!require_task(expected)) {
        return false;
    }

    const auto actual = asst::Task.get<asst::MatchTaskInfo>(expected.name);
    if (!actual || !task_list_equals(actual->templ_names, expected_templates) ||
        actual->templ_thresholds.size() != expected_templates.size() ||
        !std::ranges::all_of(
            actual->templ_thresholds,
            [](double value) { return value == asst::TemplThresholdDefault; }) ||
        actual->methods.size() != expected_templates.size() ||
        !std::ranges::all_of(actual->methods, [](auto method) { return method == asst::MatchMethod::Ccoeff; }) ||
        !actual->mask_ranges.empty() || !actual->color_scales.empty() || !actual->color_close || actual->pure_color) {
        return report_contract_failure(expected.name, "template recognition");
    }
    return true;
}

bool require_maarelease_outer_graph()
{
    using asst::AlgorithmType;
    using asst::ProcessTaskAction;

    return require_task(TaskContract {
               .name = Begin,
               .algorithm = AlgorithmType::JustReturn,
               .next = { StartExplore, StopAt },
           }) &&
           require_ocr(
               TaskContract {
                   .name = StartExplore,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { SpecialMissionVideo, ChooseSpecialSquad, StartExplore },
                   .repeated_reduce_task = WaitForExploreRestart,
                   .repeated_reduce_count = 20,
                   .roi = { 1000, 560, 200, 100 },
               },
               { "开始探索" }) &&
           require_ocr(
               TaskContract {
                   .name = SpecialMissionVideo,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickRect,
                   .next = { ChooseSpecialSquad, SpecialMissionVideo },
                   .roi = { 560, 300, 170, 50 },
                   .specific_rect = { 640, 655, 1, 1 },
                   .post_delay = 500,
               },
               { "特勤任务影像" }) &&
           require_ocr(
               TaskContract {
                   .name = ChooseSpecialSquad,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = {
                       ChooseSpecialSquadConfirm,
                       ChooseSpecialSquad,
                       ChooseStrategy,
                       ChooseRecruitVoucher,
                       ChooseMechanic,
                   },
                   .roi = { 0, 385, 1280, 110 },
               },
               { "特勤分队" }) &&
           require_ocr(
               TaskContract {
                   .name = ChooseSpecialSquadConfirm,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { ChooseStrategy, ChooseSpecialSquad, ChooseRecruitVoucher, ChooseMechanic },
                   .roi = { 380, 535, 150, 30 },
                   .post_delay = 1000,
               },
               { "你确定要这么做" }) &&
           require_ocr(
               TaskContract {
                   .name = ChooseStrategy,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { ChooseStrategyConfirm, ChooseStrategy, ChooseRecruitVoucher, ChooseMechanic },
                   .roi = { 0, 385, 1280, 110 },
               },
               { "稳扎稳打" }) &&
           require_ocr(
               TaskContract {
                   .name = ChooseStrategyConfirm,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { ChooseRecruitVoucher, ChooseStrategy, ChooseMechanic },
                   .roi = { 380, 535, 150, 30 },
                   .post_delay = 1000,
               },
               { "你确定要这么做" }) &&
           require_ocr(
               TaskContract {
                   .name = ChooseRecruitVoucher,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { ChooseMechanic, ChooseRecruitVoucher },
                   .roi = { 270, 300, 160, 50 },
               },
               { "重装招募券" }) &&
           require_ocr(
               TaskContract {
                   .name = ChooseMechanic,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { ConfirmRecruit, ChooseMechanic },
                   .roi = { 610, 0, 80, 720 },
               },
               { "机械师" }) &&
           require_ocr(
               TaskContract {
                   .name = ConfirmRecruit,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { ConfirmRecruit, WaitInitialRecruit },
                   .roi = { 1080, 650, 150, 50 },
               },
               { "确认招募" }) &&
           require_task(TaskContract {
               .name = WaitInitialRecruit,
               .algorithm = AlgorithmType::JustReturn,
               .action = ProcessTaskAction::ClickRect,
               .next = { InitialRecruit, WaitInitialRecruit },
               .specific_rect = { 670, 40, 1, 1 },
           }) &&
           require_ocr(
               TaskContract {
                   .name = InitialRecruit,
                   .algorithm = AlgorithmType::OcrDetect,
                   .next = { ChooseCasterVoucher, InitialRecruit },
                   .roi = { 610, 0, 120, 60 },
               },
               { "初始招募" }) &&
           require_ocr(
               TaskContract {
                   .name = ChooseCasterVoucher,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { GiveUpRecruit, ChooseCasterVoucher },
                   .roi = { 570, 300, 160, 50 },
               },
               { "术师招募券" }) &&
           require_ocr(
               TaskContract {
                   .name = GiveUpRecruit,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { GiveUpRecruitConfirm, GiveUpRecruit },
                   .roi = { 930, 650, 80, 50 },
               },
               { "放弃" }) &&
           require_match(
               TaskContract {
                   .name = GiveUpRecruitConfirm,
                   .algorithm = AlgorithmType::MatchTemplate,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { ChooseSniperVoucher, GiveUpRecruitConfirm },
                   .roi = { 613, 410, 424, 171 },
               },
               { "Roguelike@StageEncounterGiveUpRecruitConfirm.png" }) &&
           require_ocr(
               TaskContract {
                   .name = ChooseSniperVoucher,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { GiveUpSniperRecruit, ChooseSniperVoucher },
                   .roi = { 850, 300, 160, 50 },
               },
               { "狙击招募券" }) &&
           require_ocr(
               TaskContract {
                   .name = GiveUpSniperRecruit,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { GiveUpSniperRecruitConfirm, GiveUpSniperRecruit },
                   .roi = { 930, 650, 80, 50 },
               },
               { "放弃" }) &&
           require_match(
               TaskContract {
                   .name = GiveUpSniperRecruitConfirm,
                   .algorithm = AlgorithmType::MatchTemplate,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { ChooseTreeSea, GiveUpSniperRecruitConfirm },
                   .roi = { 613, 410, 424, 171 },
               },
               { "Roguelike@StageEncounterGiveUpRecruitConfirm.png" }) &&
           require_ocr(
               TaskContract {
                   .name = ChooseTreeSea,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { ActionPoint, ChooseTreeSea },
                   .roi = { 1060, 395, 160, 55 },
                   .post_delay = 5000,
               },
               { "沉沦于树海" }) &&
           require_ocr(
               TaskContract {
                   .name = ActionPoint,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { StructuralPrinciple, ActionPoint },
                   .roi = { 1125, 70, 60, 40 },
               },
               { "行动力", "行动", "动力" }) &&
           require_ocr(
               TaskContract {
                   .name = StructuralPrinciple,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { BolivarSkin, StructuralPrinciple },
                   .roi = { 1130, 200, 125, 500 },
               },
               { "结构性原理" }) &&
           require_ocr(
               TaskContract {
                   .name = BolivarSkin,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { UnknownMystery, ExitThenAbandon },
                   .roi = { 555, 0, 250, 35 },
                   .post_delay = 1000,
               },
               { "玻利", "瓦尔", "肤层" }) &&
           require_ocr(
               TaskContract {
                   .name = UnknownMystery,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { DepartFor, ExitThenAbandon },
                   .roi = { 888, 380, 200, 45 },
               },
               { "未知的诡秘" }) &&
           require_ocr(
               TaskContract {
                   .name = DepartFor,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { InvestSystem, ExitThenAbandon },
                   .roi = { 1140, 520, 120, 55 },
                   .post_delay = 2000,
               },
               { "出发前往" }) &&
           require_ocr(
               TaskContract {
                   .name = InvestSystem,
                   .algorithm = AlgorithmType::OcrDetect,
                   .next = { ExitThenAbandon, InvestSystem },
                   .roi = { 420, 170, 170, 40 },
               },
               { "投资", "系统" }) &&
           require_match(
               TaskContract {
                   .name = ExitThenAbandon,
                   .algorithm = AlgorithmType::MatchTemplate,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { StopAt, ExitThenAbandon },
                   .roi = { 0, 0, 90, 90 },
               },
               { "Roguelike@ExitThenAbandon.png" }) &&
           require_ocr(
               TaskContract {
                   .name = StopAt,
                   .algorithm = AlgorithmType::OcrDetect,
                   .action = ProcessTaskAction::ClickRect,
                   .next = { AbandonConfirm, StopAt },
                   .roi = { 1010, 420, 90, 40 },
                   .specific_rect = { 1140, 325, 1, 1 },
               },
               { "驻足于" }) &&
           require_match(
               TaskContract {
                   .name = AbandonConfirm,
                   .algorithm = AlgorithmType::MatchTemplate,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { AbandonConfirm, WaitForExploreRestart },
                   .roi = { 0, 0, 1280, 720 },
               },
               { "Roguelike@AbandonConfirm.png" }) &&
           require_task(TaskContract {
               .name = WaitForExploreRestart,
               .algorithm = AlgorithmType::JustReturn,
               .action = ProcessTaskAction::ClickRect,
               .next = { StartExplore, WaitInitializing, WaitForExploreRestart },
               .exceeded_next = { Stop },
               .specific_rect = { 600, 610, 1, 1 },
               .post_delay = 1500,
               .max_times = 10,
           }) &&
           require_ocr(
               TaskContract {
                   .name = WaitInitializing,
                   .algorithm = AlgorithmType::OcrDetect,
                   .next = { WaitInitializing, StartExplore, WaitForExploreRestart },
                   .roi = { 1075, 590, 120, 45 },
                   .post_delay = 1000,
               },
               { "正在初始化" });
}

bool require_store_command_roots()
{
    using asst::AlgorithmType;
    using asst::ProcessTaskAction;
    using namespace asst::black_flow_store_tasks;

    constexpr std::string_view RefreshTemplate = "Roguelike@StageTraderRefresh.png";
    const std::initializer_list<std::string_view> RefreshDialogTemplates {
        "Roguelike@StageTraderRefreshWithDiceConfirm.png",
        "Roguelike@StageTraderRefreshConfirmNew.png",
        "Roguelike@StageTraderRefreshConfirm.png",
    };

    // Page counting belongs to the C++ collector; these exact roots contain no page-specific TaskData branch.
    return require_task(
               TaskContract {
                   .name = ObserveStorePage,
                   .algorithm = AlgorithmType::JustReturn,
                   .next = { StorePageReady },
               }) &&
           require_match(
               TaskContract {
                   .name = StorePageReady,
                   .algorithm = AlgorithmType::MatchTemplate,
                   .roi = { 1046, 443, 234, 204 },
               },
               { "Roguelike@StageTraderLeave.png" }) &&
           require_match(
               TaskContract {
                   .name = OpenRefreshDialog,
                   .algorithm = AlgorithmType::MatchTemplate,
                   .action = ProcessTaskAction::ClickSelf,
                   .next = { RefreshDialogVisible },
                   .roi = { 230, 47, 211, 182 },
                   .post_delay = 500,
                   .max_times = 1,
               },
               { RefreshTemplate }) &&
           require_match(
               TaskContract {
                   .name = RefreshDialogVisible,
                   .algorithm = AlgorithmType::MatchTemplate,
                   .roi = { 580, 375, 700, 230 },
               },
               RefreshDialogTemplates) &&
           require_match(
               TaskContract {
                   .name = ConfirmRefresh,
                   .algorithm = AlgorithmType::MatchTemplate,
                   .action = ProcessTaskAction::ClickSelf,
                   .roi = { 580, 375, 700, 230 },
                   .post_delay = 500,
                   .max_times = 1,
               },
               RefreshDialogTemplates);
}
} // namespace

bool asst::run_black_flow_store_task_graph_smoke_test()
{
    if (!require_maarelease_outer_graph() || !require_store_command_roots()) {
        return false;
    }

    std::cout << "BlackFlow TaskData seam gate: MaaRelease outer nodes=30, store command roots=3, "
                 "TaskData page branches=0"
              << std::endl;
    return true;
}
#endif
