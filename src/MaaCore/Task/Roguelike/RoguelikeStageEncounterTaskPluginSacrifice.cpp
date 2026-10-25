#include "BlackFlow/BlackFlowCivilizationInventory.h"
#include "RoguelikeStageEncounterTaskPlugin.h"

#include <map>
#include <random>
#include <regex>

#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Task/ProcessTask.h"
#include "Task/Roguelike/BlackFlow/BlackFlowInventoryRules.h"
#include "Utils/Logger.hpp"
#include "Vision/Matcher.h"
#include "Vision/OCRer.h"
#include "Vision/Roguelike/BlackFlow/SacrificeRecognition.h"

namespace
{
using namespace asst;
constexpr std::string_view Prefix = "BlackFlow@Roguelike@";

bool recognize(const cv::Mat& image, std::string_view suffix)
{
    const std::string task = std::string(Prefix) + std::string(suffix);
    if (Task.get(task)->algorithm == AlgorithmType::OcrDetect) {
        OCRer ocr(image);
        ocr.set_task_info(task);
        return ocr.analyze().has_value();
    }
    Matcher matcher(image);
    matcher.set_task_info(task);
    return matcher.analyze().has_value();
}

std::size_t random_index(std::size_t size)
{
    static std::mt19937 random(std::random_device { }());
    return std::uniform_int_distribution<std::size_t>(0, size - 1)(random);
}
}

bool asst::RoguelikeStageEncounterTaskPlugin::refresh_sacrifice_context()
{
    const auto context = m_sacrifice_context_provider ? m_sacrifice_context_provider() : std::nullopt;
    if (context != m_sacrifice.context) {
        m_sacrifice = { };
        m_sacrifice.context = context;
    }
    return context.has_value();
}

bool asst::RoguelikeStageEncounterTaskPlugin::report_sacrifice(
    std::string_view phase,
    json::object details,
    const cv::Mat& image)
{
    if (m_sacrifice.evidence_failed || !m_sacrifice.context) {
        return false;
    }
    const auto& context = *m_sacrifice.context;
    if (!details.contains("exchange_round")) {
        details["exchange_round"] = m_sacrifice.exchanges + 1;
    }
    details["sacrifice_id"] =
        "sacrifice-" + std::to_string(context.run_revision) + "-" + std::to_string(context.page_revision);
    details["run_revision"] = context.run_revision;
    details["page_revision"] = context.page_revision;
    details["evidence_sequence"] = ++m_sacrifice.evidence_sequence;
    details["sacrifice_phase"] = std::string(phase);
    details["capture_kind"] = "sacrifice";
    Log.info("BlackFlow sacrifice", phase, details);
    if (!m_event_detail_observer ||
        !m_event_detail_observer(blackflow::SacrificeEventName, phase, std::move(details), image)) {
        m_sacrifice.evidence_failed = true;
        if (m_task_ptr) {
            m_task_ptr->set_enable(false);
        }
        Log.error("BlackFlow sacrifice evidence persistence failed", phase);
        return false;
    }
    return true;
}

