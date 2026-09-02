#include "BlackFlowCollectionPopupTaskPlugin.h"

#include <utility>

#include "BlackFlowCollectionPopup.h"

#include "Utils/Logger.hpp"

namespace asst::blackflow
{
BlackFlowCollectionPopupTaskPlugin::BlackFlowCollectionPopupTaskPlugin(
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

bool BlackFlowCollectionPopupTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (msg != AsstMsg::SubTaskStart || details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }
    const std::string task = details.get("details", "task", std::string());
    if (!collection_popup_button(task).has_value()) {
        return false;
    }
    m_pending_task = task;
    return true;
}

bool BlackFlowCollectionPopupTaskPlugin::_run()
{
    if (!m_capture || m_pending_task.empty()) {
        return true;
    }
    std::string error;
    if (!m_capture(m_pending_task, &error)) {
        // 采集故障不能把仍可操作的探索界面永久卡死；保留错误日志后仍交回原 ProcessTask 点击。
        Log.warn("BlackFlow collection popup capture failed", m_pending_task, error);
    }
    m_pending_task.clear();
    return true;
}
} // namespace asst::blackflow
