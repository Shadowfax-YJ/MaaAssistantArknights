#include "BlackFlowNodeEvidenceTaskPlugin.h"

#include <utility>

#include "BlackFlowCollectionPopup.h"
#include "BlackFlowEvidenceFrame.h"

#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "Utils/Logger.hpp"
#include "Vision/Matcher.h"

namespace asst::blackflow
{
BlackFlowNodeEvidenceTaskPlugin::BlackFlowNodeEvidenceTaskPlugin(
    const AsstCallback& callback,
    Assistant* inst,
    std::string_view task_chain,
    const std::shared_ptr<RoguelikeConfig>& config,
    const std::shared_ptr<RoguelikeControlTaskPlugin>& control,
    std::shared_ptr<BlackFlowSession> session,
    std::shared_ptr<IBlackFlowTaskPort> port,
    Capture capture) :
    BlackFlowTaskPluginBase(callback, inst, task_chain, config, control, std::move(session), std::move(port)),
    m_capture(std::move(capture))
{
    set_block(true);
    set_may_change_ui(true);
    set_retry_times(0);
}

void BlackFlowNodeEvidenceTaskPlugin::reset_in_run_variables()
{
    m_reward_scope.clear();
    m_stalled_reward_rounds = 0;
}

bool BlackFlowNodeEvidenceTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }
    const std::string task = details.get("details", "task", std::string());
    const bool recruitment_page = msg == AsstMsg::SubTaskStart && node_recruitment_page_task(task);
    const bool get_drop_page = msg == AsstMsg::SubTaskStart &&
                               (node_get_drop_requires_stable_click(task) || node_recruitment_choice_task(task));
    // 追猎 Boss 的收藏品展示可能在任何兜底点击时消失。首次 ClickToDrops
    // 真正执行前先无条件保存当前画面，不能等点击完成，也不能只依赖按钮模板回调。
    const bool pursuit_loot_preclick =
        msg == AsstMsg::SubTaskStart && pursuit_first_loot_click_task(task, details.get("details", "exec_times", 0));
    if (!recruitment_page && !get_drop_page && !pursuit_loot_preclick) {
        return false;
    }
    m_pending_task = task;
    return true;
}

bool BlackFlowNodeEvidenceTaskPlugin::_run()
{
    if (!m_capture || m_pending_task.empty()) {
        return true;
    }
    const bool controlled_click = node_get_drop_requires_stable_click(m_pending_task);
    if (m_pending_task == "BlackFlow@Roguelike@GetDropConfirmed") {
        click_drop_with_progress_check();
        m_pending_task.clear();
        return true;
    }
    std::string error;
    if (!m_capture(m_pending_task, std::nullopt, &error)) {
        Log.warn("BlackFlow node page capture failed", m_pending_task, error);
        // Confirmed 任务的 ProcessTask 是 DoNothing。稳定帧拿不到或语义已消失时必须放弃
        // 本次点击，让任务图重新识别；不能再使用动画开始前留下的命中框。
        m_pending_task.clear();
        return true;
    }
    if (controlled_click) {
        const cv::Mat current = ctrler()->get_image();
        Matcher matcher(current);
        matcher.set_task_info(m_pending_task);
        const auto current_hit = matcher.analyze();
        if (!current_hit.has_value()) {
            Log.warn(
                "BlackFlow guarded reward click skipped because the stable frame no longer matches",
                m_pending_task);
            m_pending_task.clear();
            return true;
        }
        Log.info("BlackFlow guarded reward click", m_pending_task, current_hit->rect);
        ctrler()->click(current_hit->rect);
    }
    m_pending_task.clear();
    return true;
}

