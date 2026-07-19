#pragma once

#include "Common/AsstMsg.h"

namespace asst
{
inline bool should_capture_black_flow_store_overview(AsstMsg msg, const json::value& details)
{
    return msg == AsstMsg::SubTaskStart && details.get("subtask", std::string()) == "ProcessTask" &&
           details.get("details", "task", std::string()) == "BlackFlowTemporary@InvestSystem";
}
} // namespace asst