bool asst::RoguelikeStageEncounterTaskPlugin::handle_sacrifice_event()
{
    using blackflow::SacrificePhase;
    if (m_sacrifice.evidence_failed || m_encounter_choice.failed) {
        return false;
    }
    // 等待服务器换页时不能把仍停留的主事件当成第二轮，也不能重复提交换出。
    if (m_sacrifice.phase == SacrificePhase::AwaitPicker) {
        sleep(500);
        return true;
    }
    if (m_sacrifice.phase == SacrificePhase::AwaitReward) {
        if (m_option_list.size() != 1 || !m_option_list.front().enabled || m_option_list.front().text == "离开") {
            sleep(500);
            return true;
        }
        if (!report_sacrifice(
                "exchange-reward",
                json::object { { "selected_name", m_sacrifice.selected_name },
                               { "reward_option", m_option_list.front().text } },
                ctrler()->get_image())) {
            return false;
        }
        if (!report_sacrifice(
                "reward-claim-attempt",
                { { "selected_name", m_sacrifice.selected_name }, { "reward_option", m_option_list.front().text } },
                ctrler()->get_image())) {
            return false;
        }
        if (select_analyzed_option(0)) {
            if (!report_sacrifice(
                    "reward-claim-click-issued",
                    { { "selected_name", m_sacrifice.selected_name }, { "reward_option", m_option_list.front().text } },
                    ctrler()->get_image())) {
                return false;
            }
            ++m_sacrifice.exchanges;
            m_sacrifice.phase = SacrificePhase::AwaitRepeat;
            m_sacrifice.selected_card.reset();
            m_sacrifice.picker_attempts = { };
            m_sacrifice.selected_name.clear();
            m_sacrifice.initial_choice.reset();
            // 奖励后的第二轮仍是同一事件，复用通用链式事件的文字快进。
            (void)next_event(std::string(blackflow::SacrificeEventName));
        }
        return true;
    }
    if (m_sacrifice.phase == SacrificePhase::AwaitRepeat && m_option_list.size() == 1 &&
        m_option_list.front().text != "离开") {
        // 奖励确认后的迟到画面，等待真正回到两选项的主事件。
        sleep(500);
        return true;
    }

    const auto leave = [&] {
        for (std::size_t i = m_option_list.size(); i > 0; --i) {
            if (m_option_list[i - 1].enabled) {
                return select_analyzed_option(i - 1);
            }
        }
        return false;
    };
    if (m_sacrifice.phase == SacrificePhase::Finished || m_sacrifice.exchanges >= 2) {
        m_sacrifice.phase = SacrificePhase::Finished;
        return leave();
    }

    auto choices = blackflow::sacrifice_initial_choices(m_option_list);
    if (choices.empty()) {
        m_sacrifice.phase = SacrificePhase::Finished;
        return leave();
    }
    const bool civilization = blackflow::is_restore_civilization(m_option_list[choices.front()].text);
    if (civilization && !m_sacrifice.civilization_inventory_scanned) {
        m_sacrifice.phase = SacrificePhase::BeforeCivilization;
        Task.set_task_base(std::string(Prefix) + "StageEncounterResult", std::string(Prefix) + "SacrificeContinuation");
        return true;
    }
    if (!m_sacrifice.initial_choice.has_value()) {
        // 第二轮按用户指定选第一个；初次仅在可选的前两个选项之间抽签。
        m_sacrifice.initial_choice = civilization ? choices.front()
                                     : m_sacrifice.phase == SacrificePhase::AwaitRepeat
                                         ? 0
                                         : choices[random_index(choices.size())];
    }
    const auto index = *m_sacrifice.initial_choice;
    if (index >= m_option_list.size() || !m_option_list[index].enabled) {
        return leave();
    }
    if (!prepare_encounter_choice(blackflow::SacrificeEventName, m_sacrifice.exchanges + 1, m_option_list_image) ||
        !plan_encounter_choice(
            index,
            civilization                ? "restore_civilization"
            : m_sacrifice.exchanges > 0 ? "first_option_second_round"
                                        : "uniform_enabled_first_two")) {
        return false;
    }
    if (!report_sacrifice(
            "event-choice",
            json::object { { "option", m_option_list[index].text }, { "option_index", index + 1 } },
            ctrler()->get_image())) {
        return false;
    }
    if (!select_analyzed_option(index)) {
        return false;
    }
    if (civilization) {
        m_sacrifice.phase = SacrificePhase::AwaitCivilization;
        Task.set_task_base(std::string(Prefix) + "StageEncounterResult", std::string(Prefix) + "SacrificeContinuation");
    }
    else {
        m_sacrifice.phase = SacrificePhase::AwaitPicker;
    }
    return true;
}

