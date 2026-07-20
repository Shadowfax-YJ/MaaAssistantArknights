#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "BlackFlowProductNameMatcher.hpp"

namespace asst
{
struct BlackFlowStoreTitleRoi
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool operator==(const BlackFlowStoreTitleRoi&) const = default;
};

inline constexpr std::array<BlackFlowStoreTitleRoi, 10> BlackFlowStoreTitleRois { {
    { 624, 179, 178, 30 },
    { 830, 179, 178, 30 },
    { 1036, 179, 178, 30 },
    { 418, 388, 178, 30 },
    { 624, 388, 178, 30 },
    { 830, 388, 178, 30 },
    { 1036, 388, 178, 30 },
    { 418, 598, 178, 30 },
    { 624, 598, 178, 30 },
    { 830, 598, 178, 30 },
} };

inline constexpr size_t BlackFlowStoreFingerprintPixelCount = 10U * 178U * 30U;
inline constexpr double BlackFlowStoreStableFingerprintDistance = 0.005;
inline constexpr double BlackFlowStoreChangedFingerprintDistance = 0.02;

using BlackFlowStoreTitleFingerprint = std::array<std::uint8_t, BlackFlowStoreFingerprintPixelCount>;

struct BlackFlowStoreFrameObservation
{
    bool store_anchor_visible = false;
    bool refresh_control_visible = false;
    bool overlay_visible = false;
    BlackFlowStoreTitleFingerprint title_fingerprint {};
    std::array<bool, 10> title_foreground {};

    bool ready() const noexcept { return store_anchor_visible && refresh_control_visible && !overlay_visible; }
};

enum class BlackFlowStorePageClassification
{
    NotReady,
    Unstable,
    StableInitial,
    StableOld,
    StableNew,
};

struct BlackFlowStoreOcrFragment
{
    int x = 0;
    std::string text;
    double score = 0.0;
};

struct BlackFlowStoreSlotOcr
{
    bool succeeded = true;
    std::vector<BlackFlowStoreOcrFragment> fragments;
    std::string error_message;
};

enum class BlackFlowAnalyzedSlotStatus
{
    Matched,
    Empty,
    Unmatched,
    OcrError,
    MatchError,
};

enum class BlackFlowAnalyzedPageStatus
{
    Complete,
    Partial,
    Failed,
};

struct BlackFlowAnalyzedSlot
{
    BlackFlowAnalyzedSlotStatus status = BlackFlowAnalyzedSlotStatus::OcrError;
    std::string ocr_text;
    double ocr_score = 0.0;
    std::string standard_product_name;
    BlackFlowProductNameMatchKind match_kind = BlackFlowProductNameMatchKind::Unmatched;
    std::string error_message;
};

struct BlackFlowStoreSlotsAnalysis
{
    BlackFlowAnalyzedPageStatus page_status = BlackFlowAnalyzedPageStatus::Failed;
    std::array<BlackFlowAnalyzedSlot, 10> slots;
};

double black_flow_store_fingerprint_distance(
    const BlackFlowStoreTitleFingerprint& left,
    const BlackFlowStoreTitleFingerprint& right) noexcept;

BlackFlowStorePageClassification classify_black_flow_store_page(
    const BlackFlowStoreFrameObservation& previous,
    const BlackFlowStoreFrameObservation& current,
    const std::optional<BlackFlowStoreTitleFingerprint>& last_committed) noexcept;

BlackFlowStoreSlotsAnalysis analyze_black_flow_store_slots(
    const std::array<bool, 10>& foreground,
    const std::array<BlackFlowStoreSlotOcr, 10>& ocr,
    const BlackFlowProductNameMatcher& matcher);
} // namespace asst
