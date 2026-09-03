#include "BlackFlowLifecycleTaskPlugin.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

#include "Config/TaskData.h"

#include "BlackFlowAutomationCollectionRules.h"
#include "Utils/Logger.hpp"

namespace asst::blackflow
{
namespace
{
constexpr std::string_view RecoveryFailedTask = "BlackFlow@Roguelike@RecoveryFailed";
constexpr std::string_view RecoverMapFailedTask = "BlackFlow@Roguelike@RecoverMapFailed";
constexpr std::string_view RecoveryRetryWaitTask = "BlackFlow@Roguelike@RecoveryRetryWait";
constexpr std::string_view RecoveryRetryActionTask = "BlackFlow@Roguelike@RecoveryRetryAction";
constexpr std::string_view StrategyTerminatedTask = "BlackFlow@Roguelike@StrategyTerminated";

bool is_retryable_recovery_target(std::string_view task)
{
    return !task.empty() && task != RecoveryFailedTask && task != RecoverMapFailedTask &&
           task != RecoveryRetryWaitTask && task != RecoveryRetryActionTask && task != StrategyTerminatedTask;
}

bool configure_recovery_retry_action(const std::string& target)
{
    std::vector<std::string> reduce_other_times;
    std::string successor(StrategyTerminatedTask);
    if (is_retryable_recovery_target(target)) {
        const auto task = Task.get(target);
        if (task == nullptr) {
            Log.error("BlackFlow recovery retry target does not exist", target);
            return false;
        }
        successor = target;
        if (task->max_times > 0 && task->max_times < std::numeric_limits<int>::max()) {
            reduce_other_times.emplace_back(target + "*" + std::to_string(task->max_times));
        }
    }

    json::value retry = json::object {};
    retry["next"] = json::array { std::move(successor) };
    retry["reduceOtherTimes"] = json::array(reduce_other_times);
    json::value patch = json::object {};
    patch[std::string(RecoveryRetryActionTask)] = std::move(retry);
    return Task.lazy_parse(patch);
}

std::pair<std::string, std::string> classify_unreported_termination(const std::string& pre_task)
{
    if (pre_task == RecoveryFailedTask) {
        return { "state_machine_dead_end", "task chain reached RecoveryFailed without a strategy result" };
    }
    if (pre_task == RecoverMapFailedTask) {
        return { "map_recovery_exhausted", "map recovery was exhausted without a strategy result" };
    }
    return { "internal_failure", "terminal action was requested without a strategy result" };
}
} // namespace

bool BlackFlowLifecycleTaskPlugin::load_params(const json::value& params)
{
    if (!BlackFlowTaskPluginBase::load_params(params)) {
        return false;
    }
    m_start_explore_seen = false;

    std::string profile = params.get("blackflow_strategy", std::string {});
    if (profile.empty()) {
        if (m_config->get_mode() == RoguelikeMode::Investment) {
            profile = "investment";
        }
        else {
            profile = params.get("investment_enabled", true) ? "burn_with_investment" : "burn";
        }
    }
    const std::string selected_profile = profile;
    if (selected_profile == "baby_animal") {
        const std::string target_text = params.get("blackflow_cultivation_target", std::string("swaddled_cat"));
        const auto target = parse_cultivated_animal_type(target_text);
        if (!target.has_value()) {
            Log.error("Invalid BlackFlow cultivation target:", target_text);
            return false;
        }
        m_session->set_cultivation_target(*target);
    }

    const bool automation_collection = selected_profile == "automation_collection";

    // 三项都直接读 params：分队要等真正在选择界面点中才会写回 RoguelikeConfig，此刻取不到。
    // 必须先于 initialize()，事实是在它末尾写入的。
    m_session->set_start_loadout(
        automation_collection ? std::string(AutomationCollectionCoreOperator)
                              : params.get("core_char", std::string()),
        automation_collection ? std::string(AutomationCollectionSquad) : params.get("squad", std::string()),
        automation_collection ? std::string(AutomationCollectionRoles) : params.get("roles", std::string()));

    std::string error;
    if (!m_session->initialize(std::move(profile), &error)) {
        Log.error("BlackFlow strategy initialization failed:", error);
        return false;
    }
    if (!configure_recovery_retry_action({})) {
        Log.error("Failed to reset BlackFlow recovery retry action");
        return false;
    }

    const std::string diagnostics_text =
        automation_collection ? "full" : params.get("blackflow_diagnostics", std::string("normal"));
    const auto diagnostics = parse_diagnostic_level(diagnostics_text);
    const int image_limit = automation_collection ? 100 : params.get("blackflow_diagnostic_image_limit", 3);
    if (!diagnostics.has_value() || image_limit < 0 || image_limit > 100) {
        Log.error("Invalid BlackFlow diagnostics parameters");
        return false;
    }
    const DiagnosticSettings settings { *diagnostics, static_cast<std::size_t>(image_limit) };
    if (!m_session->configure_diagnostics(settings, &error)) {
        Log.error("BlackFlow diagnostics initialization failed:", error);
        return false;
    }
    if (m_port != nullptr) {
        m_port->configure_diagnostics(settings);
    }

    record_run_event(
        RunLogLevel::Info,
        "run.started",
        "started",
        "success",
        json::object {
            { "profile", selected_profile },
            { "difficulty", m_config->get_difficulty() },
            { "diagnostics", diagnostics_text },
            { "image_limit", image_limit },
        },
        "BlackFlowLifecycle");

    Log.info("BlackFlow strategy initialized", "profile", selected_profile);
    auto info = basic_info_with_what("BlackFlowStrategyStarted");
    info["details"] = json::object { { "profile", selected_profile } };
    callback(AsstMsg::SubTaskExtraInfo, info);
    return true;
}

bool BlackFlowLifecycleTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }

    const std::string task = details.get("details", "task", "");
    if (msg == AsstMsg::SubTaskStart && (task == RecoveryFailedTask || task == RecoverMapFailedTask)) {
        m_pending = PendingWork::PrepareRecoveryRetry;
        m_pending_details = {};
        m_recovery_retry_target = details.get("pre_task", "");
        m_terminal_trigger.clear();
        m_terminal_pre_task.clear();
        return true;
    }
    if (msg == AsstMsg::SubTaskStart && task == RecoveryRetryWaitTask &&
        details.get("pre_task", "") != RecoveryFailedTask &&
        details.get("pre_task", "") != RecoverMapFailedTask) {
        // 动态动作槽位默认继承 RecoveryFailed；这种路径不会真正执行名为
        // RecoveryFailed 的节点，所以在等待节点补记动作槽位本身。
        m_pending = PendingWork::PrepareRecoveryRetry;
        m_pending_details = {};
        m_recovery_retry_target = details.get("pre_task", "");
        m_terminal_trigger.clear();
        m_terminal_pre_task.clear();
        return true;
    }
    if (msg == AsstMsg::SubTaskCompleted && task == "BlackFlow@Roguelike@NextLevel") {
        m_pending = PendingWork::RecordCurrentFloor;
        m_pending_details = details;
        m_terminal_trigger.clear();
        m_terminal_pre_task.clear();
        return true;
    }
    if (msg == AsstMsg::SubTaskCompleted && task == "BlackFlow@Roguelike@AbandonConfirm") {
        m_pending = PendingWork::ResetAfterAbandon;
        m_pending_details = {};
        m_terminal_trigger.clear();
        m_terminal_pre_task.clear();
        return true;
    }
    if (msg == AsstMsg::SubTaskStart && task == "BlackFlow@Roguelike@HuntedConfirmCompleted") {
        m_pending = PendingWork::ResolveHuntedAction;
        m_pending_details = {};
        m_terminal_trigger.clear();
        m_terminal_pre_task.clear();
        return true;
    }
    if (msg != AsstMsg::SubTaskStart || task != "BlackFlow@Roguelike@StrategyTerminated") {
        return false;
    }

    m_pending = PendingWork::ResolveTerminalAction;
    m_pending_details = {};
    m_terminal_trigger = task;
    m_terminal_pre_task = details.get("pre_task", "");
    return true;
}

