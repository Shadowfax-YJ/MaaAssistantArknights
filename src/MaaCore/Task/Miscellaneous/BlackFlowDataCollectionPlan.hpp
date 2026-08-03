#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "BlackFlowDataCollectionTrigger.hpp"

namespace asst
{
struct OpeningCaptureGroupId
{
    std::string value;
};

enum class BlackFlowCaptureGroupRole
{
    None,
    BeginsGroup,
    CompletesGroup,
};

enum class BlackFlowCaptureImageSource
{
    RecognitionFrame,
    FreshScreenshot,
};

struct BlackFlowCapturePlan
{
    std::filesystem::path relative_dir;
    std::string_view suffix;
    std::string_view description;
    BlackFlowCaptureGroupRole group_role;
    BlackFlowCaptureImageSource image_source;
};

inline BlackFlowCapturePlan
    black_flow_capture_plan(BlackFlowCaptureTarget target, const std::optional<OpeningCaptureGroupId>& group_id)
{
    auto group_dir = std::filesystem::path("debug") / "roguelike" / "black_flow" / "opening_cycles";
    if (group_id) {
        group_dir /= group_id->value;
    }

    switch (target) {
    case BlackFlowCaptureTarget::StoreOverview:
        return {
            std::filesystem::path("debug") / "roguelike" / "black_flow_store",
            "",
            "Black Flow store overview",
            BlackFlowCaptureGroupRole::None,
            BlackFlowCaptureImageSource::RecognitionFrame,
        };
    case BlackFlowCaptureTarget::ProcessingItemPopup:
        return {
            group_dir,
            "processing-item",
            "Black Flow processing item popup",
            BlackFlowCaptureGroupRole::BeginsGroup,
            BlackFlowCaptureImageSource::FreshScreenshot,
        };
    case BlackFlowCaptureTarget::PartsBox:
        return {
            group_dir,
            "parts-box",
            "Black Flow parts box",
            BlackFlowCaptureGroupRole::CompletesGroup,
            BlackFlowCaptureImageSource::FreshScreenshot,
        };
    case BlackFlowCaptureTarget::None:
        break;
    }
    return {
        std::filesystem::path("debug") / "roguelike" / "unknown",
        "",
        "Black Flow data collection",
        BlackFlowCaptureGroupRole::None,
        BlackFlowCaptureImageSource::RecognitionFrame,
    };
}

class BlackFlowOpeningCaptureGroup
{
public:
    void begin(OpeningCaptureGroupId group_id) { m_group_id = std::move(group_id); }

    const std::optional<OpeningCaptureGroupId>& id() const noexcept { return m_group_id; }

    bool can_capture(const BlackFlowCapturePlan& plan) const noexcept
    {
        return plan.group_role != BlackFlowCaptureGroupRole::CompletesGroup || m_group_id.has_value();
    }

    void capture_succeeded(const BlackFlowCapturePlan& plan) noexcept
    {
        if (plan.group_role == BlackFlowCaptureGroupRole::CompletesGroup) {
            m_group_id.reset();
        }
    }

private:
    std::optional<OpeningCaptureGroupId> m_group_id;
};
} // namespace asst
