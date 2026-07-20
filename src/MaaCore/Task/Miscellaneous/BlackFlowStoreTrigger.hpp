#pragma once

#include "Common/AsstMsg.h"

namespace asst
{
inline bool should_start_black_flow_store_cycle(AsstMsg msg, const json::value& details)
{
    return msg == AsstMsg::SubTaskStart && details.get("subtask", std::string()) == "ProcessTask" &&
           details.get("details", "task", std::string()) == "MiniGame@BlackFlow@Begin";
}
} // namespace asst