void BlackFlowLifecycleTaskPlugin::reset_in_run_variables()
{
    const bool run_has_progress = m_session != nullptr &&
        (m_session->current_floor().has_value() || m_session->run().floor > 0 || m_session->result().has_value());
    const StartExploreRunDisposition disposition =
        start_explore_run_disposition(m_start_explore_seen, run_has_progress);
    m_start_explore_seen = true;
    if (disposition == StartExploreRunDisposition::KeepInitialRun) {
        Log.info("BlackFlow initial StartExplore keeps the initialized run log");
        return;
    }
    finish_current_run(true);
}

void BlackFlowLifecycleTaskPlugin::finish_current_run(bool start_next_run)
{
    m_pending = PendingWork::None;
    m_pending_details = {};
    m_recovery_retry_target.clear();
    m_terminal_trigger.clear();
    m_terminal_pre_task.clear();
    m_stop_after_abandon = false;
    Task.set_task_base("BlackFlow@Roguelike@StrategyTerminalAction", "BlackFlow@Roguelike@ExitThenAbandon-Enter");
    Task.set_task_base("BlackFlow@Roguelike@HuntedAction", "BlackFlow@Roguelike@ExitThenAbandon-Enter");
    if (!configure_recovery_retry_action({})) {
        Log.error("Failed to reset BlackFlow recovery retry action after run completion");
    }
    if (m_port != nullptr) {
        std::string popup_error;
        if (!m_port->flush_pending_collection_popups(&popup_error)) {
            Log.warn("BlackFlow unresolved collection popup flush failed", popup_error);
        }
    }
    if (m_session != nullptr && !m_session->profile().empty()) {
        json::object details;
        if (m_session->result().has_value()) {
            details["result"] = m_session->result()->to_json();
        }
        record_run_event(
            RunLogLevel::Info,
            "run.ended",
            "completed",
            m_session->result().has_value() ? m_session->result()->outcome : "abandoned",
            std::move(details),
            "BlackFlowLifecycle",
            nullptr,
            true);
    }
    if (m_port != nullptr) {
        std::string archive_error;
        if (!m_port->queue_current_run_archive(&archive_error)) {
            Log.error("BlackFlow failed to queue completed run archive", archive_error);
        }
    }
    if (m_port != nullptr) {
        m_port->reset_run();
    }
    if (m_session != nullptr && !m_session->profile().empty()) {
        m_session->reset_run();
        if (start_next_run) {
            record_run_event(
                RunLogLevel::Info,
                "run.started",
                "started",
                "success",
                json::object { { "profile", m_session->profile() } },
                "BlackFlowLifecycle");
        }
    }
}