bool asst::RoguelikeStageEncounterTaskPlugin::handle_sacrifice_picker()
{
    using blackflow::SacrificePhase;
    if (need_exit()) {
        return false;
    }
    const auto leave_picker = [&]() {
        if (need_exit()) {
            return false;
        }
        ProcessTask back(*this, { std::string(Prefix) + "SacrificeBack" });
        back.set_retry_times(2);
        if (back.run()) {
            return true;
        }
        if (need_exit()) {
            return false;
        }
        // 插件返回 false 不会阻断外层 ProcessTask；退不出选择页时必须显式停用它。
        Log.error("BlackFlow sacrifice picker recovery failed; stopping the task");
        if (m_task_ptr != nullptr) {
            m_task_ptr->set_enable(false);
        }
        callback(AsstMsg::SubTaskError, basic_info_with_what("BlackFlowSacrificeRecoveryFailed"));
        return false;
    };
    if (!refresh_sacrifice_context() || m_sacrifice.phase == SacrificePhase::Finished) {
        return leave_picker();
    }
    if (m_sacrifice.evidence_failed || m_encounter_choice.failed) {
        return false;
    }
    const auto picker_image = ctrler()->get_image();
    if (recognize(picker_image, "SacrificePicker") && !confirm_encounter_choice(picker_image, "sacrifice_picker")) {
        return false;
    }
    if (!m_sacrifice.picker_attempts.begin()) {
        Log.warn("BlackFlow sacrifice picker retry limit reached; leaving the exchange");
        if (!report_sacrifice(
                "picker-recovery",
                json::object { { "reason", "retry_limit_reached" }, { "attempts", m_sacrifice.picker_attempts.count } },
                ctrler()->get_image())) {
            return false;
        }
        m_sacrifice.phase = SacrificePhase::Finished;
        return leave_picker();
    }
    if (m_sacrifice.phase == SacrificePhase::AwaitReward) {
        wait_for_secondary_event(std::string(Prefix) + "SacrificePicker");
        return true;
    }
    // 在当前节点中途识别到选择页也可以续办，但不能在上一轮奖励还未领取时开始下一轮。
    if (m_sacrifice.exchanges >= 2) {
        return false;
    }
    if (!m_sacrifice.selected_card.has_value()) {
        const auto observed = blackflow::perception::sacrifice_item_list(ctrler()->get_image());
        sleep(350);
        const auto image = ctrler()->get_image();
        const auto stable = blackflow::perception::sacrifice_item_list(image);
        if (need_exit()) {
            return false;
        }
        if (!observed.has_value() || !stable.has_value() || *observed != *stable) {
            Log.warn("BlackFlow sacrifice item list unknown or not stable; retrying without assuming it is empty");
            return true;
        }
        if (stable->cards.empty()) {
            if (!report_sacrifice(
                    "picker-recovery",
                    { { "reason", "no_exchangeable_items" },
                      { "attempts", m_sacrifice.picker_attempts.count },
                      { "list_status", "confirmed_empty" },
                      { "confirmation_basis", stable->empty_evidence },
                      { "observed_text", stable->observed_text } },
                    image)) {
                return false;
            }
            m_sacrifice.phase = SacrificePhase::Finished;
            return leave_picker();
        }
        m_sacrifice.selected_card = stable->cards[random_index(stable->cards.size())];
    }
    const Rect card = *m_sacrifice.selected_card;
    const cv::Mat before = ctrler()->get_image();
    if (!recognize(before, "SacrificePicker")) {
        return true;
    }
    if (!blackflow::perception::sacrifice_item_selected(before, card) &&
        !ctrler()->click(Rect { card.x + 20, card.y + 20, 62, 62 })) {
        return false;
    }
    std::string previous;
    for (int sample = 0; sample < 6 && !need_exit(); ++sample) {
        sleep(350);
        const auto image = ctrler()->get_image();
        if (!recognize(image, "SacrificePicker")) {
            return true;
        }
        const std::string name = blackflow::perception::sacrifice_item_selected(image, card)
                                     ? blackflow::perception::sacrifice_selected_name(image)
                                     : "";
        if (name.empty() || name != previous) {
            previous = name;
            continue;
        }
        if (!report_sacrifice("item-selected", json::object { { "selected_name", name } }, image)) {
            return false;
        }
        ProcessTask confirm(*this, { std::string(Prefix) + "SacrificeConfirm" });
        confirm.set_retry_times(0);
        if (confirm.run()) {
            m_sacrifice.selected_name = name;
            m_sacrifice.phase = SacrificePhase::AwaitReward;
            wait_for_secondary_event(std::string(Prefix) + "SacrificePicker");
        }
        return true;
    }
    Log.warn("BlackFlow sacrifice selected item name did not settle; keeping this selection for retry");
    return !need_exit();
}

