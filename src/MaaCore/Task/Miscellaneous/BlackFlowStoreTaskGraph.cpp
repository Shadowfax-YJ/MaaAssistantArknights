#include "BlackFlowStoreTaskGraph.h"

#ifdef ASST_BUILD_SMOKE_TEST

#include "BlackFlowStoreOrchestrator.hpp"
#include "Config/TaskData.h"

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
constexpr std::string_view DismissRefreshDialog = "MiniGame@BlackFlow@DismissRefreshDialog";
constexpr std::string_view ContinueSafeExit = "MiniGame@BlackFlow@ContinueSafeExit";
constexpr std::string_view WaitForExploreEntry = "MiniGame@BlackFlow@WaitForExploreEntry";
constexpr std::string_view SafeExitFailed = "MiniGame@BlackFlow@SafeExitFailed";
constexpr std::array WaitingTasks {
    std::string_view { "MiniGame@BlackFlow@WaitInitialRecruit" },
    std::string_view { "MiniGame@BlackFlow@WaitInitializing" },
    WaitForExploreEntry,
    ContinueSafeExit,
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

struct FactContract
{
    std::string_view name;
    asst::AlgorithmType algorithm;
};

constexpr std::array CoreFacts {
    FactContract { "MiniGame@BlackFlow@InitialRecruitVisible", asst::AlgorithmType::OcrDetect },
    FactContract { "MiniGame@BlackFlow@RefreshControlVisible", asst::AlgorithmType::MatchTemplate },
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
    constexpr std::array<std::string_view, 14> RequiredTasks {
        Begin,
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

    const auto continue_exit = Task.get(ContinueSafeExit);
    if (continue_exit->algorithm != AlgorithmType::JustReturn ||
        continue_exit->action != ProcessTaskAction::DoNothing || continue_exit->max_times <= 0 ||
        continue_exit->max_times == std::numeric_limits<int>::max() || continue_exit->next.size() < 2 ||
        continue_exit->next[0] != black_flow_store_tasks::StartExploreEntryVisible ||
        continue_exit->next[1] != DismissRefreshDialog || continue_exit->exceeded_next.size() != 1 ||
        continue_exit->exceeded_next.front() != SafeExitFailed) {
        std::cerr << "BlackFlow safe-exit polling does not prioritize facts or reach its inert bound" << std::endl;
        return false;
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
        if (task->action == ProcessTaskAction::ClickRect && !is_recognition_algorithm(task->algorithm)) {
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