bool BlackFlowLifecycleTaskPlugin::_run()
{
    const PendingWork work = m_pending;
    const json::value details = std::move(m_pending_details);
    const std::string recovery_retry_target = std::move(m_recovery_retry_target);
    const std::string trigger = std::move(m_terminal_trigger);
    const std::string pre_task = std::move(m_terminal_pre_task);
    m_pending = PendingWork::None;
    m_pending_details = {};
    m_recovery_retry_target.clear();
    m_terminal_trigger.clear();
    m_terminal_pre_task.clear();

    if (work == PendingWork::PrepareRecoveryRetry) {
        if (m_session != nullptr && m_session->terminated()) {
            configure_recovery_retry_action({});
            Log.info(
                "BlackFlow recovery retry skipped because the strategy already has a terminal result",
                recovery_retry_target);
            return true;
        }
        if (!is_retryable_recovery_target(recovery_retry_target)) {
            configure_recovery_retry_action({});
            Log.error("BlackFlow recovery retry has no valid failed action", recovery_retry_target);
            return true;
        }
        if (!configure_recovery_retry_action(recovery_retry_target)) {
            configure_recovery_retry_action({});
            Log.error("BlackFlow failed to configure recovery retry", recovery_retry_target);
            return true;
        }
        Log.warn(
            "BlackFlow transient UI failure will retry the same action after one minute",
            recovery_retry_target);
        record_run_event(
            RunLogLevel::Info,
            "recovery.retry_scheduled",
            "waiting",
            "retry_same_action",
            json::object {
                { "task", recovery_retry_target },
                { "delay_ms", 60'000 },
            },
            "BlackFlowLifecycle");
        return true;
    }
    if (work == PendingWork::RecordCurrentFloor) {
        if (m_session == nullptr) {
            Log.error("BlackFlow floor recognition has no active session");
            return false;
        }
        const std::string area_name = details.get("details", "result", "text", "");
        const auto task = Task.get<OcrTaskInfo>("BlackFlow@Roguelike@NextLevel");
        if (task == nullptr || area_name.empty()) {
            m_session->clear_current_floor();
            m_session->fail(
                "floor_recognition_failed",
                "NextLevel completed without a recognized floor name",
                FailureDisposition::RestartRun);
            Log.error("BlackFlow NextLevel floor recognition result is missing");
            report_outputs();
            return true;
        }
        const auto& floor_names = task->text;
        const auto matched = std::ranges::find(floor_names, area_name);
        if (matched == floor_names.end()) {
            m_session->clear_current_floor();
            m_session->fail(
                "floor_recognition_failed",
                "NextLevel returned an unconfigured floor name: " + area_name,
                FailureDisposition::RestartRun);
            Log.error("BlackFlow NextLevel returned an unconfigured floor name", area_name);
            report_outputs();
            return true;
        }
        const int floor = static_cast<int>(std::distance(floor_names.begin(), matched)) + 1;
        std::string error;
        if (!m_session->set_current_floor(floor, &error)) {
            m_session->fail("floor_recognition_failed", error, FailureDisposition::RestartRun);
            Log.error("BlackFlow NextLevel floor recognition failed", area_name, error);
            report_outputs();
            return true;
        }
        // 追猎战不对应地图事务，胜利后的 NodeCompletionAction 被临时改成了 NextLevel-Enter。
        // 新楼层识别完成后恢复普通节点的默认落点，避免影响后续战斗结算。
        Task.set_task_base("BlackFlow@Roguelike@NodeCompletionAction", "BlackFlow@Roguelike@MapPrepare");
        Log.info("BlackFlow current floor recognized", "floor", floor, "area", area_name);
        report_outputs();
        return true;
    }
    if (work == PendingWork::ResetAfterAbandon) {
        const bool stop_after_abandon = m_stop_after_abandon;
        if (abandon_reset_disposition(stop_after_abandon) == AbandonResetDisposition::DeferUntilStartExplore) {
            // AbandonConfirm 后还要先经过失败结算页；此时提前 reset 会把该结算页记成一局新的探索，
            // 随后的 StartExplore 又会由 RoguelikeResetTaskPlugin reset 一次，生成只有数条事件的伪 run。
            // 重开分支统一等真正开始下一局时再结束旧 run 并建立新 run。
            m_stop_after_abandon = false;
            Log.info("BlackFlow run reset deferred until StartExplore after abandonment settlement");
            return true;
        }

        finish_current_run(false);
        Log.info("BlackFlow run state finished after abandonment");
        if (m_task_ptr != nullptr) {
            m_task_ptr->set_enable(false);
            Log.info("BlackFlow task stopped after abandonment");
        }
        return true;
    }
    if (work == PendingWork::ResolveHuntedAction) {
        Task.set_task_base("BlackFlow@Roguelike@HuntedAction", "BlackFlow@Roguelike@ExitThenAbandon-Enter");
        if (m_session == nullptr || m_session->profile() != "automation_collection") {
            return true;
        }

        const int floor = m_session->current_floor().value_or(m_session->run().floor);
        if (floor != 3 && floor >= 1 && floor <= 5) {
            // 自动化收集只适配第三层追猎。其他层明确结束这一局并走可重开的弃局链，
            // 而不是把行动力耗尽误报为采集完成。AbandonConfirm 后会清空局内状态并回到 StartExplore。
            m_session->fail(
                "automation_collection_pursuit_unsupported",
                "floor " + std::to_string(floor) + " pursuit battle is not adapted",
                FailureDisposition::RestartRun);
            Task.set_task_base("BlackFlow@Roguelike@HuntedAction", "BlackFlow@Roguelike@StrategyTerminated-Enter");
            Log.info("BlackFlow automation collection abandons unsupported pursuit", "floor", floor);
            report_outputs();
            return true;
        }
        if (floor == 3) {
            // 追猎不是地图节点事务，直接复用已适配的编队/战斗链；战斗胜利后跳过节点结果派发，
            // 直接识别并进入第四层。战斗失败仍由通用结算链回到 StartExplore。
            m_session->mark_floor_three_pursuit_battle_pending();
            Task.set_task_base("BlackFlow@Roguelike@NodeCompletionAction", "BlackFlow@Roguelike@NextLevel-Enter");
            Task.set_task_base("BlackFlow@Roguelike@HuntedAction", "BlackFlow@Roguelike@HuntedBattle-Enter");
            Log.info("BlackFlow automation collection enters adapted floor 3 pursuit");
            return true;
        }
        Log.error("BlackFlow pursuit appeared with an invalid floor", floor);
        return true;
    }
    if (work != PendingWork::ResolveTerminalAction) {
        return true;
    }

    if (m_session != nullptr && !m_session->terminated()) {
        auto [outcome, reason] = classify_unreported_termination(pre_task);
        Log.error("BlackFlow terminated without a strategy result", trigger, pre_task, outcome, reason);
        m_session->fail(std::move(outcome), std::move(reason), FailureDisposition::StopTask);
    }

    std::string profile;
    std::string outcome;
    std::string reason;
    if (m_session != nullptr && m_session->result().has_value()) {
        profile = m_session->result()->profile;
        outcome = m_session->result()->outcome;
        reason = m_session->result()->termination_reason;
    }

    const std::string next_action =
        m_session != nullptr && m_session->result().has_value() ? m_session->result()->next_action : "stop_run";
    if (next_action == "stop_task") {
        Log.error(
            "BlackFlow stops the task without abandoning the current run",
            "profile",
            profile,
            trigger,
            pre_task,
            outcome,
            reason);
        report_outputs();
        if (m_task_ptr != nullptr) {
            m_task_ptr->set_enable(false);
        }
        return true;
    }
    // 两支都在主 ProcessTask 上完成放弃；停止一支在 AbandonConfirm 完成后、StartExplore 之前停用主任务。
    m_stop_after_abandon = next_action != "restart_current_run";
    const std::string task = "BlackFlow@Roguelike@ExitThenAbandon-Enter";
    Task.set_task_base("BlackFlow@Roguelike@StrategyTerminalAction", task);
    Log.info(
        "BlackFlow strategy terminal action",
        "profile",
        profile,
        trigger,
        pre_task,
        outcome,
        reason,
        next_action,
        task);
    report_outputs();
    return true;
}

} // namespace asst::blackflow
