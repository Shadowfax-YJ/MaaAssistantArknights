#pragma once

#include <algorithm>
#include <cstdlib>

#include "BlackFlowAutomationCollectionRules.h"
#include "BlackFlowStartRewardRules.h"
#include "Vision/OCRer.h"
#include "Vision/Miscellaneous/PipelineAnalyzer.h"

namespace asst::blackflow
{
struct OpeningRolesObservation
{
    cv::Mat image;
    std::vector<TextRect> candidates;
    std::vector<TextRect> vouchers;
    std::optional<Rect> prompt;
    bool voucher_page = false;
};

inline OpeningRolesObservation inspect_opening_roles(cv::Mat image)
{
    OpeningRolesObservation frame { .image = std::move(image) };
    if (frame.image.empty()) {
        return frame;
    }
    const auto read = [&](std::string_view task) {
        OCRer ocr(frame.image);
        ocr.set_task_info(std::string(task));
        return ocr.analyze().value_or(std::vector<TextRect> {});
    };
    frame.voucher_page = !read("BlackFlow@Roguelike@OpeningVoucherPage").empty();
    if (frame.voucher_page) {
        frame.vouchers = read("BlackFlow@Roguelike@OpeningVoucherTitles");
    }
    else {
        frame.candidates = read("BlackFlow@Roguelike@OpeningRoleCandidates");
        if (!frame.candidates.empty()) {
            const auto prompts = read("BlackFlow@Roguelike@AutomationCollectionStartRewardConfirmationPrompt");
            if (!prompts.empty()) {
                frame.prompt = prompts.front().rect;
            }
        }
    }
    return frame;
}

inline bool opening_vouchers_match(const OpeningRolesObservation& frame)
{
    if (!frame.voucher_page || frame.vouchers.size() != 3) {
        return false;
    }
    std::vector<std::string> names;
    for (const auto& voucher : frame.vouchers) {
        names.push_back(voucher.text);
    }
    std::sort(names.begin(), names.end());
    return names == std::vector<std::string> { "医疗招募券", "术师招募券", "重装招募券" };
}

inline json::object opening_roles_details(const OpeningRolesObservation& frame)
{
    json::array candidates, vouchers;
    for (const auto& title : frame.candidates) {
        candidates.emplace_back(title.to_json());
    }
    for (const auto& title : frame.vouchers) {
        vouchers.emplace_back(title.to_json());
    }
    return {
        { "expected_group", std::string(AutomationCollectionRoles) },
        { "expected_vouchers", json::array { "重装招募券", "术师招募券", "医疗招募券" } },
        { "candidates", std::move(candidates) },
        { "vouchers", std::move(vouchers) },
        { "voucher_page", frame.voucher_page },
        { "confirmation_prompt", frame.prompt ? json::value(json::array {
              frame.prompt->x, frame.prompt->y, frame.prompt->width, frame.prompt->height }) : json::value(nullptr) },
        { "evidence_scope", "opening_diagnostic" },
    };
}

struct OpeningRewardTransition
{
    bool same_reward_page = false;
    bool destination_ready = false;
};

inline OpeningRewardTransition inspect_opening_reward_transition(const cv::Mat& image, std::string_view selected)
{
    OpeningRewardTransition result;
    OCRer header(image);
    header.set_task_info("BlackFlow@Roguelike@OpeningSupportPage");
    if (header.analyze()) {
        OCRer titles(image);
        titles.set_task_info("BlackFlow@Roguelike@AutomationCollectionPreferredStartReward");
        titles.set_required({});
        titles.set_fuzzy_match(false);
        if (const auto detected = titles.analyze()) {
            bool known_title = false;
            for (const auto& title : *detected) {
                const auto normalized = normalize_automation_collection_start_reward_title(title.text);
                known_title = known_title || normalized.has_value();
                if (normalized && *normalized == selected) {
                    result.same_reward_page = true;
                }
            }
            result.destination_ready = !result.same_reward_page && known_title;
        }
        return result;
    }
    PipelineAnalyzer next(image);
    next.set_tasks({
        "BlackFlow@Roguelike@OpeningRoleCandidates",
        "BlackFlow@Roguelike@OpeningVoucherPage",
        "BlackFlow@Roguelike@LastRewardConfirm@(BlackFlow@Roguelike@CloseCollection)",
    });
    result.destination_ready = next.analyze().has_value();
    return result;
}

// Page recognition, device input and persistence are injected; this state
// machine is shared by the live opening plugin and the timing replay.
template <typename Observe, typename Record, typename Click, typename Swipe, typename Wait, typename Cancelled>
bool select_opening_roles(Observe&& observe, Record&& record, Click&& click, Swipe&& swipe, Wait&& wait, Cancelled&& cancelled)
{
    bool selected = false, confirmed = false;
    int swipes = 0, stable = 0, voucher_frames = 0, voucher_matches = 0;
    std::optional<Rect> previous;
    OpeningRolesObservation frame;
    const auto save = [&](std::string_view kind, std::string_view reason = "") {
        auto details = opening_roles_details(frame);
        details["selection_issued"] = selected;
        details["confirmation_issued"] = confirmed;
        details["failure_reason"] = std::string(reason);
        if (kind == "roles.confirming" && frame.prompt) {
            const auto point = automation_collection_start_reward_confirmation_point(*frame.prompt);
            details["input_x"] = point.x;
            details["input_y"] = point.y;
        }
        return record(kind, std::move(details), std::make_shared<cv::Mat>(frame.image));
    };
    for (int attempt = 0; attempt < 48 && !cancelled(); ++attempt) {
        if (attempt != 0 && !wait(250)) {
            return false;
        }
        frame = observe();
        if (attempt == 0 && !save("roles.observed")) {
            return false;
        }
        if (frame.voucher_page) {
            voucher_matches = opening_vouchers_match(frame) ? voucher_matches + 1 : 0;
            if (++voucher_frames < 2 || (selected && confirmed && voucher_matches < 2 && voucher_frames < 8)) {
                continue;
            }
            if (!selected || !confirmed || voucher_matches < 2) {
                save("failed", selected && confirmed ? "opening_vouchers_mismatch" : "roles_selection_bypassed");
                return false;
            }
            return save("roles.verified");
        }
        voucher_frames = 0;
        voucher_matches = 0;
        const auto target = std::find_if(frame.candidates.begin(), frame.candidates.end(), [](const auto& title) {
            return title.text == AutomationCollectionRoles;
        });
        if (target == frame.candidates.end()) {
            previous.reset();
            stable = 0;
            // Only a positively recognized combo page authorizes a swipe.
            if (!selected && !frame.candidates.empty() && swipes < 7) {
                if (!save("roles.swiping") || !swipe() || !wait(1000)) {
                    return false;
                }
                ++swipes;
            }
            continue;
        }
        const Rect hit = selected && frame.prompt ? *frame.prompt : target->rect;
        if (!previous || std::abs(previous->x - hit.x) > 5 || std::abs(previous->y - hit.y) > 5) {
            previous = hit;
            stable = 1;
            continue;
        }
        if (++stable < 2 || confirmed) {
            continue;
        }
        if (!selected) {
            if (!save("roles.selecting") || !click(Point { target->rect.x + target->rect.width / 2,
                                                         target->rect.y + target->rect.height / 2 })) {
                return false;
            }
            selected = true;
            previous.reset();
        }
        else if (frame.prompt &&
                 std::abs(frame.prompt->x + frame.prompt->width / 2 - target->rect.x - target->rect.width / 2) < 100) {
            // The prompt must belong to the selected card on this fresh frame.
            if (!save("roles.confirming") || !click(automation_collection_start_reward_confirmation_point(*frame.prompt))) {
                return false;
            }
            confirmed = true;
            previous.reset();
        }
    }
    if (!cancelled()) {
        save("failed", "roles_selection_did_not_settle");
    }
    return false;
}
} // namespace asst::blackflow
