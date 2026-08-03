#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

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
    RequiresGroup,
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
            BlackFlowCaptureGroupRole::RequiresGroup,
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
} // namespace asst
