#include "BlackFlowStoreTaskGraph.h"

#ifdef ASST_BUILD_SMOKE_TEST

#include "BlackFlowStoreOrchestrator.hpp"
#include "Config/TaskData.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace
{
constexpr std::string_view StablePrefix = "MiniGame@BlackFlow@";
constexpr std::string_view Begin = "MiniGame@BlackFlow@Begin";
constexpr std::string_view InitialRecruitSkipVisible = "MiniGame@BlackFlow@InitialRecruitSkipVisible";
constexpr std::string_view AdvanceInitialRecruitPortrait = "MiniGame@BlackFlow@AdvanceInitialRecruitPortrait";
constexpr std::string_view InitialRecruitVisible = "MiniGame@BlackFlow@InitialRecruitVisible";
constexpr std::string_view WaitInitialRecruit = "MiniGame@BlackFlow@WaitInitialRecruit";
constexpr std::string_view RefreshControlVisible = "MiniGame@BlackFlow@RefreshControlVisible";
constexpr std::string_view DismissRefreshDialog = "MiniGame@BlackFlow@DismissRefreshDialog";
constexpr std::string_view LeaveStore = "MiniGame@BlackFlow@LeaveStore";
constexpr std::string_view LeaveStoreConfirm = "MiniGame@BlackFlow@LeaveStoreConfirm";
constexpr std::string_view ExitExploration = "MiniGame@BlackFlow@ExitExploration";
constexpr std::string_view AbandonEntryVisible = "MiniGame@BlackFlow@AbandonEntryVisible";
constexpr std::string_view AbandonEntry = "MiniGame@BlackFlow@AbandonEntry";
constexpr std::string_view AbandonEntryFallback = "MiniGame@BlackFlow@AbandonEntryFallback";
constexpr std::string_view AbandonConfirmVisible = "MiniGame@BlackFlow@AbandonConfirmVisible";
constexpr std::string_view AbandonConfirm = "MiniGame@BlackFlow@AbandonConfirm";
constexpr std::string_view ContinueAfterAbandon = "MiniGame@BlackFlow@ContinueAfterAbandon";
constexpr std::string_view WaitAfterAbandonInitializing = "MiniGame@BlackFlow@WaitAfterAbandonInitializing";
constexpr std::string_view RetryAfterAbandonDelay = "MiniGame@BlackFlow@RetryAfterAbandonDelay";
constexpr std::string_view ContinueSafeExit = "MiniGame@BlackFlow@ContinueSafeExit";
constexpr std::string_view WaitForExploreEntry = "MiniGame@BlackFlow@WaitForExploreEntry";
constexpr std::string_view SafeExitFailed = "MiniGame@BlackFlow@SafeExitFailed";
constexpr std::array WaitingTasks {
    WaitInitialRecruit,
    std::string_view { "MiniGame@BlackFlow@WaitInitializing" },
    WaitAfterAbandonInitializing,
    RetryAfterAbandonDelay,
    ContinueAfterAbandon,
    WaitForExploreEntry,
    ContinueSafeExit,
};
constexpr std::array StateChangingSafeExitTasks {
    DismissRefreshDialog, LeaveStore,           LeaveStoreConfirm, ExitExploration,
    AbandonEntry,         AbandonEntryFallback, AbandonConfirm,
};

bool has_stable_name(std::string_view task_name) noexcept
{
    return task_name.starts_with(StablePrefix);
}

bool has_investment_name(std::string_view task_name) noexcept
{
    return task_name.find("Invest") != std::string_view::npos || task_name.find("Investment") != std::string_view::npos;
}

template <typename Visitor>
void visit_control_flow_edges(const asst::TaskInfo& task, Visitor&& visitor)
{
    for (const auto* edges : { &task.next, &task.sub, &task.on_error_next, &task.exceeded_next }) {
        for (const auto& edge : *edges) {
            visitor(edge);
        }
    }
}

bool has_no_task_references(const asst::TaskInfo& task) noexcept
{
    return task.next.empty() && task.sub.empty() && task.on_error_next.empty() && task.exceeded_next.empty() &&
           task.reduce_other_times.empty();
}

bool has_allowed_action(asst::ProcessTaskAction action) noexcept
{
    return action == asst::ProcessTaskAction::DoNothing || action == asst::ProcessTaskAction::ClickSelf ||
           action == asst::ProcessTaskAction::ClickRect;
}

bool is_recognition_algorithm(asst::AlgorithmType algorithm) noexcept
{
    return algorithm == asst::AlgorithmType::MatchTemplate || algorithm == asst::AlgorithmType::OcrDetect;
}

bool is_waiting_task(std::string_view task_name) noexcept
{
    for (const std::string_view waiting_task : WaitingTasks) {
        if (task_name == waiting_task) {
            return true;
        }
    }
    return false;
}

bool has_safe_exit_transition_barrier(std::string_view task_name)
{
    constexpr int MinimumTransitionDelay = 500;
    const auto task = asst::Task.get(task_name);
    if (!task) {
        return false;
    }
    if (task->post_delay >= MinimumTransitionDelay) {
        return true;
    }
    if (task->next.empty() || task->next.front() == ContinueSafeExit) {
        return false;
    }

    const auto successor = asst::Task.get(task->next.front());
    return successor && successor->action == asst::ProcessTaskAction::DoNothing &&
           (successor->pre_delay >= MinimumTransitionDelay || successor->post_delay >= MinimumTransitionDelay ||
            is_recognition_algorithm(successor->algorithm));
}

bool control_flow_reaches(std::string_view root, std::string_view target)
{
    std::queue<std::string> pending;
    pending.emplace(root);
    std::unordered_set<std::string> visited;

    while (!pending.empty()) {
        auto name = std::move(pending.front());
        pending.pop();
        if (!visited.emplace(name).second) {
            continue;
        }
        if (name == target) {
            return true;
        }

        const auto task = asst::Task.get(name);
        if (!task) {
            continue;
        }
        visit_control_flow_edges(*task, [&](const std::string& edge) { pending.push(edge); });
    }

    return false;
}

bool has_explicit_recruit_fallback_exhaustion(const asst::TaskInfo& fallback, std::string_view terminal)
{
    if (fallback.exceeded_next.empty()) {
        return false;
    }

    bool reaches_terminal = false;
    for (const auto& edge : fallback.exceeded_next) {
        const auto task = asst::Task.get(edge);
        if (!task || !is_recognition_algorithm(task->algorithm)) {
            return false;
        }
        reaches_terminal = reaches_terminal || control_flow_reaches(edge, terminal);
    }
    return reaches_terminal;
}

struct FactContract
{
    std::string_view name;
    asst::AlgorithmType algorithm;
};

constexpr std::array CoreFacts {
    FactContract { InitialRecruitVisible, asst::AlgorithmType::OcrDetect },
    FactContract { RefreshControlVisible, asst::AlgorithmType::MatchTemplate },
    FactContract { AbandonEntryVisible, asst::AlgorithmType::OcrDetect },
    FactContract { AbandonConfirmVisible, asst::AlgorithmType::OcrDetect },
    FactContract { asst::black_flow_store_tasks::StorePageReady, asst::AlgorithmType::MatchTemplate },
    FactContract { asst::black_flow_store_tasks::RefreshDialogVisible, asst::AlgorithmType::MatchTemplate },
    FactContract { "MiniGame@BlackFlow@WaitInitializing", asst::AlgorithmType::OcrDetect },
    FactContract { asst::black_flow_store_tasks::StartExploreEntryVisible, asst::AlgorithmType::OcrDetect },
};

struct RequiredPath
{
    std::string_view root;
    std::string_view terminal;
};
} // namespace

