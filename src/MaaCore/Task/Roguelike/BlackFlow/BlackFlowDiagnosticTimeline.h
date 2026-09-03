#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asst::blackflow
{
[[nodiscard]] constexpr int diagnostic_processing_item_floor(int run_floor) noexcept
{
    // Normal routing now rebuilds the map before scanning the parts box. Keep a
    // defensive fallback so an interrupted/resumed observation never creates floor-0.
    return std::max(1, run_floor);
}

[[nodiscard]] constexpr bool diagnostic_evidence_is_visible_at_step(
    int evidence_floor,
    std::uint64_t evidence_sequence,
    int decision_floor,
    std::uint64_t decision_sequence) noexcept
{
    return evidence_floor == decision_floor && evidence_sequence <= decision_sequence;
}

[[nodiscard]] constexpr bool diagnostic_evidence_is_visible_at_step(
    int evidence_floor,
    std::uint64_t evidence_map_generation,
    std::uint64_t evidence_sequence,
    int decision_floor,
    std::uint64_t decision_map_generation,
    std::uint64_t decision_sequence) noexcept
{
    return evidence_floor == decision_floor && evidence_map_generation == decision_map_generation &&
           evidence_sequence <= decision_sequence;
}

[[nodiscard]] constexpr bool diagnostic_node_identity_checkpoint_required(
    bool exploration_notebook_identity_updated,
    bool page_content_resolved_identity) noexcept
{
    // Unknown hidden nodes can have their event title seeded while entering the page. The
    // later RoguelikeEvent callback is then not the first observed content, but it is still
    // the point where the old-floor exploration notebook changes and must be persisted.
    return exploration_notebook_identity_updated && page_content_resolved_identity;
}

struct DiagnosticEvidenceStamp
{
    int floor = 0;
    std::uint64_t map_generation = 0;
    std::uint64_t sequence = 0;
    std::string evidence_type;
};

template <typename EvidenceRange>
[[nodiscard]] std::vector<std::size_t> diagnostic_latest_evidence_by_type(
    const EvidenceRange& evidence,
    int decision_floor,
    std::uint64_t decision_map_generation,
    std::uint64_t decision_sequence)
{
    struct Latest
    {
        std::string_view evidence_type;
        std::uint64_t sequence = 0;
        std::size_t index = 0;
    };
    std::vector<Latest> latest;
    for (std::size_t index = 0; index < evidence.size(); ++index) {
        const auto& candidate = evidence[index];
        if (!diagnostic_evidence_is_visible_at_step(
                candidate.floor,
                candidate.map_generation,
                candidate.sequence,
                decision_floor,
                decision_map_generation,
                decision_sequence)) {
            continue;
        }
        const auto existing = std::ranges::find(latest, candidate.evidence_type, &Latest::evidence_type);
        if (existing == latest.end()) {
            latest.emplace_back(Latest { candidate.evidence_type, candidate.sequence, index });
        }
        else if (candidate.sequence >= existing->sequence) {
            existing->sequence = candidate.sequence;
            existing->index = index;
        }
    }
    std::vector<std::size_t> result;
    result.reserve(latest.size());
    for (const Latest& entry : latest) {
        result.emplace_back(entry.index);
    }
    std::ranges::sort(result);
    return result;
}

[[nodiscard]] inline std::string diagnostic_map_section_key(
    int floor,
    std::uint64_t map_generation,
    bool floor_four_remembrance)
{
    std::string result = "floor-" + std::to_string(floor) + "-generation-" + std::to_string(map_generation);
    if (floor_four_remembrance) {
        result += "-remembrance";
    }
    return result;
}

[[nodiscard]] inline std::string diagnostic_map_section_label(int floor, bool floor_four_remembrance)
{
    return (floor_four_remembrance ? "追忆 " : "") + std::to_string(floor) + " 层";
}

struct DiagnosticImageSelection
{
    bool captured = false;
    bool overlay = false;
};

[[nodiscard]] constexpr DiagnosticImageSelection diagnostic_image_selection(
    bool wants_map_images,
    bool routing_decision,
    std::size_t persisted_overlay_packages,
    std::size_t overlay_package_limit) noexcept
{
    const bool within_overlay_budget = persisted_overlay_packages < overlay_package_limit;
    return {
        .captured = wants_map_images && (routing_decision || within_overlay_budget),
        .overlay = wants_map_images && within_overlay_budget,
    };
}
} // namespace asst::blackflow
