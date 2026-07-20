#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "Utils/EnumMapping.hpp"

namespace asst
{
enum class BlackFlowClientType
{
    Official,
    Bilibili,
};

enum class BlackFlowTaskAdmission
{
    NotBlackFlow,
    Accepted,
    Rejected,
};

namespace black_flow_client_detail
{
inline constexpr std::array<std::pair<BlackFlowClientType, std::string_view>, 2> ClientTypeNames { {
    { BlackFlowClientType::Official, "Official" },
    { BlackFlowClientType::Bilibili, "Bilibili" },
} };
} // namespace black_flow_client_detail

inline std::string_view black_flow_client_type_name(BlackFlowClientType client_type) noexcept
{
    return utils::enum_name(client_type, black_flow_client_detail::ClientTypeNames);
}

inline std::optional<BlackFlowClientType> parse_black_flow_client_type(std::string_view client_type) noexcept
{
    return utils::parse_enum(client_type, black_flow_client_detail::ClientTypeNames);
}

inline bool is_black_flow_task_name(std::string_view task_name) noexcept
{
    constexpr std::string_view MiniGamePrefix = "MiniGame@BlackFlow@";
    constexpr std::string_view LegacyPrefix = "BlackFlowTemporary@";
    return task_name == "MiniGame@BlackFlow" || task_name.starts_with(MiniGamePrefix) ||
           task_name == "BlackFlowTemporary" || task_name.starts_with(LegacyPrefix);
}

inline BlackFlowTaskAdmission check_black_flow_task_admission(
    std::span<const std::string> task_names,
    std::optional<BlackFlowClientType> client_type) noexcept
{
    const bool contains_black_flow_task = std::ranges::any_of(task_names, is_black_flow_task_name);
    if (!contains_black_flow_task) {
        return BlackFlowTaskAdmission::NotBlackFlow;
    }
    return client_type ? BlackFlowTaskAdmission::Accepted : BlackFlowTaskAdmission::Rejected;
}
} // namespace asst
