#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <meojson/json.hpp>

#include "BlackFlowPolicy.h"

namespace cv
{
class Mat;
}

namespace asst::blackflow
{
enum class DiagnosticLevel
{
    Normal,
    Detailed,
    Full,
};

enum class DiagnosticTrigger
{
    RoutineObservation,
    RoutingDecision,
    BattleStageObservation,
    NodeIdentityResolved,
    ProcessingItemObservation,
    RebuildConflict,
    InferredEdgeSelected,
    PreviewCostMismatch,
    IdentityConflict,
    PostMoveMismatch,
    MapRebuildFailed,
    PageRecoveryFailed,
};

struct DiagnosticSettings
{
    DiagnosticLevel level = DiagnosticLevel::Normal;
    std::size_t image_package_limit = 3;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct DiagnosticArtifactRequest
{
    DiagnosticTrigger trigger = DiagnosticTrigger::RebuildConflict;
    std::string artifact_set_id;
    std::string observation_id;
    std::string decision_id;
    std::string transaction_id;
    bool include_captured_image = false;
    bool include_images = false;
    json::object snapshot;
    struct EvidenceImage
    {
        std::string role;
        std::shared_ptr<cv::Mat> image;
    };
    std::vector<EvidenceImage> evidence_images;
};

struct BlackFlowTelemetryEvent
{
    std::string what;
    json::object details;
};

[[nodiscard]] std::optional<DiagnosticLevel> parse_diagnostic_level(std::string_view value) noexcept;
[[nodiscard]] bool includes_full_routing_details(DiagnosticLevel level) noexcept;
[[nodiscard]] std::string_view to_string(DiagnosticLevel level) noexcept;
[[nodiscard]] std::string_view to_string(DiagnosticTrigger trigger) noexcept;
} // namespace asst::blackflow
