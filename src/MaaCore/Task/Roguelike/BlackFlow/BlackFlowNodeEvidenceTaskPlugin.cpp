#include "BlackFlowNodeEvidenceTaskPlugin.h"

#include <utility>

#include "BlackFlowCollectionPopup.h"

#include "Controller/Controller.h"
#include "Utils/Logger.hpp"
#include "Vision/Matcher.h"

namespace asst::blackflow
{
BlackFlowNodeEvidenceTaskPlugin::BlackFlowNodeEvidenceTaskPlugin(
    const AsstCallback& callback,
    Assistant* inst,
    std::string_view task_chain,
    Capture capture) :
    AbstractTaskPlugin(callback, inst, task_chain),
    m_capture(std::move(capture))
{
    set_block(true);
    set_may_change_ui(true);
    set_retry_times(0);
}

bool BlackFlowNodeEvidenceTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }
    const std::string task = details.get("details", "task", std::string());
    const bool recruitment_page = msg == AsstMsg::SubTaskStart && node_recruitment_page_task(task);
    const bool get_drop_page = msg == AsstMsg::SubTaskStart &&
                               node_get_drop_requires_stable_click(task);
    // 追猎 Boss 的收藏品展示可能在任何兜底点击时消失。首次 ClickToDrops
    // 真正执行前先无条件保存当前画面，不能等点击完成，也不能只依赖按钮模板回调。
    const bool pursuit_loot_preclick = msg == AsstMsg::SubTaskStart &&
                                       pursuit_first_loot_click_task(
                                           task,
                                           details.get("details", "exec_times", 0));
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
} // namespace asst::blackflow