blackflow::SacrificeInventoryScanResult
    asst::RoguelikeStageEncounterTaskPlugin::scan_sacrifice_natural_items(std::string_view phase)
{
    blackflow::SacrificeInventoryScanResult result;
    const auto& context = m_sacrifice.context;
    const std::string scan_id = "inventory-" + std::to_string(context ? context->run_revision : 0) + "-" +
                                std::to_string(context ? context->page_revision : 0) + "-" +
                                std::to_string(m_sacrifice.evidence_sequence + 1);
    int page = 0, attempt = 0, observed = 0, sample_index = 0;
    std::optional<int> count;
    json::array capacity_readings;
    blackflow::CivilizationInventorySlots last_slots, last_empty;
    std::vector<std::string> naturals;
    std::string failure;
    const auto readings = [](const auto& values) {
        json::array array;
        for (const auto& value : values) array.emplace_back(value.to_json());
        return array;
    };
    const auto slots = [](const auto& values) {
        json::array array;
        for (const auto& [position, value] : values) {
            auto entry = value.to_json();
            entry["column"] = position.first;
            entry["row"] = position.second;
            array.emplace_back(std::move(entry));
        }
        return array;
    };
    const auto save = [&](std::string status, std::string reason, const cv::Mat& image,
                          json::object extra = {}, bool raw = false) {
        extra["scan_id"] = scan_id;
        extra["scan_attempt"] = attempt;
        extra["scan_page"] = page;
        extra["sample_index"] = sample_index;
        extra["inventory_status"] = std::move(status);
        extra["reason"] = reason;
        extra["inventory_count"] = count ? json::value(*count) : json::value(nullptr);
        extra["capacity_ocr"] = capacity_readings;
        extra["scan_complete"] = result.natural_items.has_value();
        extra["panel_closed"] = result.panel_closed;
        extra["observed_count"] = observed;
        extra["observed_natural_items"] = json::array(naturals);
        extra["natural_items"] = result.natural_items ? json::value(json::array(*result.natural_items)) : json::value(nullptr);
        extra["slot_observations"] = slots(last_slots);
        extra["empty_slot_observations"] = slots(last_empty);
        if (raw && !image.empty()) {
            OCRer original(image);
            original.set_task_info(std::string(Prefix) + "MovementInventoryAllItems");
            original.set_required({});
            original.set_fuzzy_match(false);
            original.set_replace({});
            (void)original.analyze();
            extra["raw_ocr"] = readings(original.get_result());
        }
        if (!reason.empty()) failure = std::move(reason);
        return report_sacrifice(phase, std::move(extra), image);
    };
    const auto single_task = [&](std::string_view suffix) {
        if (need_exit() || m_sacrifice.evidence_failed) return false;
        ProcessTask task(*this, {std::string(Prefix) + std::string(suffix)});
        task.set_retry_times(0);
        return task.run();
    };
    const auto set_panel = [&](bool expanded) {
        int consecutive = 0, cooldown = 0, clicks = 0, details_closed = 0;
        cv::Mat image;
        for (int sample = 0; sample < 22 && !need_exit() && !m_sacrifice.evidence_failed; ++sample) {
            sample_index = sample;
            image = ctrler()->get_image();
            if (recognize(image, "SacrificeInventoryDetailsClose")) {
                if (!save("obstructed", "relic_details_overlay", image, {}, true) || details_closed++ >= 1 ||
                    !single_task("SacrificeInventoryDetailsClose")) return false;
                consecutive = 0;
                sleep(300);
                continue;
            }
            const bool is_expanded = recognize(image, "SacrificeInventoryExpanded");
            const bool is_collapsed = !is_expanded && recognize(image, "SacrificeInventoryOpen") &&
                (recognize(image, "StageEncounterOcr") || recognize(image, "MapPrepare-Ready") || recognize(image, "MapPrepare-ZoomOut"));
            consecutive = (expanded ? is_expanded : is_collapsed) ? consecutive + 1 : 0;
            if (consecutive >= 2) return true;
            if (!is_expanded && !is_collapsed && (sample == 0 || sample == 21)) {
                if (!save("ui_unrecognized", "toggle_page_unrecognized", image, {{"target_expanded",expanded}}, true)) return false;
            }
            if ((expanded ? is_collapsed : is_expanded) && cooldown == 0 && clicks < 3) {
                if (!save("toggle_attempt", {}, image, {{"target_expanded",expanded},{"toggle_attempt",clicks+1}})) return false;
                if (!single_task(expanded ? "SacrificeInventoryOpen" : "SacrificeInventoryClose")) {
                    (void)save("toggle_failed", "toggle_input_failed", ctrler()->get_image(), {{"target_expanded",expanded}}, true);
                    return false;
                }
                ++clicks;
                cooldown = 5;
            }
            sleep(300);
            cooldown = std::max(0, cooldown - 1);
        }
        if (!need_exit() && !m_sacrifice.evidence_failed)
            (void)save("toggle_failed", "toggle_timeout", image, {{"target_expanded",expanded}}, true);
        return false;
    };
    if (!set_panel(false) || !set_panel(true)) {
        if (!need_exit() && !m_sacrifice.evidence_failed) {
            (void)save("open_failed", "inventory_open_failed", ctrler()->get_image(), {}, true);
            if (m_task_ptr) m_task_ptr->set_enable(false);
        }
        return result;
    }
    const auto scan = [&]() -> bool {
        const auto capacity_image = ctrler()->get_image();
        OCRer capacity(capacity_image);
        capacity.set_task_info(std::string(Prefix) + "SacrificeInventoryCount");
        if (capacity.analyze()) {
            capacity_readings = readings(capacity.get_result());
            const std::regex fraction(R"(([0-9]+|[Oo])\s*/\s*([0-9]+))");
            for (const auto& value : capacity.get_result()) {
                std::smatch match;
                if (!std::regex_search(value.text, match, fraction)) continue;
                if (match[1].length() > 2 || match[2].length() > 2) { count.reset(); break; }
                const int current = match[1] == "O" || match[1] == "o" ? 0 : std::stoi(match[1].str());
                const int maximum = std::stoi(match[2].str());
                if (maximum <= 0 || current > maximum || (count && *count != current)) {
                    count.reset(); break;
                }
                count = current;
            }
        }
        if (!count || *count > 3 * (2 + blackflow::InventoryMaximumSwipes)) {
            (void)save("incomplete", "capacity_unrecognized", capacity_image, {}, true);
            return false;
        }
        cv::Mat previous_image;
        blackflow::CivilizationInventorySlots previous_page;
        const auto natural_names = Task.get<OcrTaskInfo>(std::string(Prefix) + "InventoryNaturalPriority")->text;
        for (page = 0; page <= blackflow::InventoryMaximumSwipes && !need_exit(); ++page) {
            if (page > 0 && !single_task("MovementInventorySwipe")) {
                (void)save("incomplete", "swipe_input_failed", ctrler()->get_image(), {}, true); return false;
            }
            blackflow::CivilizationInventorySlots prior_names;
            std::optional<blackflow::CivilizationInventorySlots> stable;
            cv::Mat image;
            for (sample_index = 0; sample_index < 5 && !need_exit(); ++sample_index) {
                sleep(300);
                image = ctrler()->get_image();
                if (recognize(image, "SacrificeInventoryDetailsClose")) {
                    (void)save("obstructed", "relic_details_overlay", image, {}, true); return false;
                }
                if (!recognize(image, "SacrificeInventoryExpanded")) continue;
                OCRer names(image);
                names.set_task_info(std::string(Prefix) + "MovementInventoryAllItems");
                (void)names.analyze();
                last_slots = blackflow::civilization_inventory_slots(names.get_result(), image);
                if (sample_index > 0 && blackflow::civilization_slot_layout_equal(prior_names, last_slots) &&
                    (!last_slots.empty() || *count == 0)) {
                    stable = last_slots; break;
                }
                prior_names = last_slots;
            }
            if (!stable) {
                (void)save("incomplete", "names_not_stable_or_page_lost", image, {}, true); return false;
            }
            last_empty.clear();
            if ((page + 2) * 3 > *count) {
                OCRer empty(image);
                empty.set_task_info(std::string(Prefix) + "SacrificeInventoryEmptySlots");
                (void)empty.analyze();
                last_empty = blackflow::civilization_inventory_slots(empty.get_result(), image, true);
            }
            if (!blackflow::civilization_inventory_page_complete(*stable, last_empty, page, *count)) {
                (void)save("incomplete", "column_coverage_incomplete", image, {}, true); return false;
            }
            if (page > 0 && !blackflow::civilization_inventory_advanced_one_column(previous_image, previous_page, image, *stable)) {
                (void)save("incomplete", "column_advance_not_verified", image, {}, true); return false;
            }
            for (const auto& [slot, name] : *stable) {
                if (page > 0 && slot.first == 0) continue;
                ++observed;
                if (std::ranges::find(natural_names, name.text) != natural_names.end()) naturals.push_back(name.text);
            }
            if (!save("page_observed", {}, image)) return false;
            if (observed == *count) { result.natural_items = naturals; return true; }
            previous_image = image.clone();
            previous_page = *stable;
        }
        if (!need_exit()) (void)save("incomplete", "scan_limit_reached", ctrler()->get_image(), {}, true);
        return false;
    };
    for (attempt = 1; attempt <= 2 && !need_exit(); ++attempt) {
        page = observed = 0; naturals.clear(); count.reset(); last_slots.clear(); last_empty.clear();
        if (scan() || m_sacrifice.evidence_failed || failure != "relic_details_overlay") break;
        if (attempt == 2 || !set_panel(false) || !set_panel(true)) break;
    }
    if (m_sacrifice.evidence_failed || need_exit()) return result;
    result.panel_closed = set_panel(false);
    if (m_sacrifice.evidence_failed) return result;
    if (!save(result.natural_items ? "complete" : "incomplete", result.panel_closed ? std::string() : "inventory_close_failed", ctrler()->get_image())) return result;
    if (!result.panel_closed && m_task_ptr) m_task_ptr->set_enable(false);
    return result;
}


