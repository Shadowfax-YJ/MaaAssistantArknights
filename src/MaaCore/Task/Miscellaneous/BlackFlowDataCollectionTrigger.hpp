#pragma once

#include <string>

#include "Common/AsstMsg.h"

namespace asst
{
enum class BlackFlowCaptureTarget
{
    None,
    StoreOverview,
    ProcessingItemPopup,
    PartsBox,
};

inline BlackFlowCaptureTarget black_flow_capture_target(AsstMsg msg, const json::value& details)
{
    if (details.get("subtask", std::string()) != "ProcessTask") {
        return BlackFlowCaptureTarget::None;
    }

    const auto task = details.get("details", "task", std::string());
    if (msg == AsstMsg::SubTaskStart && task == "BlackFlowTemporary@InvestSystem") {
        return BlackFlowCaptureTarget::StoreOverview;
    }
    if (msg != AsstMsg::SubTaskCompleted) {
        return BlackFlowCaptureTarget::None;
    }
    if (task == "MiniGame@BlackFlow@ProcessingItemReady") {
        return BlackFlowCaptureTarget::ProcessingItemPopup;
    }
    if (task == "MiniGame@BlackFlow@PartsBoxReady") {
        return BlackFlowCaptureTarget::PartsBox;
    }
    return BlackFlowCaptureTarget::None;
}
} // namespace asst