void BlackFlowNodeEvidenceTaskPlugin::click_drop_with_progress_check()
{
    constexpr std::string_view Action = "BlackFlow@Roguelike@GetDropConfirmedAction";
    constexpr std::string_view Continue = "BlackFlow@Roguelike@GetDropConfirmedContinue";
    // 动作槽位每次重置。插件返回 false 不会改变 ProcessTask 的后继，必须显式切换恢复入口。
    Task.set_task_base(std::string(Action), std::string(Continue));
    const auto state = m_session->run_log_state();
    const std::string scope = std::to_string(m_session->run_revision()) + ":" +
                              state.get("transaction_id", std::string()) + ":" +
                              std::to_string(state.get("page", "page_revision", 0));
    if (scope != m_reward_scope) {
        m_reward_scope = scope;
        m_stalled_reward_rounds = 0;
    }

    // 只比较奖励名称、数量及说明；HUD、零件特效与按钮呼吸动画不能算领取进展。
    const cv::Rect reward_content(20, 330, 1060, 180);
    for (int attempt = 1; attempt <= 3 && !need_exit(); ++attempt) {
        std::string error;
        if (!m_capture(m_pending_task, std::nullopt, &error)) {
            // 证据捕获也会因领奖后离开奖励页而失败。先看当前页，不能把成功转场
            // 当作下一次领取失败；空截图仍是未知状态，保留有界恢复。
            const cv::Mat current = ctrler()->get_image();
            if (!current.empty()) {
                Matcher current_matcher(current);
                current_matcher.set_task_info(m_pending_task);
                if (!current_matcher.analyze().has_value()) {
                    Log.info("BlackFlow reward page changed; resuming page classification");
                    m_stalled_reward_rounds = 0;
                    return;
                }
            }
            Log.warn("BlackFlow reward evidence unavailable", error);
            continue;
        }
        const cv::Mat before = ctrler()->get_image();
        if (before.empty()) {
            continue;
        }
        Matcher matcher(before);
        matcher.set_task_info(m_pending_task);
        const auto hit = matcher.analyze();
        if (!hit.has_value()) {
            // 当前页面已变，由原任务图识别招募、选奖励或离开确认。
            m_stalled_reward_rounds = 0;
            return;
        }
        const bool dispatched = ctrler()->click(hit->rect);
        Log.info("BlackFlow guarded reward click", m_pending_task, hit->rect, "dispatched", dispatched);
        cv::Mat previous_changed;
        bool progressed = false;
        for (int sample = 0; sample < 4 && !need_exit(); ++sample) {
            if (!sleep(500)) {
                return;
            }
            const cv::Mat after = ctrler()->get_image();
            if (after.empty() || after.size() != before.size() || after.type() != before.type()) {
                previous_changed.release();
                continue;
            }
            if (evidence_frames_match(before, after, reward_content)) {
                previous_changed.release();
                continue;
            }
            if (evidence_frames_match(previous_changed, after, reward_content)) {
                progressed = true;
                break;
            }
            previous_changed = after.clone();
        }
        record_run_event(
            progressed ? RunLogLevel::Info : RunLogLevel::Warning,
            "reward.click",
            "verified",
            progressed ? "page_changed" : "no_progress",
            json::object {
                { "attempt", attempt },
                { "recovery_round", m_stalled_reward_rounds },
                { "click_dispatched", dispatched },
                { "rect", json::array { hit->rect.x, hit->rect.y, hit->rect.width, hit->rect.height } },
            },
            m_pending_task);
        if (progressed) {
            m_stalled_reward_rounds = 0;
            return;
        }
    }
    if (need_exit()) {
        return;
    }
    ++m_stalled_reward_rounds;
    if (m_stalled_reward_rounds >= 2) {
        m_session->fail(
            "reward_no_progress",
            "奖励领取连续无进展，等待重试后仍未恢复；已停止任务并保留当前探索",
            FailureDisposition::StopTask);
        report_outputs();
        Task.set_task_base(std::string(Action), "BlackFlow@Roguelike@StrategyTerminated-Enter");
        return;
    }
    Log.warn("BlackFlow reward unavailable or unchanged after bounded rechecks; scheduling recovery");
    Task.set_task_base(std::string(Action), "BlackFlow@Roguelike@RecoveryFailed");
}
} // namespace asst::blackflow