bool asst::RoguelikeStageEncounterTaskPlugin::finish_sacrifice_civilization()
{
    using blackflow::SacrificePhase;
    if (m_sacrifice.evidence_failed || m_encounter_choice.failed) {
        return false;
    }
    const auto continue_later = [&] {
        Task.set_task_base(std::string(Prefix) + "StageEncounterResult", std::string(Prefix) + "SacrificeContinuation");
        return !need_exit();
    };
    if (m_sacrifice.phase == SacrificePhase::BeforeCivilization) {
        const auto scan = scan_sacrifice_natural_items("civilization-before");
        m_sacrifice.naturals_before = scan.natural_items;
        if (!scan.panel_closed) return false;
        if (m_sacrifice.evidence_failed) {
            return false;
        }
        if (recognize(ctrler()->get_image(), "SacrificeInventoryExpanded")) {
            return continue_later();
        }
        m_sacrifice.civilization_inventory_scanned = true;
        m_sacrifice.phase = m_sacrifice.exchanges > 0 ? SacrificePhase::AwaitRepeat : SacrificePhase::Initial;
        m_sacrifice.initial_choice.reset();
        return true;
    }

    for (int sample = 0; sample < 16 && !need_exit(); ++sample) {
        sleep(400);
        const auto image = ctrler()->get_image();
        bool popup = false;
        for (const auto suffix : { "SacrificePopupClose", "SacrificePopupContinue" }) {
            if (recognize(image, suffix)) {
                m_sacrifice.civilization_transition_seen = true;
                if (!confirm_encounter_choice(image, "civilization_popup") ||
                    !report_sacrifice("civilization-popup", { }, image)) {
                    return false;
                }
                ProcessTask close(*this, { std::string(Prefix) + suffix });
                close.set_retry_times(0);
                (void)close.run();
                popup = true;
                break;
            }
        }
        if (popup) {
            continue;
        }
        const bool map = recognize(image, "MapPrepare-Ready") || recognize(image, "MapPrepare-ZoomOut");
        if (!m_sacrifice.civilization_transition_seen && !map && recognize(image, "StageEncounterOcr")) {
            OptionAnalyzer options(image);
            options.set_theme(std::string(RoguelikeTheme::BlackFlow));
            if (options.analyze() && options.get_result().size() == 1 &&
                !blackflow::is_restore_civilization(options.get_result().front().text)) {
                m_sacrifice.civilization_transition_seen = true;
            }
        }
        m_sacrifice.civilization_transition_seen |= map;
        if (!m_sacrifice.civilization_transition_seen) {
            continue;
        }
        if (!confirm_encounter_choice(image, map ? "map_after_civilization" : "civilization_options_changed")) {
            return false;
        }
        const auto scan = scan_sacrifice_natural_items("civilization-after");
        auto after = scan.natural_items;
        if (!scan.panel_closed) return false;
        if (m_sacrifice.evidence_failed) {
            return false;
        }
        if (recognize(ctrler()->get_image(), "SacrificeInventoryExpanded")) {
            return continue_later();
        }
        json::object details {
            { "before",
              m_sacrifice.naturals_before.has_value() ? json::value(json::array(*m_sacrifice.naturals_before))
                                                      : json::value(nullptr) },
            { "after", after.has_value() ? json::value(json::array(*after)) : json::value(nullptr) },
            { "comparison_status", "incomplete" },
        };
        if (m_sacrifice.naturals_before.has_value() && after.has_value()) {
            const auto removed = blackflow::removed_natural_items(*m_sacrifice.naturals_before, *after);
            const auto added = blackflow::removed_natural_items(*after, *m_sacrifice.naturals_before);
            details["removed"] = json::array(removed);
            details["added"] = json::array(added);
            details["comparison_status"] =
                removed.size() == 2 && added.empty() ? "confirmed_two" : "unexpected_difference";
        }
        if (!report_sacrifice("civilization-consumed", std::move(details), ctrler()->get_image())) {
            return false;
        }
        m_sacrifice.phase = SacrificePhase::Finished;
        return true;
    }
    return continue_later();
}
