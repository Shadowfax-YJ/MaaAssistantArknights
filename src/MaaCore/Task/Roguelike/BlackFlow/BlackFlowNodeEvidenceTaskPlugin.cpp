#include "BlackFlowNodeEvidenceTaskPlugin.h"

#include <utility>

#include "BlackFlowCollectionPopup.h"

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
    set_retry_times(0);
}

bool BlackFlowNodeEvidenceTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }
    const std::string task = details.get("details", "task", std::string());
    const bool recruitment_page = msg == AsstMsg::SubTaskStart && node_recruitment_page_task(task);
    const bool get_drop_page = msg == AsstMsg::SubTaskStart && node_get_drop_screen(task).has_value() &&
                               !node_get_drop_uses_custom_selector(task);
    if (!recruitment_page && !get_drop_page) {
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
    std::optional<Rect> selected_button;
    if (node_get_drop_screen(m_pending_task) == NodeGetDropScreen::Select) {
        if (const auto hit = get_hit_detail<Matcher::Result>(); hit != nullptr) {
            selected_button = hit->rect;
        }
    }
    std::string error;
    if (!m_capture(m_pending_task, selected_button, &error)) {
        // 证据采集不能阻塞掉落领取；失败只进入日志，原 ProcessTask 仍继续点击。
        Log.warn("BlackFlow node page capture failed", m_pending_task, error);
    }
    m_pending_task.clear();
    return true;
}
} // namespace asst::blackflow
