#include "BlackFlowStorePageAnalyzer.hpp"

#include <algorithm>
#include <cmath>
#include <exception>

double asst::black_flow_store_fingerprint_distance(
    const BlackFlowStoreTitleFingerprint& left,
    const BlackFlowStoreTitleFingerprint& right) noexcept
{
    size_t mismatch_count = 0;
    for (size_t index = 0; index < left.size(); ++index) {
        mismatch_count += left[index] != right[index] ? 1U : 0U;
    }
    return static_cast<double>(mismatch_count) / static_cast<double>(left.size());
}

asst::BlackFlowStorePageClassification asst::classify_black_flow_store_page(
    const BlackFlowStoreFrameObservation& previous,
    const BlackFlowStoreFrameObservation& current,
    const std::optional<BlackFlowStoreTitleFingerprint>& last_committed) noexcept
{
    if (!previous.ready() || !current.ready()) {
        return BlackFlowStorePageClassification::NotReady;
    }
    if (black_flow_store_fingerprint_distance(previous.title_fingerprint, current.title_fingerprint) >
        BlackFlowStoreStableFingerprintDistance) {
        return BlackFlowStorePageClassification::Unstable;
    }
    if (!last_committed) {
        return BlackFlowStorePageClassification::StableInitial;
    }
    if (black_flow_store_fingerprint_distance(current.title_fingerprint, last_committed.value()) >=
        BlackFlowStoreChangedFingerprintDistance) {
        return BlackFlowStorePageClassification::StableNew;
    }
    return BlackFlowStorePageClassification::StableOld;
}

asst::BlackFlowStoreSlotsAnalysis asst::analyze_black_flow_store_slots(
    const std::array<bool, 10>& foreground,
    const std::array<BlackFlowStoreSlotOcr, 10>& ocr,
    const BlackFlowProductNameMatcher& matcher)
{
    BlackFlowStoreSlotsAnalysis result;
    size_t error_count = 0;

    for (size_t index = 0; index < result.slots.size(); ++index) {
        auto& slot = result.slots[index];
        if (!ocr[index].succeeded) {
            slot.status = BlackFlowAnalyzedSlotStatus::OcrError;
            slot.error_message = ocr[index].error_message.empty() ? "Word OCR failed" : ocr[index].error_message;
            ++error_count;
            continue;
        }

        auto fragments = ocr[index].fragments;
        std::ranges::stable_sort(fragments, {}, &BlackFlowStoreOcrFragment::x);
        bool valid_scores = true;
        slot.ocr_score = fragments.empty() ? 0.0 : 1.0;
        for (const auto& fragment : fragments) {
            slot.ocr_text += fragment.text;
            if (!std::isfinite(fragment.score) || fragment.score < 0.0 || fragment.score > 1.0) {
                valid_scores = false;
            }
            else {
                slot.ocr_score = std::min(slot.ocr_score, fragment.score);
            }
        }
        if (!valid_scores) {
            slot.status = BlackFlowAnalyzedSlotStatus::OcrError;
            slot.error_message = "Word OCR returned an invalid score";
            ++error_count;
            continue;
        }

        if (slot.ocr_text.empty()) {
            if (foreground[index]) {
                slot.status = BlackFlowAnalyzedSlotStatus::OcrError;
                slot.error_message = "Title ROI contains foreground but Word OCR returned no text";
                ++error_count;
            }
            else {
                slot.status = BlackFlowAnalyzedSlotStatus::Empty;
            }
            continue;
        }

        try {
            const auto matched = matcher.match(slot.ocr_text, slot.ocr_score);
            slot.match_kind = matched.kind;
            slot.standard_product_name = matched.standard_product_name;
            slot.status = matched.kind == BlackFlowProductNameMatchKind::Unmatched
                              ? BlackFlowAnalyzedSlotStatus::Unmatched
                              : BlackFlowAnalyzedSlotStatus::Matched;
        }
        catch (const std::exception& exception) {
            slot.status = BlackFlowAnalyzedSlotStatus::MatchError;
            slot.error_message = exception.what();
            ++error_count;
        }
    }

    if (error_count == 0U) {
        result.page_status = BlackFlowAnalyzedPageStatus::Complete;
    }
    else if (error_count == result.slots.size()) {
        result.page_status = BlackFlowAnalyzedPageStatus::Failed;
    }
    else {
        result.page_status = BlackFlowAnalyzedPageStatus::Partial;
    }
    return result;
}