bool asst::run_black_flow_store_task_graph_smoke_test()
{
    constexpr std::array<std::string_view, 19> RequiredTasks {
        Begin,
        InitialRecruitSkipVisible,
        AdvanceInitialRecruitPortrait,
        black_flow_store_tasks::PrepareFreshEntry,
        black_flow_store_tasks::EnterFreshExploration,
        black_flow_store_tasks::StartExploreEntryVisible,
        black_flow_store_tasks::StorePageReady,
        black_flow_store_tasks::ObserveStorePage,
        black_flow_store_tasks::OpenRefreshDialog,
        black_flow_store_tasks::RefreshDialogVisible,
        black_flow_store_tasks::ConfirmRefresh,
        black_flow_store_tasks::SafeExit,
        DismissRefreshDialog,
        AbandonEntry,
        AbandonEntryFallback,
        AbandonConfirm,
        ContinueSafeExit,
        WaitForExploreEntry,
        SafeExitFailed,
    };
    for (const std::string_view name : RequiredTasks) {
        if (!Task.get(name)) {
            std::cerr << "BlackFlow TaskData node is missing: " << name << std::endl;
            return false;
        }
    }

    const auto begin = Task.get(Begin);
    if (begin->algorithm != AlgorithmType::JustReturn || begin->action != ProcessTaskAction::DoNothing ||
        !has_no_task_references(*begin)) {
        std::cerr << "BlackFlow stable entry is not an inert JustReturn leaf" << std::endl;
        return false;
    }

    for (const std::string_view root : {
             black_flow_store_tasks::PrepareFreshEntry,
             black_flow_store_tasks::SafeExit,
         }) {
        const auto task = Task.get(root);
        if (task->algorithm != AlgorithmType::JustReturn || task->action != ProcessTaskAction::DoNothing ||
            task->next.size() != 1 || task->next.front() != ContinueSafeExit) {
            std::cerr << "BlackFlow takeover entry is not an inert handoff to the shared recovery dispatcher: " << root
                      << std::endl;
            return false;
        }
    }

    const auto failed_exit = Task.get(SafeExitFailed);
    if (failed_exit->algorithm != AlgorithmType::JustReturn || failed_exit->action != ProcessTaskAction::DoNothing ||
        !has_no_task_references(*failed_exit)) {
        std::cerr << "BlackFlow safe-exit failure terminal is not an inert leaf" << std::endl;
        return false;
    }

    const auto recruit_fallback = Task.get(AdvanceInitialRecruitPortrait);
    const auto established_recruit_fallback = Task.get("Roguelike@RecruitWithoutButton");
    if (recruit_fallback->algorithm != AlgorithmType::JustReturn ||
        recruit_fallback->action != ProcessTaskAction::ClickRect || recruit_fallback->specific_rect.empty() ||
        recruit_fallback->max_times <= 0 || recruit_fallback->max_times == std::numeric_limits<int>::max() ||
        !established_recruit_fallback ||
        recruit_fallback->specific_rect != established_recruit_fallback->specific_rect ||
        !has_explicit_recruit_fallback_exhaustion(*recruit_fallback, InitialRecruitVisible) ||
        !control_flow_reaches(InitialRecruitSkipVisible, AdvanceInitialRecruitPortrait) ||
        !control_flow_reaches(AdvanceInitialRecruitPortrait, InitialRecruitVisible)) {
        std::cerr << "BlackFlow initial recruitment lacks the established bounded portrait-advance fallback"
                  << std::endl;
        return false;
    }

    const auto established_abandon_entry = Task.get<MatchTaskInfo>("JieGarden@Roguelike@Abandon");
    const auto abandon_entry = Task.get<MatchTaskInfo>(AbandonEntry);
    const auto abandon_entry_fallback = Task.get(AbandonEntryFallback);
    const auto established_abandon_confirm = Task.get<MatchTaskInfo>("Roguelike@AbandonConfirm");
    const auto abandon_confirm = Task.get<MatchTaskInfo>(AbandonConfirm);
    if (!established_abandon_entry || !abandon_entry ||
        abandon_entry->algorithm != established_abandon_entry->algorithm ||
        abandon_entry->action != established_abandon_entry->action ||
        abandon_entry->roi != established_abandon_entry->roi ||
        abandon_entry->templ_names != established_abandon_entry->templ_names ||
        abandon_entry->templ_thresholds != established_abandon_entry->templ_thresholds ||
        abandon_entry->methods != established_abandon_entry->methods ||
        abandon_entry->mask_ranges != established_abandon_entry->mask_ranges ||
        abandon_entry->color_scales != established_abandon_entry->color_scales ||
        abandon_entry->color_close != established_abandon_entry->color_close ||
        abandon_entry->pure_color != established_abandon_entry->pure_color || abandon_entry->max_times != 3 ||
        abandon_entry->post_delay < 500 || abandon_entry->next.size() != 2 ||
        abandon_entry->next[0] != AbandonConfirm || abandon_entry->next[1] != AbandonEntry ||
        !established_abandon_confirm || !abandon_entry_fallback ||
        abandon_entry_fallback->algorithm != AlgorithmType::JustReturn ||
        abandon_entry_fallback->action != ProcessTaskAction::ClickRect ||
        abandon_entry_fallback->specific_rect != Rect { 1140, 325, 1, 1 } || abandon_entry_fallback->max_times != 3 ||
        abandon_entry_fallback->post_delay < 500 || abandon_entry_fallback->next.size() != 2 ||
        abandon_entry_fallback->next[0] != AbandonConfirm || abandon_entry_fallback->next[1] != AbandonEntryFallback ||
        abandon_entry_fallback->exceeded_next.size() != 1 ||
        abandon_entry_fallback->exceeded_next.front() != SafeExitFailed || !abandon_confirm ||
        abandon_confirm->action != established_abandon_confirm->action ||
        abandon_confirm->roi != established_abandon_confirm->roi ||
        abandon_confirm->post_delay != established_abandon_confirm->post_delay ||
        abandon_confirm->templ_names != established_abandon_confirm->templ_names || abandon_confirm->next.empty() ||
        abandon_confirm->next.front() == ContinueSafeExit ||
        !control_flow_reaches(AbandonEntry, black_flow_store_tasks::StartExploreEntryVisible) ||
        control_flow_reaches(AbandonEntry, DismissRefreshDialog) ||
        !control_flow_reaches(AbandonEntryFallback, black_flow_store_tasks::StartExploreEntryVisible) ||
        control_flow_reaches(AbandonEntryFallback, DismissRefreshDialog)) {
        std::cerr << "BlackFlow abandon stages do not combine JieGarden recognition with the guarded fallback"
                  << std::endl;
        return false;
    }

    const auto abandon_entry_fact = Task.get<OcrTaskInfo>(AbandonEntryVisible);
    const auto abandon_confirm_fact = Task.get<OcrTaskInfo>(AbandonConfirmVisible);
    if (!abandon_entry_fact || abandon_entry_fact->action != ProcessTaskAction::DoNothing ||
        std::ranges::find(abandon_entry_fact->text, "驻足于") == abandon_entry_fact->text.end() ||
        abandon_entry_fact->next.size() != 2 || abandon_entry_fact->next[0] != AbandonEntry ||
        abandon_entry_fact->next[1] != AbandonEntryFallback || !abandon_confirm_fact ||
        abandon_confirm_fact->action != ProcessTaskAction::DoNothing ||
        std::ranges::find(abandon_confirm_fact->text, "确认放弃本次探索") == abandon_confirm_fact->text.end() ||
        abandon_confirm_fact->next.size() != 1 || abandon_confirm_fact->next.front() != AbandonConfirm) {
        std::cerr << "BlackFlow abandon actions are not guarded by their stage-specific screen facts" << std::endl;
        return false;
    }

    const auto continue_after_abandon = Task.get(ContinueAfterAbandon);
    const auto wait_after_abandon_initializing = Task.get(WaitAfterAbandonInitializing);
    const auto retry_after_abandon_delay = Task.get(RetryAfterAbandonDelay);
    if (!continue_after_abandon || continue_after_abandon->algorithm != AlgorithmType::JustReturn ||
        continue_after_abandon->action != ProcessTaskAction::DoNothing || continue_after_abandon->max_times <= 0 ||
        continue_after_abandon->max_times == std::numeric_limits<int>::max() || continue_after_abandon->next.empty() ||
        continue_after_abandon->next.front() != black_flow_store_tasks::StartExploreEntryVisible ||
        std::ranges::find(continue_after_abandon->next, AbandonConfirm) == continue_after_abandon->next.end() ||
        control_flow_reaches(ContinueAfterAbandon, DismissRefreshDialog) ||
        continue_after_abandon->exceeded_next.size() != 1 ||
        continue_after_abandon->exceeded_next.front() != SafeExitFailed || !wait_after_abandon_initializing ||
        wait_after_abandon_initializing->action != ProcessTaskAction::DoNothing ||
        wait_after_abandon_initializing->post_delay < 500 || wait_after_abandon_initializing->next.size() != 1 ||
        wait_after_abandon_initializing->next.front() != ContinueAfterAbandon || !retry_after_abandon_delay ||
        retry_after_abandon_delay->algorithm != AlgorithmType::JustReturn ||
        retry_after_abandon_delay->action != ProcessTaskAction::DoNothing ||
        retry_after_abandon_delay->post_delay < 500 || retry_after_abandon_delay->next.size() != 1 ||
        retry_after_abandon_delay->next.front() != ContinueAfterAbandon) {
        std::cerr << "BlackFlow post-abandon polling can re-enter the refresh-dialog recovery stage" << std::endl;
        return false;
    }

    const auto observe_store_page = Task.get(black_flow_store_tasks::ObserveStorePage);
    if (observe_store_page->algorithm != AlgorithmType::JustReturn ||
        observe_store_page->action != ProcessTaskAction::DoNothing || observe_store_page->next.size() != 1 ||
        observe_store_page->next.front() != black_flow_store_tasks::StorePageReady ||
        control_flow_reaches(black_flow_store_tasks::ObserveStorePage, RefreshControlVisible)) {
        std::cerr << "BlackFlow store-page observation is coupled to refresh availability" << std::endl;
        return false;
    }

    const auto open_refresh = Task.get<MatchTaskInfo>(black_flow_store_tasks::OpenRefreshDialog);
    if (!open_refresh || open_refresh->action != ProcessTaskAction::ClickSelf ||
        std::ranges::find(open_refresh->templ_names, "Roguelike@StageTraderRefresh.png") ==
            open_refresh->templ_names.end()) {
        std::cerr << "BlackFlow refresh command does not independently require the available refresh control"
                  << std::endl;
        return false;
    }

    const auto continue_exit = Task.get(ContinueSafeExit);
    const auto abandon_entry_action = std::ranges::find(continue_exit->next, AbandonEntry);
    const auto abandon_entry_fallback_action = std::ranges::find(continue_exit->next, AbandonEntryFallback);
    const auto abandon_confirm_action = std::ranges::find(continue_exit->next, AbandonConfirm);
    const auto abandon_entry_guard = std::ranges::find(continue_exit->next, AbandonEntryVisible);
    const auto abandon_confirm_guard = std::ranges::find(continue_exit->next, AbandonConfirmVisible);
    const auto dismiss_refresh = std::ranges::find(continue_exit->next, DismissRefreshDialog);
    if (continue_exit->algorithm != AlgorithmType::JustReturn ||
        continue_exit->action != ProcessTaskAction::DoNothing || continue_exit->max_times <= 0 ||
        continue_exit->max_times == std::numeric_limits<int>::max() || continue_exit->next.size() < 2 ||
        continue_exit->next[0] != black_flow_store_tasks::StartExploreEntryVisible ||
        abandon_entry_action != continue_exit->next.end() ||
        abandon_entry_fallback_action != continue_exit->next.end() ||
        abandon_confirm_action != continue_exit->next.end() || abandon_entry_guard == continue_exit->next.end() ||
        abandon_confirm_guard == continue_exit->next.end() || dismiss_refresh == continue_exit->next.end() ||
        abandon_confirm_guard > dismiss_refresh || continue_exit->exceeded_next.size() != 1 ||
        continue_exit->exceeded_next.front() != SafeExitFailed) {
        std::cerr << "BlackFlow safe-exit polling exposes abandon actions to the refresh-dialog dispatcher"
                  << std::endl;
        return false;
    }

    for (const std::string_view task_name : StateChangingSafeExitTasks) {
        if (!has_safe_exit_transition_barrier(task_name)) {
            std::cerr << "BlackFlow safe-exit action can immediately redispatch on a stale transition frame: "
                      << task_name << std::endl;
            return false;
        }
    }

    const auto confirm = Task.get(black_flow_store_tasks::ConfirmRefresh);
    if (confirm->algorithm != AlgorithmType::MatchTemplate || confirm->action != ProcessTaskAction::ClickSelf ||
        confirm->max_times != 1 || !has_no_task_references(*confirm)) {
        std::cerr << "BlackFlow refresh confirmation is not a one-shot leaf action" << std::endl;
        return false;
    }

    for (const auto& fact : CoreFacts) {
        const auto task = Task.get(fact.name);
        if (!task || task->algorithm != fact.algorithm || task->action != ProcessTaskAction::DoNothing) {
            std::cerr << "BlackFlow TaskData fact has an invalid algorithm or action: " << fact.name << std::endl;
            return false;
        }
    }

    for (const std::string_view terminal : {
             black_flow_store_tasks::StorePageReady,
             black_flow_store_tasks::RefreshDialogVisible,
             black_flow_store_tasks::StartExploreEntryVisible,
         }) {
        if (!has_no_task_references(*Task.get(terminal))) {
            std::cerr << "BlackFlow TaskData success terminal is not a leaf: " << terminal << std::endl;
            return false;
        }
    }

    constexpr std::array<std::string_view, 7> Roots {
        Begin,
        black_flow_store_tasks::PrepareFreshEntry,
        black_flow_store_tasks::EnterFreshExploration,
        black_flow_store_tasks::ObserveStorePage,
        black_flow_store_tasks::OpenRefreshDialog,
        black_flow_store_tasks::ConfirmRefresh,
        black_flow_store_tasks::SafeExit,
    };
    std::queue<std::string> pending;
    for (const std::string_view root : Roots) {
        pending.emplace(root);
    }

    std::unordered_set<std::string> visited;
    while (!pending.empty()) {
        auto name = std::move(pending.front());
        pending.pop();
        if (!visited.emplace(name).second) {
            continue;
        }
        if (!has_stable_name(name) || has_investment_name(name)) {
            std::cerr << "BlackFlow TaskData reaches a forbidden node: " << name << std::endl;
            return false;
        }
        const auto task = Task.get(name);
        if (!task) {
            std::cerr << "BlackFlow TaskData edge targets a missing node: " << name << std::endl;
            return false;
        }
        if (!has_allowed_action(task->action)) {
            std::cerr << "BlackFlow TaskData contains a non-short action: " << name << std::endl;
            return false;
        }
        if (task->action == ProcessTaskAction::ClickRect && !is_recognition_algorithm(task->algorithm) &&
            name != AdvanceInitialRecruitPortrait && name != AbandonEntryFallback) {
            std::cerr << "BlackFlow TaskData contains an unrecognized fixed-coordinate click: " << name << std::endl;
            return false;
        }
        if (is_waiting_task(name) && task->action != ProcessTaskAction::DoNothing) {
            std::cerr << "BlackFlow TaskData waiting node performs an action: " << name << std::endl;
            return false;
        }
        bool valid_edges = true;
        visit_control_flow_edges(*task, [&](const std::string& edge) {
            if (!has_stable_name(edge) || has_investment_name(edge)) {
                std::cerr << "BlackFlow TaskData contains a forbidden edge: " << name << " -> " << edge << std::endl;
                valid_edges = false;
                return;
            }
            pending.push(edge);
        });
        for (const auto& reference : task->reduce_other_times) {
            if (!has_stable_name(reference) || has_investment_name(reference)) {
                std::cerr << "BlackFlow TaskData contains a forbidden reduceOtherTimes reference: " << name << " -> "
                          << reference << std::endl;
                valid_edges = false;
            }
        }
        if (!valid_edges) {
            return false;
        }
    }

    constexpr std::array ExpectedPaths {
        RequiredPath {
            black_flow_store_tasks::PrepareFreshEntry,
            black_flow_store_tasks::StartExploreEntryVisible,
        },
        RequiredPath { black_flow_store_tasks::EnterFreshExploration, black_flow_store_tasks::StorePageReady },
        RequiredPath { black_flow_store_tasks::ObserveStorePage, black_flow_store_tasks::StorePageReady },
        RequiredPath { black_flow_store_tasks::OpenRefreshDialog, black_flow_store_tasks::RefreshDialogVisible },
        RequiredPath { DismissRefreshDialog, black_flow_store_tasks::StartExploreEntryVisible },
        RequiredPath { black_flow_store_tasks::SafeExit, black_flow_store_tasks::StartExploreEntryVisible },
    };
    for (const auto& path : ExpectedPaths) {
        if (!control_flow_reaches(path.root, path.terminal)) {
            std::cerr << "BlackFlow TaskData subgraph does not reach its success terminal: " << path.root << " -> "
                      << path.terminal << std::endl;
            return false;
        }
    }

    std::cout << "BlackFlow TaskData gate: reachable_nodes=" << visited.size() << ", investment_edges=0" << std::endl;
    return true;
}
#endif
