#pragma once

#include <memory>
#include <cstdlib>
#include <optional>
#include <string>

#include "Config/TaskData.h"
#include "BlackFlowRunLog.h"
#include "Vision/Matcher.h"
#include "Vision/OCRer.h"

namespace asst::blackflow
{
inline RunLogEvent opening_cultivation_event(std::string_view kind, json::object details, json::object state)
{
    const bool pending = kind == "confirming", confirmed = kind == "confirmed", failed = kind == "failed";
    return {
        .level = failed ? RunLogLevel::Warning : RunLogLevel::Info,
        .action = "start.cultivation.reward." + std::string(kind),
        .phase = pending ? "before-click" : confirmed || failed ? "completed" : "observed",
        .outcome = pending ? "pending" : confirmed ? "confirmed" : failed ? "failed" :
            details.get("recognition_status", std::string()),
        .task = "RoguelikeCustomStartTaskPlugin",
        .state = std::move(state),
        .details = std::move(details),
    };
}

struct OpeningCultivationObservation
{
    cv::Mat image;
    bool page_visible = false;
    std::optional<Rect> confirm_button;
    bool next_page_ready = false;
};

inline OpeningCultivationObservation inspect_opening_cultivation(cv::Mat image)
{
    OpeningCultivationObservation result { .image = std::move(image) };
    if (result.image.empty()) {
        return result;
    }
    Matcher title(result.image);
    title.set_task_info("BlackFlow@Roguelike@OpeningCultivationHarvest");
    result.page_visible = title.analyze().has_value();
    if (result.page_visible) {
        Matcher button(result.image);
        button.set_task_info("BlackFlow@Roguelike@OpeningCultivationHarvestConfirm");
        if (const auto hit = button.analyze(); hit.has_value()) {
            result.confirm_button = hit->rect;
        }
    }
    return result;
}

template <typename Observe, typename Wait, typename Cancelled>
OpeningCultivationObservation wait_for_opening_cultivation(Observe&& observe, Wait&& wait, Cancelled&& cancelled)
{
    for (int attempt = 0; attempt < 12; ++attempt) {
        if (cancelled() || (attempt != 0 && !wait(250)) || cancelled()) {
            return {};
        }
        auto current = observe();
        if (!current.image.empty() && current.page_visible && current.confirm_button.has_value()) {
            return current;
        }
    }
    return {};
}

inline std::optional<int> opening_cultivation_amount(std::string_view text)
{
    if (text.starts_with('+')) {
        text.remove_prefix(1);
    }
    if (text.empty() || text.size() > 6) {
        return std::nullopt;
    }
    int amount = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        amount = amount * 10 + ch - '0';
    }
    return amount;
}

inline json::object opening_cultivation_details(const cv::Mat& image)
{
    OCRer ocr(image);
    ocr.set_task_info("BlackFlow@Roguelike@OpeningCultivationHarvestItems");
    const auto texts = image.empty() ? std::vector<TextRect> {} : ocr.analyze().value_or(std::vector<TextRect> {});
    json::array raw, items;
    bool complete = true;
    for (const auto& text : texts) {
        raw.emplace_back(json::object {
            { "text", text.text }, { "score", text.score },
            { "rect", json::array { text.rect.x, text.rect.y, text.rect.width, text.rect.height } },
        });
        if (opening_cultivation_amount(text.text).has_value() || text.score < 0.85) {
            continue;
        }
        std::optional<int> amount;
        int matches = 0;
        for (const auto& candidate : texts) {
            const auto value = opening_cultivation_amount(candidate.text);
            if (value.has_value() && candidate.score >= 0.85 && candidate.rect.y > text.rect.y &&
                std::abs(candidate.rect.x + candidate.rect.width / 2 - text.rect.x - text.rect.width / 2) <= 70) {
                amount = value;
                ++matches;
            }
        }
        complete = complete && matches == 1;
        json::object item { { "name", text.text }, { "amount", nullptr } };
        if (matches == 1) {
            item["amount"] = *amount;
        }
        items.emplace_back(std::move(item));
    }
    return {
        { "source", "opening_cultivation" },
        { "recognition_scope", "visible_reward_cards" },
        { "recognition_status", items.empty() ? "unreadable" : complete ? "recognized" : "partial" },
        { "recognized_texts", std::move(raw) },
        { "items", std::move(items) },
    };
}

// Replay the same persistence/click/wait policy with injected device I/O.
// A disappearing button alone is insufficient: require two positive successor frames.
template <typename Observe, typename Record, typename Click, typename Wait, typename Cancelled>
bool claim_opening_cultivation(
    OpeningCultivationObservation current,
    json::object details,
    Observe&& observe,
    Record&& record,
    Click&& click,
    Wait&& wait,
    Cancelled&& cancelled)
{
    const auto fail = [&](std::string_view reason) {
        details["claim_status"] = "unconfirmed";
        details["failure_reason"] = std::string(reason);
        record("failed", details, nullptr);
        return false;
    };
    if (!current.page_visible || !current.confirm_button.has_value() || current.image.empty() || cancelled()) {
        return fail("harvest_page_or_button_not_verified");
    }
    details["claim_status"] = "unconfirmed";
    if (!record("observed", details, std::make_shared<cv::Mat>(current.image.clone()))) {
        return fail("harvest_evidence_not_saved");
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (cancelled() || !current.page_visible || !current.confirm_button.has_value()) {
            return fail("harvest_page_lost_before_click");
        }
        details["confirmation_attempt"] = attempt + 1;
        details["claim_status"] = "pending";
        details["confirmation_rect"] = json::array {
            current.confirm_button->x, current.confirm_button->y,
            current.confirm_button->width, current.confirm_button->height,
        };
        if (!record("confirming", details, nullptr)) {
            return fail("harvest_confirmation_evidence_not_saved");
        }
        if (cancelled() || !click(*current.confirm_button)) {
            return fail("harvest_click_not_dispatched");
        }
        int ready = 0;
        for (int sample = 0; sample < 20; ++sample) {
            if (cancelled() || !wait(250) || cancelled()) {
                return fail("cancelled_after_harvest_click");
            }
            current = observe();
            ready = !current.page_visible && current.next_page_ready ? ready + 1 : 0;
            if (ready >= 2) {
                details["claim_status"] = "confirmed";
                if (!record("confirmed", details, nullptr)) {
                    return fail("harvest_confirmation_result_not_saved");
                }
                return true;
            }
            if (sample >= 5 && current.page_visible && current.confirm_button.has_value()) {
                break;
            }
        }
    }
    return fail("harvest_confirmation_did_not_settle");
}
} // namespace asst::blackflow
