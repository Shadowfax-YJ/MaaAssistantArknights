#include "RoguelikeStageEncounterTaskPlugin.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include "Config/Roguelike/RoguelikeStageEncounterConfig.h"
#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "MaaUtils/ImageIo.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Task/ProcessTask.h"
#include "Task/Roguelike/Map/RoguelikeBoskyPassageMap.h"
#include "Utils/DebugImageHelper.hpp"
#include "Utils/FuzzyTextMatcher.h"
#include "Utils/Logger.hpp"
#include "Vision/Matcher.h"
#include "Vision/Miscellaneous/PipelineAnalyzer.h"
#include "Vision/RegionOCRer.h"
#include "Vision/Roguelike/BlackFlow/HudOcr.h"

bool asst::RoguelikeStageEncounterTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    // 安全屋，掷骰子之类的带选项的也都是视为不期而遇了
    if (msg != AsstMsg::SubTaskStart || details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }

    if (!RoguelikeConfig::is_valid_theme(m_config->get_theme())) {
        Log.error("Roguelike name doesn't exist!");
        return false;
    }
    const std::string roguelike_name = m_config->get_theme() + "@";
    const std::string& task = details.get("details", "task", "");
    std::string_view task_view = task;
    if (task_view.starts_with(roguelike_name)) {
        task_view.remove_prefix(roguelike_name.length());
    }
    if (task_view == "Roguelike@StageEncounterJudgeOption" ||
        (m_config->get_theme() == RoguelikeTheme::BlackFlow &&
         (task_view == "Roguelike@ExpeditionSelectOper" || task_view == "Roguelike@SacrificeSelectItem" ||
          task_view == "Roguelike@SacrificeContinue"))) {
        return true;
    }
    else {
        return false;
    }
}

void asst::RoguelikeStageEncounterTaskPlugin::reset_in_run_variables()
{
    m_sacrifice = { };
    m_encounter_choice = { };
    m_expedition_context.reset();
    m_expedition_pending_operator.clear();
    m_expedition_finished = false;
    m_expedition_initial_choice.reset();
    m_expedition_evidence = { };
    m_lake_fairy_plan.reset();
    m_lake_fairy_initial_choice_index = 0;
    m_lake_fairy_unique_choice_selected = false;
}

void asst::RoguelikeStageEncounterTaskPlugin::refresh_expedition_context()
{
    const auto context = m_expedition_context_provider ? m_expedition_context_provider() : std::nullopt;
    if (!m_expedition_pending_operator.empty() &&
        (!context.has_value() || context->floor != 2 || !m_expedition_context.has_value() ||
         context->run_revision != m_expedition_context->run_revision ||
         context->page_revision != m_expedition_context->page_revision)) {
        (void)abort_expedition("dispatch_context_changed");
        return;
    }
    if (!context.has_value() || context->floor != 2) {
        m_expedition_context.reset();
        m_expedition_pending_operator.clear();
        m_expedition_finished = false;
        m_expedition_initial_choice.reset();
        m_expedition_evidence = { };
        return;
    }
    if (!m_expedition_context.has_value() || context->run_revision != m_expedition_context->run_revision ||
        context->page_revision != m_expedition_context->page_revision) {
        m_expedition_pending_operator.clear();
        m_expedition_finished = false;
        m_expedition_initial_choice.reset();
        m_expedition_evidence = { };
        m_expedition_context = context;
        // 安全天猫、伊桑、古米按固定顺序尝试，识别重试不改变本次候选。
        m_expedition_context->operators =
            blackflow::prioritized_expedition_operator_order(m_expedition_context->operators);
        m_expedition_evidence.details["operator_selection_policy"] = "unpromoted_core_then_ethan_then_gummy";
        Log.info(
            "BlackFlow expedition fixed operator priority (safe unpromoted core, Ethan, Gummy)",
            json::array(m_expedition_context->operators));
    }
}

bool asst::RoguelikeStageEncounterTaskPlugin::report_expedition(
    std::string_view phase,
    json::object details,
    const cv::Mat& image)
{
    if (m_expedition_evidence.failed || !m_expedition_context.has_value()) {
        return false;
    }
    auto payload = m_expedition_evidence.details;
    for (const auto& [key, value] : details) {
        payload[key] = value;
    }
    const auto& context = *m_expedition_context;
    const std::string id =
        "expedition-" + std::to_string(context.run_revision) + "-" + std::to_string(context.page_revision);
    payload["expedition_id"] = id;
    payload["run_revision"] = context.run_revision;
    payload["page_revision"] = context.page_revision;
    payload["expedition_phase"] = std::string(phase);
    payload["evidence_sequence"] = ++m_expedition_evidence.sequence;
    payload["choice_attempt"] = m_expedition_evidence.choice_attempt;
    payload["dispatch_attempt"] = m_expedition_evidence.dispatch_attempt;
    if (!image.empty()) {
        const auto capture_id = id + ":" + std::to_string(m_expedition_evidence.sequence);
        payload["capture_id"] = capture_id;
        if (phase == "candidates" || phase == "dispatch_prepared") {
            const std::string key = phase == "candidates" ? "candidate_capture_id" : "preparation_capture_id";
            payload[key] = capture_id;
            m_expedition_evidence.details[key] = capture_id;
        }
    }
    if (!m_expedition_evidence_observer || !m_expedition_evidence_observer(phase, payload, image)) {
        Log.error("BlackFlow expedition evidence persistence failed", phase, id);
        m_expedition_evidence.failed = true;
        m_task_ptr->set_enable(false);
        return false;
    }
    Log.info("BlackFlow expedition evidence", phase, json::value(std::move(payload)));
    return true;
}

bool asst::RoguelikeStageEncounterTaskPlugin::abort_expedition(std::string_view reason)
{
    (void)report_expedition("unresolved", { { "reason", std::string(reason) } });
    m_expedition_evidence.failed = true;
    m_task_ptr->set_enable(false);
    return false;
}

bool asst::RoguelikeStageEncounterTaskPlugin::report_encounter_choice(
    std::string_view phase,
    json::object details,
    const cv::Mat& image)
{
    auto& choice = m_encounter_choice;
    if (choice.failed || !choice.context) {
        return false;
    }
    auto payload = choice.details;
    for (const auto& [key, value] : details) {
        payload[key] = value;
    }
    const auto& context = *choice.context;
    payload["choice_id"] = "choice-" + std::to_string(context.run_revision) + "-" +
                           std::to_string(context.page_revision) + "-" + std::to_string(choice.round);
    if (choice.step > 0) {
        payload["choice_id"] = payload.get("choice_id", std::string()) + "-step-" + std::to_string(choice.step);
        payload["choice_step"] = choice.step;
    }
    if (choice.generic) payload["acquisition_status"] = "unresolved";
    if (!image.empty()) {
        const std::string capture_id =
            payload.get("choice_id", std::string()) + ":" + std::to_string(choice.sequence + 1);
        payload["capture_id"] = capture_id;
        if (phase == "candidates") {
            choice.details["candidate_capture_id"] = capture_id;
            payload["candidate_capture_id"] = capture_id;
        }
    }
    payload["run_revision"] = context.run_revision;
    payload["page_revision"] = context.page_revision;
    payload["exchange_round"] = choice.round;
    payload["choice_phase"] = std::string(phase);
    payload["capture_kind"] = "encounter_choice";
    payload["evidence_sequence"] = ++choice.sequence;
    payload["choice_attempt"] = choice.attempt;
    if (!m_event_detail_observer || !m_event_detail_observer(choice.event_name, phase, payload, image)) {
        choice.failed = true;
        if (m_task_ptr) {
            m_task_ptr->set_enable(false);
        }
        Log.error("BlackFlow encounter choice evidence persistence failed", choice.event_name, phase);
        return false;
    }
    if (phase == "unresolved") {
        choice.failed = true;
        if (m_task_ptr) {
            m_task_ptr->set_enable(false);
        }
        return false;
    }
    return true;
}

void asst::RoguelikeStageEncounterTaskPlugin::refresh_encounter_choice_context()
{
    if (!m_encounter_choice.context) {
        return;
    }
    const auto current = m_sacrifice_context_provider ? m_sacrifice_context_provider() : std::nullopt;
    if (m_encounter_choice.context != current) {
        // A battle or another special event need not provide a generic result receipt.
        // Keep that old evidence unresolved in its original node; never report it in
        // the next transaction, nor let its selected index capture a new event's input.
        Log.info(
            "BlackFlow retire encounter choice after context change",
            m_encounter_choice.event_name,
            m_encounter_choice.context->run_revision,
            m_encounter_choice.context->page_revision);
        m_encounter_choice = { };
    }
}

bool asst::RoguelikeStageEncounterTaskPlugin::prepare_encounter_choice(
    std::string_view event_name,
    int round,
    const cv::Mat& image)
{
    const auto context = m_sacrifice_context_provider ? m_sacrifice_context_provider() : std::nullopt;
    if (!context) {
        return true; // Other BlackFlow modes retain their existing flow.
    }
    auto& choice = m_encounter_choice;
    const bool same_context = choice.context == context;
    const bool same = same_context && choice.event_name == event_name && choice.round == round;
    const bool generic = event_name != "货从口出" && event_name != blackflow::SacrificeEventName && event_name != "失与得" &&
                         event_name != blackflow::ExpeditionEventName;
    if (same && !generic) return !choice.failed;
    if (same_context && choice.failed) return false;
    bool changed = same_context && choice.details.at("candidates").as_array().size() != m_option_list.size();
    for (size_t i = 0; same_context && !changed && i < m_option_list.size(); ++i) {
        changed = choice.details.at("candidates").as_array()[i].get("option", std::string()) != m_option_list[i].text;
    }
    if (generic && same_context && !same && choice.clicked && !choice.confirmed) {
        choice.details["result_event_name"] = std::string(event_name);
        if (changed) {
            if (!confirm_encounter_choice(image, "next_event_candidate_page")) return false;
        }
        else if (!report_encounter_choice("result_observed", {{"result_status","unresolved"},
                     {"result_basis","next_event_candidate_page"}}, image)) return false;
    }
    if (same && !choice.confirmed) {
        if (!changed || !choice.clicked) return true;
        if (!confirm_encounter_choice(image, "next_candidate_page")) return false;
    }
    const int step = generic && same_context ? choice.step + 1 : 0;
    const bool parent_confirmed = choice.confirmed;
    const std::string parent_id = choice.context ?
        "choice-" + std::to_string(choice.context->run_revision) + "-" + std::to_string(choice.context->page_revision) +
        "-" + std::to_string(choice.round) + (choice.step > 0 ? "-step-" + std::to_string(choice.step) : "") : "";
    choice = { };
    choice.context = context;
    choice.event_name = event_name;
    choice.round = round;
    choice.step = step;
    choice.generic = generic;
    if (generic && same_context && !parent_id.empty()) {
        choice.details["parent_choice_id"] = parent_id;
        choice.details["parent_choice_status"] = parent_confirmed ? "confirmed" : "unresolved";
    }
    json::array candidates;
    for (size_t i = 0; i < m_option_list.size(); ++i) {
        const auto& option = m_option_list[i];
        candidates.emplace_back(
            json::object {
                { "option_index", i + 1 },
                { "option", option.text },
                { "enabled", option.enabled },
                { "red_description", option.red_description },
            });
    }
    choice.details["candidates"] = std::move(candidates);
    return report_encounter_choice("candidates", { }, image);
}

bool asst::RoguelikeStageEncounterTaskPlugin::plan_encounter_choice(size_t index, std::string_view policy)
{
    auto& choice = m_encounter_choice;
    if (!choice.context || choice.confirmed) {
        return true;
    }
    if (choice.failed || index >= m_option_list.size()) {
        return false;
    }
    if (choice.selected && choice.selected != index && choice.clicked) {
        return report_encounter_choice("unresolved", { { "reason", "prior_choice_input_unresolved" } });
    }
    choice.selected = index;
    choice.clicked = false;
    choice.capture_clicks = true;
    choice.details["option_index"] = index + 1;
    choice.details["option"] = m_option_list[index].text;
    choice.details["selection_policy"] = std::string(policy);
    return report_encounter_choice("planned");
}

bool asst::RoguelikeStageEncounterTaskPlugin::randomize_encounter_choice_order(std::vector<size_t>& order)
{
    auto& choice = m_encounter_choice;
    if (choice.generic && choice.context && !choice.confirmed) {
        auto eligible = order;
        std::ranges::sort(eligible);
        if (const auto previous = choice.group_orders.find(eligible); previous != choice.group_orders.end()) {
            order = previous->second;
            return !choice.failed;
        }
        static thread_local std::mt19937 random_engine(std::random_device { }());
        std::shuffle(order.begin(), order.end(), random_engine);
        choice.group_orders.emplace(eligible, order);
        choice.details["randomized_option_order"] = json::array(order);
        return report_encounter_choice("order", {{"eligible_option_indices",json::array(eligible)}});
    }
    if (choice.context && !choice.confirmed && !choice.order.empty()) {
        order = choice.order;
        return !choice.failed;
    }
    static thread_local std::mt19937 random_engine(std::random_device { }());
    std::shuffle(order.begin(), order.end(), random_engine);
    if (!choice.context || choice.confirmed) {
        return true;
    }
    choice.order = order;
    choice.details["randomized_option_order"] = json::array(order);
    return report_encounter_choice("order");
}

bool asst::RoguelikeStageEncounterTaskPlugin::confirm_encounter_choice(const cv::Mat& image, std::string_view basis)
{
    auto& choice = m_encounter_choice;
    if (!choice.context || choice.confirmed || !choice.selected || !choice.clicked) {
        return true;
    }
    if (!report_encounter_choice("confirmed", { { "confirmation_basis", std::string(basis) } }, image)) {
        return false;
    }
    choice.confirmed = true;
    return true;
}

bool asst::RoguelikeStageEncounterTaskPlugin::observe_encounter_choice_result(const cv::Mat& image)
{
    refresh_encounter_choice_context();
    auto& choice = m_encounter_choice;
    if (!choice.generic || !choice.context || !choice.clicked || choice.confirmed || choice.failed) return !choice.failed;
    if (image.empty()) {
        return report_encounter_choice("result_pending", {{"result_status","unresolved"}, {"reason","empty_result_capture"}});
    }
    if (!report_encounter_choice("result_observed", { { "result_status", "unresolved" } }, image)) return false;
    for (const auto task : { "LoadingText", "BlackFlow@Roguelike@DataSyncConfirm" }) {
        OCRer overlay(image); overlay.set_task_info(task);
        if (overlay.analyze()) return true;
    }
    for (const auto task : { "BlackFlow@Roguelike@CloseCollection", "BlackFlow@Roguelike@CloseCollectionContinue",
                            "BlackFlow@Roguelike@MapPrepare-Ready", "BlackFlow@Roguelike@MapPrepare-ZoomOut" }) {
        Matcher result(image); result.set_task_info(task);
        if (result.analyze()) return confirm_encounter_choice(image, task);
    }
    // A successful input or an absent confirmation button cannot prove an award.
    // The next candidates / reward observer may provide the independent receipt.
    return true;
}

bool asst::RoguelikeStageEncounterTaskPlugin::confirm_cargo_choice()
{
    if (!m_encounter_choice.context || m_encounter_choice.confirmed) {
        return true;
    }
    std::vector<std::string> previous;
    int reward_frames = 0;
    int ending_frames = 0;
    const bool declined = m_encounter_choice.clicked &&
                          m_encounter_choice.details.get("option", std::string()) == "没什么想买的";
    for (int sample = 0; sample < 12 && !need_exit(); ++sample) {
        sleep(350);
        const auto image = ctrler()->get_image();
        if (image.empty()) {
            ending_frames = 0;
            continue;
        }
        OCRer loading(image);
        loading.set_task_info("LoadingText");
        OCRer syncing(image);
        syncing.set_task_info("BlackFlow@Roguelike@DataSyncConfirm");
        if (loading.analyze() || syncing.analyze()) {
            reward_frames = 0;
            ending_frames = 0;
            continue;
        }
        Matcher reward(image);
        reward.set_task_info("BlackFlow@Roguelike@CloseCollection");
        reward_frames = reward.analyze() ? reward_frames + 1 : 0;
        if (reward_frames >= 2) {
            return confirm_encounter_choice(image, "reward_popup_after_choice");
        }
        if (declined) {
            Matcher ending(image);
            ending.set_task_info("BlackFlow@Roguelike@CloseEventAfterEncounter");
            ending_frames = ending.analyze() ? ending_frames + 1 : 0;
            if (ending_frames >= 2) {
                return confirm_encounter_choice(image, "event_ending_after_decline");
            }
            if (ending_frames != 0) {
                // Leave the check untouched for the original ending dispatcher.
                continue;
            }
        }
        bool map = false;
        for (const auto task : { "BlackFlow@Roguelike@MapPrepare-Ready", "BlackFlow@Roguelike@MapPrepare-ZoomOut" }) {
            Matcher matcher(image);
            matcher.set_task_info(task);
            if (matcher.analyze()) {
                map = true;
            }
        }
        if (map) {
            return confirm_encounter_choice(image, "map_after_choice");
        }
        OptionAnalyzer options(image);
        options.set_theme(std::string(RoguelikeTheme::BlackFlow));
        std::vector<std::string> current;
        if (options.analyze()) {
            for (const auto& option : options.get_result()) {
                current.push_back(option.text);
            }
        }
        std::vector<std::string> original;
        for (const auto& option : m_option_list) {
            original.push_back(option.text);
        }
        const bool new_option = std::ranges::any_of(current, [&](const auto& text) {
            return std::ranges::find(original, text) == original.end();
        });
        if (new_option && current == previous) {
            return confirm_encounter_choice(image, "changed_options_stable_twice");
        }
        previous = std::move(current);
        // Passive observation only: never advance through an unarchived reward.
    }
    return report_encounter_choice("unresolved", { { "reason", "cargo_choice_transition_not_confirmed" } });
}

bool asst::RoguelikeStageEncounterTaskPlugin::capture_expedition_candidates(const cv::Mat& image)
{
    if (m_expedition_evidence.candidates_saved) {
        return true;
    }
    json::array candidates;
    for (size_t index = 0; index < m_option_list.size(); ++index) {
        const auto& option = m_option_list[index];
        candidates.emplace_back(
            json::object { { "option_index_from_top", static_cast<int>(index + 1) },
                           { "enabled", option.enabled },
                           { "text", option.text },
                           { "red_description", option.red_description } });
    }
    if (image.empty()) {
        return abort_expedition("candidate_capture_empty");
    }
    if (!report_expedition("candidates", { { "candidates", std::move(candidates) } }, image)) {
        return false;
    }
    m_expedition_evidence.candidates_saved = true;
    return true;
}

bool asst::RoguelikeStageEncounterTaskPlugin::handle_expedition_choice()
{
    if (!m_expedition_evidence.candidates_saved) {
        return abort_expedition("initial_candidates_not_saved");
    }
    const auto leave = [&](std::string_view reason) {
        if (m_option_list.empty() || !m_option_list.back().enabled) {
            return abort_expedition("leave_option_unavailable");
        }
        const auto index = m_option_list.size() - 1;
        if (!report_expedition(
                "cancelled",
                { { "reason", std::string(reason) },
                  { "dispatch_was_attempted", false },
                  { "leave_option_index_from_top", index + 1 },
                  { "leave_option_text", m_option_list[index].text } },
                ctrler()->get_image())) {
            return false;
        }
        m_expedition_finished = true;
        if (!select_analyzed_option(index)) {
            return abort_expedition(need_exit() ? "user_stopped_during_leave" : "leave_choice_not_accepted");
        }
        return true;
    };
    if (m_expedition_context->operators.empty()) {
        return leave("no_eligible_expedition_operator");
    }
    if (!m_expedition_initial_choice.has_value()) {
        if (m_option_list.size() < 2 || !m_option_list.front().enabled) {
            return leave("first_expedition_option_unavailable");
        }
        m_expedition_initial_choice = 0;
        const auto index = *m_expedition_initial_choice;
        m_expedition_evidence.details["selected_option_index_from_top"] = static_cast<int>(index + 1);
        m_expedition_evidence.details["selected_option_text"] = m_option_list[index].text;
        m_expedition_evidence.details["selected_option_red_description"] = m_option_list[index].red_description;
        // Preserve the actual OCR text. The branch label is a text-backed mapping, not an index assumption.
        const auto& text = m_option_list[index].text;
        const bool interior = text.find("内部") != std::string::npos;
        const bool source = text.find("源头") != std::string::npos;
        m_expedition_evidence.details["selected_branch"] =
            interior == source ? json::value(nullptr) : json::value(interior ? "interior" : "source");
        m_expedition_evidence.details["selection_policy"] = "fixed_first_option";
        m_expedition_evidence.details["operator_selection_policy"] = "unpromoted_core_then_ethan_then_gummy";
        m_expedition_evidence.details["operator_preference_order"] = json::array(m_expedition_context->operators);
        if (!report_expedition("choice_planned")) {
            return false;
        }
    }
    const auto selected_index = *m_expedition_initial_choice;
    if (selected_index >= m_option_list.size() || !m_option_list[selected_index].enabled ||
        m_option_list[selected_index].text !=
            m_expedition_evidence.details.get("selected_option_text", std::string())) {
        return abort_expedition("planned_choice_changed_before_click");
    }
    if (!select_analyzed_option(*m_expedition_initial_choice)) {
        return abort_expedition(need_exit() ? "user_stopped_during_choice" : "choice_click_not_accepted");
    }
    // Only the actual picker will confirm this choice; the leave-confirm task is not a dispatch receipt.
    return true;
}

bool asst::RoguelikeStageEncounterTaskPlugin::confirm_expedition_dispatch(const cv::Mat& image, std::string_view basis)
{
    if (m_expedition_pending_operator.empty()) {
        return true;
    }
    if (!m_expedition_evidence.choice_confirmed || image.empty() ||
        (basis != "secondary_event" && basis != "map_after_depart")) {
        return abort_expedition("dispatch_confirmation_missing");
    }
    if (!report_expedition("dispatch_confirmed", { { "confirmation_basis", std::string(basis) } }, image)) {
        return false;
    }
    // Persist the immutable pre-dispatch snapshot before the existing rule-based promotion updates runtime state.
    Log.info("BlackFlow expedition dispatch confirmed", m_expedition_pending_operator);
    auto& operators = m_config->status().opers;
    if (const auto dispatched = operators.find(m_expedition_pending_operator); dispatched != operators.end()) {
        const int elite = blackflow::confirmed_expedition_core_elite(
            m_expedition_initial_choice,
            m_expedition_pending_operator,
            dispatched->second.elite);
        if (elite != dispatched->second.elite) {
            dispatched->second.elite = elite;
            Log.info("BlackFlow expedition core promotion confirmed", m_expedition_pending_operator, "elite", elite);
        }
    }
    if (m_expedition_dispatch_observer) {
        m_expedition_dispatch_observer(m_expedition_pending_operator);
    }
    m_expedition_pending_operator.clear();
    m_expedition_finished = true;
    return true;
}

bool asst::RoguelikeStageEncounterTaskPlugin::wait_for_secondary_event(std::string_view picker_task)
{
    for (int attempt = 0; attempt < 20 && !need_exit(); ++attempt) {
        sleep(400);
        if (need_exit()) {
            return false;
        }
        const cv::Mat image = ctrler()->get_image();
        const bool expedition = picker_task == "BlackFlow@Roguelike@ExpeditionPicker";
        if (image.empty()) {
            continue;
        }
        if (expedition) {
            // A still-open picker or network overlay takes precedence over background event/map matches.
            Matcher still_picking(image);
            still_picking.set_task_info(std::string(picker_task));
            OCRer waiting(image);
            waiting.set_task_info("LoadingText");
            if (still_picking.analyze() || waiting.analyze()) {
                continue;
            }
            OCRer syncing(image);
            syncing.set_task_info("BlackFlow@Roguelike@DataSyncConfirm");
            if (syncing.analyze()) {
                if (!report_expedition(
                        "transition_before_advance",
                        { { "reason", "data_sync_popup" }, { "advance_was_attempted", false } },
                        image)) {
                    return false;
                }
                return abort_expedition("data_sync_interrupted_dispatch");
            }
            // The direct ending has a different title layout from an event with options.
            // Confirm its own title and explicit departure text before handing its checkbox
            // back to the existing settlement graph; do not click through it here.
            Matcher ending(image);
            ending.set_task_info("BlackFlow@Roguelike@CloseEventAfterEncounter");
            if (ending.analyze()) {
                OCRer title(image);
                title.set_task_info("BlackFlow@Roguelike@ExpeditionEndingTitle");
                OCRer departed(image);
                departed.set_task_info("BlackFlow@Roguelike@ExpeditionEndingDeparture");
                departed.set_required({ });
                if (title.analyze() && title.get_result().size() == 1 &&
                    title.get_result().front().text == blackflow::ExpeditionEventName &&
                    title.get_result().front().score >= 0.9 && departed.analyze() &&
                    std::ranges::any_of(departed.get_result(), [](const auto& text) {
                        return text.score >= 0.9 && text.text.find("你的同伴出发了") != std::string::npos;
                    })) {
                    std::string story_text;
                    bool readable = true;
                    for (const auto& text : departed.get_result()) {
                        story_text += text.text;
                        readable = readable && text.score >= 0.9;
                    }
                    json::object details {
                        { "secondary_event_layout", "ending" },
                        { "ending_title", title.get_result().front().text },
                        { "recognition_scope", "ending_title_and_body_roi" },
                    };
                    if (readable) {
                        details["story_text"] = std::move(story_text);
                    }
                    if (!report_expedition("secondary_event", std::move(details), image)) {
                        return false;
                    }
                    m_expedition_evidence.confirmation_image = image.clone();
                    m_expedition_evidence.confirmation_basis = "secondary_event";
                    return true;
                }
                // A recognized ending from an unrelated event is not a dispatch receipt.
                continue;
            }
        }
        OCRer event(image);
        event.set_task_info("BlackFlow@Roguelike@StageEncounterOcr");
        if (event.analyze()) {
            if (expedition) {
                if (event.get_result().empty() || event.get_result().front().text != blackflow::ExpeditionEventName) {
                    continue;
                }
                if (!report_expedition("secondary_event", { }, image)) {
                    return false;
                }
                m_expedition_evidence.confirmation_image = image.clone();
                m_expedition_evidence.confirmation_basis = "secondary_event";
            }
            return true;
        }
        for (const std::string task :
             { "BlackFlow@Roguelike@MapPrepare-Ready", "BlackFlow@Roguelike@MapPrepare-ZoomOut" }) {
            Matcher map(image);
            map.set_task_info(task);
            if (map.analyze()) {
                if (expedition) {
                    m_expedition_evidence.confirmation_image = image.clone();
                    m_expedition_evidence.confirmation_basis = "map_after_depart";
                }
                return true;
            }
        }
        PipelineAnalyzer picker(image);
        picker.set_tasks({ std::string(picker_task) });
        OCRer loading(image);
        loading.set_task_info("LoadingText");
        if (picker.analyze() || loading.analyze()) {
            continue;
        }
        // Keep the bounded fast-forward recovery, but save every screen BEFORE it can be left.
        // An unidentified transition is evidence only, never a secondary-event or dispatch confirmation.
        if (expedition) {
            if (!report_expedition("transition_before_advance", { }, image) || need_exit()) {
                return false;
            }
            // next_event may issue several clicks internally. One click per captured frame prevents
            // it from passing a newly appeared story page without preserving that page first.
            if (!ctrler()->click(Task.get("Roguelike@StageEncounterJudgeClick")->specific_rect)) {
                return abort_expedition("transition_advance_input_failed");
            }
            continue;
        }
        // 派遣/交换页消失后复用通用事件的中央快进动作。不能等标题播放完才开始快进，
        // 也不能再次点击派遣或换出；网络延迟时保持原有待确认状态。
        (void)next_event(
            std::string(
                picker_task == "BlackFlow@Roguelike@ExpeditionPicker" ? blackflow::ExpeditionEventName
                                                                      : blackflow::SacrificeEventName));
    }
    return false;
}

bool asst::RoguelikeStageEncounterTaskPlugin::handle_expedition_picker()
{
    const auto await_dispatch = [&]() {
        if (wait_for_secondary_event("BlackFlow@Roguelike@ExpeditionPicker")) {
            return confirm_expedition_dispatch(
                m_expedition_evidence.confirmation_image,
                m_expedition_evidence.confirmation_basis);
        }
        return abort_expedition(need_exit() ? "user_stopped_after_depart" : "dispatch_confirmation_timeout");
    };
    if (m_expedition_evidence.failed) {
        return false;
    }
    if (!m_expedition_pending_operator.empty()) {
        return await_dispatch();
    }
    if (m_expedition_context.has_value() && m_expedition_context->floor == 2 && !m_expedition_finished) {
        if (!m_expedition_initial_choice.has_value() || !m_expedition_evidence.choice_clicked) {
            return abort_expedition("picker_without_recorded_choice");
        }
        if (!m_expedition_evidence.choice_confirmed) {
            if (!report_expedition("choice_confirmed", { { "confirmation_basis", "expedition_picker" } })) {
                return false;
            }
            m_expedition_evidence.choice_confirmed = true;
        }
        for (const std::string& candidate : m_expedition_context->operators) {
            for (int slot = 0; slot < 5 && !need_exit(); ++slot) {
                const cv::Mat picker_image = ctrler()->get_image();
                if (picker_image.empty()) {
                    return abort_expedition("picker_image_empty");
                }
                Matcher picker(picker_image);
                picker.set_task_info("BlackFlow@Roguelike@ExpeditionPicker");
                if (!picker.analyze()) {
                    m_expedition_finished = true;
                    return report_expedition(
                        "cancelled",
                        { { "reason", "picker_closed_before_depart" }, { "dispatch_was_attempted", false } });
                }
                const auto portrait = Task.get("BlackFlow@Roguelike@ExpeditionFirstOper")->specific_rect;
                Rect click = portrait;
                click.x += slot * 113;
                if (!ctrler()->click(click)) {
                    return false;
                }
                std::string previous;
                for (int sample = 0; sample < 4 && !need_exit(); ++sample) {
                    sleep(350);
                    const cv::Mat selected_image = ctrler()->get_image();
                    OCRer description(selected_image);
                    description.set_task_info("BlackFlow@Roguelike@ExpeditionOperDescription");
                    std::string name;
                    if (description.analyze()) {
                        for (const auto& result : description.get_result()) {
                            name = blackflow::expedition_operator_from_description(result.text);
                            if (!name.empty()) {
                                break;
                            }
                        }
                    }
                    if (name.empty() || name != previous) {
                        previous = std::move(name);
                        continue;
                    }
                    if (name != candidate) {
                        break;
                    }
                    // 两帧姓名一致且命中本次抽中的干员，再单独识别右下角按钮。
                    Log.info("BlackFlow expedition operator verified", name, "slot", slot + 1);
                    Matcher selected_picker(selected_image);
                    selected_picker.set_task_info("BlackFlow@Roguelike@ExpeditionPicker");
                    if (selected_image.empty() || !selected_picker.analyze()) {
                        return abort_expedition("selected_picker_disappeared");
                    }
                    auto& evidence = m_expedition_evidence.details;
                    evidence["operator_name"] = name;
                    evidence["operator_slot_index_from_left"] = slot + 1;
                    json::array description_text;
                    for (const auto& result : description.get_result()) {
                        description_text.emplace_back(result.text);
                    }
                    evidence["operator_description_text"] = std::move(description_text);
                    const auto oper = m_config->status().opers.find(name);
                    const int elite = oper == m_config->status().opers.end() ? -1 : oper->second.elite;
                    evidence["elite_before"] = elite >= 0 && elite <= 2 ? json::value(elite) : json::value(nullptr);
                    evidence["elite_before_source"] = elite >= 0 && elite <= 2 ? "runtime_status" : "unknown";
                    evidence["elite_image_confirmed"] = nullptr;
                    evidence["rule_predicted_elite_after"] =
                        elite >= 0 && elite <= 2
                            ? json::value(
                                  blackflow::confirmed_expedition_core_elite(m_expedition_initial_choice, name, elite))
                            : json::value(nullptr);
                    ++m_expedition_evidence.dispatch_attempt;
                    if (!report_expedition("dispatch_prepared", { }, selected_image) ||
                        !report_expedition("dispatch_attempt")) {
                        return false;
                    }
                    ProcessTask depart(*this, { "BlackFlow@Roguelike@ExpeditionDepart" });
                    depart.set_retry_times(0);
                    if (!depart.run()) {
                        return abort_expedition("depart_action_failed");
                    }
                    m_expedition_pending_operator = std::move(name);
                    if (!report_expedition("dispatch_click_issued")) {
                        return false;
                    }
                    return await_dispatch();
                }
            }
        }
    }
    if (need_exit()) {
        return abort_expedition("user_stopped_in_picker");
    }
    // 没找到允许派遣的干员时返回事件，按默认选项离开；不能派其他干员兜底。
    if (m_expedition_context.has_value() && !m_expedition_finished) {
        if (!report_expedition(
                "cancelled",
                { { "reason", "no_eligible_operator_verified" }, { "dispatch_was_attempted", false } })) {
            return false;
        }
    }
    m_expedition_finished = true;
    ProcessTask back(*this, { "BlackFlow@Roguelike@ExpeditionBack" });
    back.set_retry_times(2);
    const bool returned = back.run();
    return returned || (m_expedition_context.has_value() ? abort_expedition("picker_back_failed") : false);
}

bool asst::RoguelikeStageEncounterTaskPlugin::_run()
{
    LogTraceFunction;

    const std::string& theme = m_config->get_theme();
    std::vector<std::string> event_names = RoguelikeStageEncounter.get_event_names(theme);

    if (theme == RoguelikeTheme::BlackFlow) {
        refresh_encounter_choice_context();
        // 事件选项点击失败时画面通常仍是原事件页。继续走事件结果分发即可重新识别并续办；
        // 禁止点击左上角尝试“恢复地图”，那只会打开退出探索确认并回到原事件页。
        Task.set_task_base("BlackFlow@Roguelike@StageEncounterResult", "BlackFlow@Roguelike@StageEncounterReward");
        if (refresh_sacrifice_context() && (m_sacrifice.phase == blackflow::SacrificePhase::AwaitCivilization ||
                                            m_sacrifice.phase == blackflow::SacrificePhase::BeforeCivilization)) {
            return finish_sacrifice_civilization();
        }
        Matcher sacrifice_picker(ctrler()->get_image());
        sacrifice_picker.set_task_info("BlackFlow@Roguelike@SacrificePicker");
        if (sacrifice_picker.analyze()) {
            return handle_sacrifice_picker();
        }
        Matcher picker(ctrler()->get_image());
        picker.set_task_info("BlackFlow@Roguelike@ExpeditionPicker");
        if (picker.analyze()) {
            refresh_expedition_context();
            return handle_expedition_picker();
        }
    }

    const std::string themed_ocr_task = theme + "@Roguelike@StageEncounterOcr";
    const auto event_name_task_ptr =
        Task.get(themed_ocr_task) != nullptr ? Task.get(themed_ocr_task) : Task.get("Roguelike@StageEncounterOcr");
    sleep(event_name_task_ptr->pre_delay);

    if (need_exit()) {
        return false;
    }

    constexpr int EventNameOcrAttempts = 3;
    constexpr int EventNameOcrRetryDelay = 700;
    cv::Mat image;
    std::string current_event_name;
    for (int attempt = 1; attempt <= EventNameOcrAttempts; ++attempt) {
        image = ctrler()->get_image();
        OCRer name_analyzer(image);
        name_analyzer.set_task_info(event_name_task_ptr);
        name_analyzer.set_required(event_names);
        if (name_analyzer.analyze() && !name_analyzer.get_result().empty()) {
            current_event_name = name_analyzer.get_result().front().text;
            break;
        }
        if (attempt < EventNameOcrAttempts) {
            Log.warn("Unknown Event, retrying title OCR", attempt, "of", EventNameOcrAttempts);
            sleep(EventNameOcrRetryDelay);
            if (need_exit()) {
                return false;
            }
        }
    }

    if (current_event_name.empty()) {
        Log.error("Unknown Event after title OCR retries");
        save_img(image, "event title OCR failed");
        auto info = basic_info_with_what("EncounterOcrError");
        info["details"]["attempts"] = EventNameOcrAttempts;
        callback(AsstMsg::SubTaskExtraInfo, info);
        return true;
    }

    if (theme == RoguelikeTheme::BlackFlow && !m_expedition_pending_operator.empty()) {
        // 已从干员选择页进入二级事件，至此才提交派遣事实；此后同名事件只走默认离开。
        refresh_expedition_context();
        if (current_event_name != blackflow::ExpeditionEventName ||
            !confirm_expedition_dispatch(image, "secondary_event")) {
            return abort_expedition("unexpected_event_after_depart");
        }
    }

    // 处理主事件及其链式 next_event
    while (!current_event_name.empty()) {
        if (current_event_name != blackflow::LakeFairyEventName) {
            m_lake_fairy_plan.reset();
            m_lake_fairy_initial_choice_index = 0;
            m_lake_fairy_unique_choice_selected = false;
        }
        auto next = handle_single_event(current_event_name);
        if (!next) {
            break;
        }
        current_event_name = next.value();
    }

    if (theme == RoguelikeTheme::BlackFlow && !observe_encounter_choice_result(ctrler()->get_image())) return false;
    return true;
}

std::optional<std::string> asst::RoguelikeStageEncounterTaskPlugin::handle_single_event(const std::string& event_name)
{
    const std::string& theme = m_config->get_theme();
    const RoguelikeMode& mode = m_config->get_mode();
    const auto& event_map = RoguelikeStageEncounter.get_events(theme, mode);

    auto it = event_map.find(event_name);
    if (it == event_map.end()) {
        Log.error("Unknown event:", event_name);
        return std::nullopt;
    }

    const auto& event = it->second;

    cv::Mat image = ctrler()->get_image();

    int special_val = 0;
    // 水月的不好识别，先试试萨米能不能用
    if (m_config->get_theme() == RoguelikeTheme::Sami) {
        OCRer analyzer(image);
        analyzer.set_task_info(m_config->get_theme() + "@Roguelike@SpecialValRecognition");
        analyzer.set_replace(Task.get<OcrTaskInfo>("NumberOcrReplace")->replace_map);
        analyzer.set_use_char_model(true);
        if (!analyzer.analyze()) {
            // return std::nullopt;
            Log.error("Failed to recognize special value for event:", event.name);
        }
        else {
            utils::chars_to_number(analyzer.get_result().front().text, special_val);
        }
    }

    size_t choose_option = process_task(event, special_val);
    std::optional<std::vector<std::string>> planned_choice_order;
    if (theme == RoguelikeTheme::BlackFlow && m_blackflow_encounter_choice_provider) {
        if (const auto planned_choice = m_blackflow_encounter_choice_provider(event.name); planned_choice.has_value()) {
            choose_option = *planned_choice;
        }
    }
    if (theme == RoguelikeTheme::BlackFlow && m_blackflow_encounter_choice_order_provider) {
        planned_choice_order = m_blackflow_encounter_choice_order_provider(event.name);
        if (planned_choice_order.has_value()) {
            Log.info("Event:", event.name, "planned configured choice order", json::array(*planned_choice_order));
        }
    }
    Log.info("Event:", event.name, "special_val", special_val, "choose option", choose_option);

    auto info = basic_info_with_what("RoguelikeEvent");
    info["details"]["name"] = event.name;
    info["details"]["default_choose"] = event.default_choose;
    info["details"]["choose_option"] = choose_option;
    if (planned_choice_order.has_value()) {
        info["details"]["planned_choice_order"] = json::array(*planned_choice_order);
    }
    callback(AsstMsg::SubTaskExtraInfo, info);
    // 插件 callback 只会上报给外部调用方，不会再广播给同一 ProcessTask 的其他插件。
    // 需要页面内容的主题专用逻辑必须通过显式观察器接收事件名。
    if (m_event_observer) {
        m_event_observer(event.name);
    }
    if (theme == RoguelikeTheme::BlackFlow && event.name == blackflow::ExpeditionEventName) {
        refresh_expedition_context();
    }

    // 萨卡兹内容拓展 II，#11861
    if (event.name.starts_with("魂灵见闻：")) {
        Matcher matcher(image);
        matcher.set_task_info("Sarkaz@Roguelike@CloseCollectionClose");
        if (matcher.analyze()) {
            Log.trace("Found extra 'Plans', click CloseCollectionClose and StageEncounterJudgeClick");
            ctrler()->click(matcher.get_result().rect);
            ProcessTask(*this, { "Roguelike@StageEncounterJudgeClick" }).run();
            ProcessTask(*this, { "Roguelike@StageEncounterJudgeClick2" }).run();
        }
    }

    // 界园树洞
    if (m_config->get_theme() == RoguelikeTheme::JieGarden) {
        auto& bosky_map = RoguelikeBoskyPassageMap::get_instance();
        if (event.name == "掷地有声") {
            bosky_map.set_node_subtype(bosky_map.get_curr_pos(), RoguelikeBoskySubNodeType::Ling);
        }
        else if (event.name == "种因得果") {
            bosky_map.set_node_subtype(bosky_map.get_curr_pos(), RoguelikeBoskySubNodeType::Shu);
        }
        else if (event.name == "三缺一") {
            bosky_map.set_node_subtype(bosky_map.get_curr_pos(), RoguelikeBoskySubNodeType::Nian);
        }

        if (bosky_map.get_target_subtype() != RoguelikeBoskySubNodeType::Unknown) {
            if (bosky_map.get_node_subtype(bosky_map.get_curr_pos()) == bosky_map.get_target_subtype()) {
                Log.info(__FUNCTION__, "| Found target playtime node, completing task and exiting");

                auto target_info = basic_info_with_what("RoguelikeJieGardenTargetFound");
                target_info["details"]["target_subtype"] = subtype2name(bosky_map.get_target_subtype());
                callback(AsstMsg::SubTaskExtraInfo, target_info);

                // 完成任务，退出
                m_control_ptr->exit_then_stop();
                m_task_ptr->set_enable(false);
                return std::nullopt;
            }
        }
    }

    // 界园与黑流树海通过识别实际选项列表选择事件选项。
    if (theme == RoguelikeTheme::JieGarden || theme == RoguelikeTheme::BlackFlow) {
        reset_option_list_and_view_data();
        const bool options_ready = update_option_list(event.name);
        if (m_expedition_evidence.failed || m_encounter_choice.failed || m_sacrifice.evidence_failed) {
            return std::nullopt;
        }
        if (options_ready) {
            if (theme == RoguelikeTheme::BlackFlow && event.name == "溯源" && m_portal_choice) {
                std::vector<std::string> prices;
                for (const auto& option : m_option_list) {
                    prices.emplace_back(option.enabled ? option.red_description : "");
                }
                const auto choice = m_portal_choice(prices);
                if (choice.has_value() && *choice < m_option_list.size()) {
                    Log.info("BlackFlow portal option", *choice + 1, "price", prices[*choice]);
                    if (select_analyzed_option(*choice) && m_portal_selected) {
                        m_portal_selected(prices[*choice]);
                    }
                    return std::nullopt;
                }
                // Unique follow-up or the normal default leave option remains in the common flow.
            }
            if (theme == RoguelikeTheme::BlackFlow && event.name == blackflow::SacrificeEventName &&
                refresh_sacrifice_context()) {
                (void)handle_sacrifice_event();
                return std::nullopt;
            }
            if (theme == RoguelikeTheme::BlackFlow && event.name == blackflow::ExpeditionEventName &&
                m_expedition_context.has_value() && !m_expedition_finished) {
                (void)handle_expedition_choice();
                // 派遣和主动离开均已自行选择；不能再进入通用兜底重复点击。
                return std::nullopt;
            }
            if (theme == RoguelikeTheme::BlackFlow && event.name == blackflow::LakeFairyEventName) {
                return handle_blackflow_lake_fairy(event);
            }
            if (!event.choice_groups.empty()) {
                std::unordered_set<size_t> grouped_choices;
                std::vector<std::string> grouped_ocr_candidates;
                for (const auto& candidate_group : event.choice_groups) {
                    for (const auto& candidate_choice : candidate_group.choices) {
                        if (const auto* candidate_text = std::get_if<std::string>(&candidate_choice)) {
                            grouped_ocr_candidates.emplace_back(*candidate_text);
                        }
                    }
                }
                const auto resolve_grouped_choice = [&](int configured_choice) -> std::optional<size_t> {
                    const auto option_count = static_cast<std::int64_t>(m_option_list.size());
                    const std::int64_t resolved_choice =
                        configured_choice > 0 ? static_cast<std::int64_t>(configured_choice)
                                              : option_count + static_cast<std::int64_t>(configured_choice) + 1;
                    if (resolved_choice <= 0 || resolved_choice > option_count) {
                        return std::nullopt;
                    }
                    return static_cast<size_t>(resolved_choice);
                };

                for (const auto& group : event.choice_groups) {
                    std::vector<size_t> resolved_group_choices;
                    std::unordered_map<std::string, size_t> resolved_group_choice_texts;
                    const auto append_grouped_choice = [&](size_t grouped_choice) {
                        if (!grouped_choices.emplace(grouped_choice).second) {
                            Log.warn(
                                "Event:",
                                event.name,
                                "Option",
                                grouped_choice,
                                "is already assigned to an earlier group");
                            return;
                        }
                        resolved_group_choices.emplace_back(grouped_choice);
                    };

                    for (const auto& configured_choice : group.choices) {
                        std::optional<size_t> grouped_choice_opt;
                        if (const auto* choice_index = std::get_if<int>(&configured_choice)) {
                            grouped_choice_opt = resolve_grouped_choice(*choice_index);
                        }
                        else if (const auto* choice_text = std::get_if<std::string>(&configured_choice)) {
                            const auto option_it =
                                std::ranges::find_if(m_option_list, [&](const OptionAnalyzer::Option& option) {
                                    return option.text == *choice_text;
                                });
                            if (option_it != m_option_list.end()) {
                                grouped_choice_opt = std::distance(m_option_list.begin(), option_it) + 1;
                            }
                            else {
                                std::optional<size_t> fuzzy_choice;
                                utils::FuzzyTextMatch fuzzy_match;
                                bool ambiguous = false;
                                for (size_t option_index = 0; option_index < m_option_list.size(); ++option_index) {
                                    const auto match = utils::fuzzy_match_ocr_text(
                                        m_option_list[option_index].text,
                                        grouped_ocr_candidates);
                                    if (!match.accepted || match.canonical != *choice_text) {
                                        continue;
                                    }
                                    if (fuzzy_choice.has_value()) {
                                        ambiguous = true;
                                        break;
                                    }
                                    fuzzy_choice = option_index + 1;
                                    fuzzy_match = match;
                                }
                                if (!ambiguous && fuzzy_choice.has_value()) {
                                    grouped_choice_opt = fuzzy_choice;
                                    Log.info(
                                        "Event:",
                                        event.name,
                                        "Grouped OCR text",
                                        *choice_text,
                                        "fuzzy matched",
                                        m_option_list[*fuzzy_choice - 1].text,
                                        "distance",
                                        fuzzy_match.edit_distance,
                                        "similarity",
                                        fuzzy_match.similarity);
                                }
                                else if (ambiguous) {
                                    Log.warn(
                                        "Event:",
                                        event.name,
                                        "Grouped OCR text",
                                        *choice_text,
                                        "fuzzy matched multiple options; rejecting ambiguous match");
                                }
                            }
                        }

                        if (!grouped_choice_opt) {
                            if (const auto* choice_index = std::get_if<int>(&configured_choice)) {
                                Log.warn(
                                    "Event:",
                                    event.name,
                                    "Grouped choice",
                                    *choice_index,
                                    "cannot be resolved for",
                                    m_option_list.size(),
                                    "option(s)");
                            }
                            else {
                                Log.warn(
                                    "Event:",
                                    event.name,
                                    "Grouped OCR text",
                                    std::get<std::string>(configured_choice),
                                    "does not match any option");
                            }
                            continue;
                        }
                        append_grouped_choice(grouped_choice_opt.value());
                        if (const auto* choice_text = std::get_if<std::string>(&configured_choice)) {
                            resolved_group_choice_texts.insert_or_assign(*choice_text, grouped_choice_opt.value());
                        }
                    }

                    if (group.choice_range) {
                        const auto range_begin_opt = resolve_grouped_choice(group.choice_range->first);
                        const auto range_end_opt = resolve_grouped_choice(group.choice_range->second);
                        if (!range_begin_opt || !range_end_opt) {
                            Log.warn(
                                "Event:",
                                event.name,
                                "Grouped range cannot be resolved for",
                                m_option_list.size(),
                                "option(s)");
                        }
                        else {
                            const size_t range_begin = range_begin_opt.value();
                            const size_t range_end = range_end_opt.value();
                            for (size_t grouped_choice = range_begin;;) {
                                append_grouped_choice(grouped_choice);
                                if (grouped_choice == range_end) {
                                    break;
                                }
                                if (range_begin < range_end) {
                                    ++grouped_choice;
                                }
                                else {
                                    --grouped_choice;
                                }
                            }
                        }
                    }

                    if (planned_choice_order.has_value()) {
                        std::vector<size_t> planned_group_choices;
                        std::unordered_set<size_t> appended_planned_choices;
                        for (const std::string& configured_text : *planned_choice_order) {
                            const auto resolved = resolved_group_choice_texts.find(configured_text);
                            if (resolved == resolved_group_choice_texts.end() ||
                                !appended_planned_choices.emplace(resolved->second).second) {
                                continue;
                            }
                            planned_group_choices.emplace_back(resolved->second);
                        }
                        // 名称计划是权威候选集：没有列出的配置项代表安全评估已明确排除。
                        // grouped_choices 仍保留完整显式组，避免被排除项又落入隐式最终回退。
                        resolved_group_choices = std::move(planned_group_choices);
                    }

                    if (group.random) {
                        if (theme == RoguelikeTheme::BlackFlow) {
                            if (!randomize_encounter_choice_order(resolved_group_choices)) {
                                return std::nullopt;
                            }
                        }
                        else {
                            static thread_local std::mt19937 random_engine(std::random_device { }());
                            std::shuffle(resolved_group_choices.begin(), resolved_group_choices.end(), random_engine);
                        }
                    }

                    if (theme == RoguelikeTheme::BlackFlow && event.name == blackflow::GoldStasisEventName) {
                        const auto context = m_blackflow_encounter_context_provider
                                                 ? m_blackflow_encounter_context_provider()
                                                 : std::nullopt;
                        const bool core_operator_elite_two = context.has_value() && context->core_operator_elite_two;
                        std::ranges::stable_sort(resolved_group_choices, [&](size_t lhs, size_t rhs) {
                            return blackflow::gold_stasis_choice_priority_bucket(
                                       m_option_list[lhs - 1].text,
                                       core_operator_elite_two) <
                                   blackflow::gold_stasis_choice_priority_bucket(
                                       m_option_list[rhs - 1].text,
                                       core_operator_elite_two);
                        });
                        if (core_operator_elite_two) {
                            Log.info("Event: 金色凝滞 | defer 查看无人机 and 向泉水许愿 behind all other choices");
                        }
                    }

                    for (const size_t grouped_choice : resolved_group_choices) {
                        if (!m_option_list[grouped_choice - 1].enabled) {
                            Log.info("Event:", event.name, "Grouped choice", grouped_choice, "is disabled");
                            continue;
                        }
                        if (event.name == "货从口出" &&
                            !plan_encounter_choice(grouped_choice - 1, "random_group_order")) {
                            return std::nullopt;
                        }
                        if (!select_analyzed_option(grouped_choice - 1)) {
                            if (m_encounter_choice.failed) {
                                return std::nullopt;
                            }
                            continue;
                        }
                        if (event.name == "货从口出" && !confirm_cargo_choice()) {
                            return std::nullopt;
                        }

                        if (theme == RoguelikeTheme::BlackFlow) {
                            Task.set_task_base(
                                "BlackFlow@Roguelike@StageEncounterResult",
                                "BlackFlow@Roguelike@StageEncounterReward");
                        }
                        return next_event(group.next_event.value_or(event.next_event));
                    }
                }

                if (planned_choice_order.has_value()) {
                    // 权威名称计划中的所有候选都未能选择时宁可让本轮失败/重试，也不能把
                    // OCR 未成功归组的危险代价项当成普通未覆盖选项再次捞回来。
                    return std::nullopt;
                }

                // 隐式最终回退组：从后向前尝试所有未被显式组覆盖的可用选项，继承事件级 next_event。
                for (size_t fallback_choice = m_option_list.size(); fallback_choice > 0; --fallback_choice) {
                    if (grouped_choices.contains(fallback_choice) || !m_option_list[fallback_choice - 1].enabled) {
                        continue;
                    }
                    if (event.name == "货从口出" &&
                        !plan_encounter_choice(fallback_choice - 1, "configured_fallback")) {
                        return std::nullopt;
                    }
                    if (!select_analyzed_option(fallback_choice - 1)) {
                        if (m_encounter_choice.failed) {
                            return std::nullopt;
                        }
                        continue;
                    }
                    if (event.name == "货从口出" && !confirm_cargo_choice()) {
                        return std::nullopt;
                    }
                    if (theme == RoguelikeTheme::BlackFlow) {
                        Task.set_task_base(
                            "BlackFlow@Roguelike@StageEncounterResult",
                            "BlackFlow@Roguelike@StageEncounterReward");
                    }
                    return next_event(event.next_event);
                }
                return std::nullopt;
            }

            size_t choice = 0; // 以 0 作为无效 index。
            if (theme == RoguelikeTheme::BlackFlow) {
                if (choose_option > 0 && choose_option <= m_option_list.size() &&
                    m_option_list[choose_option - 1].enabled) {
                    choice = choose_option;
                }
                else {
                    const auto enabled_it =
                        std::ranges::find_if(m_option_list.rbegin(), m_option_list.rend(), [](const auto& option) {
                            return option.enabled;
                        });
                    if (enabled_it != m_option_list.rend()) {
                        choice = static_cast<size_t>(std::distance(m_option_list.begin(), enabled_it.base()));
                    }
                }
            }
            else if (!event.option_text.empty()) {
                for (const std::string& event_text : event.option_text) {
                    const auto option_it =
                        std::ranges::find_if(m_option_list, [&event_text](const OptionAnalyzer::Option& option) {
                            return option.text == event_text;
                        });
                    if (option_it != m_option_list.end()) {
                        choice = std::distance(m_option_list.begin(), option_it) + 1;
                        break;
                    }
                }
            }
            else if (event.option_num == m_option_list.size()) {
                choice = choose_option;
            }
            else {
                for (const auto& [total, item] : event.fallback_choices) {
                    if (total == m_option_list.size()) {
                        choice = item;
                        break;
                    }
                }
            }

            if (choice == 0) {
                Log.error(
                    std::format(
                        "RoguelikeEncounter | Failed to find choice for scenario with {} option(s)",
                        m_option_list.size()));
            }
            else if (select_analyzed_option(choice - 1)) {
                if (theme == RoguelikeTheme::BlackFlow) {
                    Task.set_task_base(
                        "BlackFlow@Roguelike@StageEncounterResult",
                        "BlackFlow@Roguelike@StageEncounterReward");
                }
                return next_event(event.next_event);
            }

            if (theme == RoguelikeTheme::BlackFlow) {
                for (choice = m_option_list.size(); choice > 0; --choice) {
                    if (m_option_list[choice - 1].enabled && select_analyzed_option(choice - 1)) {
                        Task.set_task_base(
                            "BlackFlow@Roguelike@StageEncounterResult",
                            "BlackFlow@Roguelike@StageEncounterReward");
                        return next_event(event.next_event);
                    }
                }
                return std::nullopt;
            }

            // 界园兜底：从下到上依次选择。
            for (choice = m_option_list.size(); choice > 0; --choice) {
                if (m_option_list[choice - 1].enabled && select_analyzed_option(choice - 1)) {
                    return next_event(event.next_event);
                }
            }
        }
        else if (theme == RoguelikeTheme::BlackFlow) {
            Log.error("BlackFlow encounter option analysis failed");
            return std::nullopt;
        }
    }

    const auto click_option_task_name = [&](size_t item, size_t total) {
        if (item > total) {
            Log.warn("Event:", event.name, "Total:", total, "Choice", item, "out of range, switch to choice", total);
            item = total;
        }
        return m_config->get_theme() + "@Roguelike@OptionChoose" + std::to_string(total) + "-" + std::to_string(item);
    };

    for (int j = 0; j < 2; ++j) {
        ProcessTask(*this, { click_option_task_name(choose_option, event.option_num) }).run();
        sleep(300);
    }

    sleep(1500);

    // 判断是否点击成功，成功进入对话后左上角的生命值会消失
    image = ctrler()->get_image();
    bool hp_disappeared = (hp(image) < 0);
    // fallback 可变选项，临时处理，之后还得改成更通用的方式
    if (!hp_disappeared) {
        for (const auto& [total, item] : event.fallback_choices) {
            Log.info("Trying fallback choice", total, "-", item);
            for (int j = 0; j < 2; ++j) {
                ProcessTask(*this, { click_option_task_name(item, total) }).run();
                sleep(300);
            }
            sleep(500);
            image = ctrler()->get_image();
            if (hp(image) < 0) {
                Log.info("Fallback choice success");
                hp_disappeared = true;
                break;
            }
        }
    }

    if (hp_disappeared) {
        return next_event(event.next_event);
    }

    // 兜底处理，从 option_num-option_num 点到 1-1
    size_t max_time = event.option_num;
    while (max_time > 0) {
        for (size_t i = max_time; i > 0; --i) {
            bool ret =
                ProcessTask(*this, { "Roguelike@TutorialButton" }).set_reusable_image(image).set_retry_times(3).run();
            if (ret) {
                return std::nullopt;
            }

            for (int j = 0; j < 2; ++j) {
                ProcessTask(*this, { click_option_task_name(i, max_time) }).run();
                sleep(300);
            }

            if (need_exit()) {
                return std::nullopt;
            }

            sleep(500);
            image = ctrler()->get_image();
            if (hp(image) < 0) {
                return std::nullopt;
            }
        }
        --max_time;
    }

    return event.next_event.empty() ? std::nullopt : std::optional { event.next_event };
}

std::optional<std::string>
    asst::RoguelikeStageEncounterTaskPlugin::handle_blackflow_lake_fairy(const Config::RoguelikeEvent& event)
{
    // 新事件从开场重新建立计划；EncounterReward 对中途页面的续办保留已完成进度。
    if (std::ranges::any_of(m_option_list, [](const auto& option) { return option.text == "上前看看"; })) {
        m_lake_fairy_plan.reset();
        m_lake_fairy_initial_choice_index = 0;
        m_lake_fairy_unique_choice_selected = false;
    }
    if (!m_lake_fairy_plan.has_value()) {
        const std::optional<blackflow::LakeFairyContext> context =
            m_blackflow_encounter_context_provider ? m_blackflow_encounter_context_provider() : std::nullopt;
        if (!context.has_value()) {
            Log.warn("Event: 湖中仙女 | encounter context unavailable; use the conservative branch");
        }
        blackflow::LakeFairyContext resolved_context = context.value_or(blackflow::LakeFairyContext { });
        const auto current_ingots = blackflow::perception::recognize_ingots(ctrler()->get_image());
        // 事件内的付费决策必须读取当前 HUD，识别失败也不能沿用旧地图金额。
        resolved_context.ingots = current_ingots.value_or(0);
        if (!current_ingots.has_value()) {
            Log.warn("Event: 湖中仙女 | current ingots unavailable; use the conservative branch");
        }
        m_lake_fairy_plan = blackflow::make_lake_fairy_choice_plan(resolved_context);
        Log.info(
            "Event: 湖中仙女 | locked branch",
            resolved_context.core_operator_elite_two && resolved_context.ingots >= 3
                ? "first option four times"
                : "first option, then second option",
            "core operator elite two",
            resolved_context.core_operator_elite_two,
            "ingots",
            resolved_context.ingots);
    }

    std::vector<blackflow::LakeFairyOption> options;
    for (const auto& option : m_option_list) {
        options.push_back({ option.enabled, option.text });
    }
    const auto choice = blackflow::resolve_lake_fairy_choice(
        *m_lake_fairy_plan,
        m_lake_fairy_initial_choice_index,
        m_lake_fairy_unique_choice_selected,
        options);
    if (!choice.has_value()) {
        Log.error("Event: 湖中仙女 | no applicable enabled option on the current page");
        return std::nullopt;
    }
    if (choice->fallback) {
        Log.warn("Event: 湖中仙女 | planned option unavailable; fallback to", options[choice->index].text);
    }
    if (!select_analyzed_option(choice->index)) {
        return std::nullopt;
    }
    m_lake_fairy_initial_choice_index = choice->next_initial_choice_index;
    m_lake_fairy_unique_choice_selected = choice->unique;
    Task.set_task_base("BlackFlow@Roguelike@StageEncounterResult", "BlackFlow@Roguelike@StageEncounterReward");
    return next_event(event.next_event);
}

bool asst::RoguelikeStageEncounterTaskPlugin::satisfies_condition(
    const Config::ChoiceRequire& requirement,
    const int special_val)
{
    int value = 0;
    bool ret = utils::chars_to_number(requirement.vision.value, value);
    Log.trace("special_val: ", special_val, "value: ", value);
    switch (requirement.vision.type) {
    case Config::ComparisonType::GreaterThan:
        ret &= special_val > value;
        Log.trace("special_val > value: ", special_val > value ? "true" : "false");
        break;
    case Config::ComparisonType::LessThan:
        ret &= special_val < value;
        Log.trace("special_val < value: ", special_val < value ? "true" : "false");
        break;
    case Config::ComparisonType::Equal:
        ret &= special_val == value;
        Log.trace("special_val == value: ", special_val == value ? "true" : "false");
        break;
    case Config::ComparisonType::None:
        Log.warn("no vision type");
        break;
    case Config::ComparisonType::Unsupported:
        Log.warn("unsupported vision type");
        return false;
    }
    /*
    switch (requirement.hp.type) {
        // ...
    }
    */
    if (!ret) {
        return false;
    }
    return true;
}

size_t asst::RoguelikeStageEncounterTaskPlugin::process_task(const Config::RoguelikeEvent& event, const int special_val)
{
    for (const auto& requirement : event.choice_require) {
        if (requirement.choose == -1) {
            continue;
        }
        if (satisfies_condition(requirement, special_val)) {
            return requirement.choose;
        }
    }
    return event.default_choose;
}

int asst::RoguelikeStageEncounterTaskPlugin::hp(const cv::Mat& image) const
{
    LogTraceFunction;

    if (!ProcessTask(*this, { "Roguelike@HpFlag" }).run()) {
        Log.info("Not found HpFlag");
        return -1;
    }

    auto task = Task.get<OcrTaskInfo>("Roguelike@HpRecognition");
    std::vector<std::pair<std::string, std::string>> merged_map;
    merged_map.insert(merged_map.end(), task->replace_map.begin(), task->replace_map.end());
    merged_map.emplace_back("(.*)/.*", "$1");

    auto roi_image = make_roi(image, task->roi).clone();
    cv::Mat r_channel;
    cv::extractChannel(roi_image, r_channel, 2);
    cv::Mat mask;
    cv::threshold(r_channel, mask, 50, 255, cv::THRESH_BINARY);
    cv::Mat inv_mask;
    cv::bitwise_not(mask, inv_mask);
    roi_image.setTo(cv::Scalar(0, 0, 0), mask);

    RegionOCRer analyzer(roi_image);
    analyzer.set_replace(merged_map);
    analyzer.set_use_char_model(true);
    analyzer.set_bin_threshold(60); // 血量没有红色通道，虽然它看着很明显，但实际上在灰度中只有 2/3

    auto res_vec_opt = analyzer.analyze();
    if (!res_vec_opt) {
        return 0;
    }

    int hp_val;
    return utils::chars_to_number(res_vec_opt->text, hp_val) ? hp_val : 0;
}

bool asst::RoguelikeStageEncounterTaskPlugin::update_option_list(std::string_view event_name)
{
    LogTraceFunction;

    const std::string& theme = m_config->get_theme();

    // 不期而遇默认位置会在选项列表中央, 为了从上到下检视选项列表, 需要先向上滑动
    move_to_option_list_head();

    cv::Mat image = ctrler()->get_image();
    RoguelikeEncounterOptionAnalyzer analyzer(image);
    analyzer.set_theme(theme);
    analyzer.set_analyze_red_description(event_name == "溯源");
    const size_t max_swipe_times = theme == RoguelikeTheme::BlackFlow ? BLACKFLOW_MAX_SWIPE_TIMES : MAX_SWIPE_TIMES;
    for (size_t swipe_times = 0; swipe_times < max_swipe_times && !need_exit(); ++swipe_times) {
        move_forward();
        image = ctrler()->get_image();
        const std::optional<int> ret = analyzer.merge_image(image);
        if (!ret) {
            return false;
        }
        if (ret.value() <= 0) {
            break;
        }
    }

    if (!analyzer.analyze()) {
        return false;
    }

    m_option_list = analyzer.get_result();
    m_option_list_image = analyzer.get_stitched_image().clone();
    if (theme == RoguelikeTheme::BlackFlow && event_name != blackflow::SacrificeEventName && event_name != "失与得" &&
        event_name != blackflow::ExpeditionEventName &&
        !prepare_encounter_choice(event_name, 0, analyzer.get_stitched_image())) {
        return false;
    }
    const bool expedition_candidates = event_name == blackflow::ExpeditionEventName &&
                                       m_expedition_context.has_value() && !m_expedition_finished &&
                                       !m_expedition_evidence.candidates_saved;
    if (expedition_candidates) {
        if (!capture_expedition_candidates(analyzer.get_stitched_image())) {
            return false;
        }
    }
    else if (m_event_capture_observer && !analyzer.get_stitched_image().empty()) {
        m_event_capture_observer(event_name, analyzer.get_stitched_image());
    }

    report_analyzed_options();

    update_view(image);

    return true;
}

bool asst::RoguelikeStageEncounterTaskPlugin::select_analyzed_option(size_t index)
{
    LogTraceFunction;
    if (m_config->get_theme() == RoguelikeTheme::BlackFlow) {
        refresh_encounter_choice_context();
    }
    if (m_config->get_theme() == RoguelikeTheme::BlackFlow && m_encounter_choice.generic &&
        m_sacrifice_context_provider && m_encounter_choice.context == m_sacrifice_context_provider() &&
        index < m_option_list.size() && m_option_list[index].enabled && !m_encounter_choice.confirmed) {
        const auto& candidates = m_encounter_choice.details.at("candidates").as_array();
        if (index < candidates.size() && candidates[index].get("option", std::string()) == m_option_list[index].text &&
            !plan_encounter_choice(index, "resolved_enabled_option")) return false;
    }
    // Consume the explicit plan: a later leave/reward click may reuse this index.
    const bool encounter_choice = m_encounter_choice.capture_clicks && m_encounter_choice.context &&
                                  !m_encounter_choice.confirmed && m_encounter_choice.selected == index;
    m_encounter_choice.capture_clicks = false;

    // sanity check
    if (index >= m_option_list.size()) [[unlikely]] {
        Log.error(
            __FUNCTION__,
            std::format("| Attempt to select option {} out of {}", index + 1, m_option_list.size()));
        return false;
    }
    if (!m_option_list[index].enabled) {
        Log.info(__FUNCTION__, std::format("| Attempt to select disabled option {}", index + 1));
        return false;
    }

    if (m_config->get_theme() == RoguelikeTheme::BlackFlow) {
        constexpr int ClickAttempts = 3;
        constexpr int ConfirmationRecognitionRetries = 3;
        for (int attempt = 1; attempt <= ClickAttempts && !need_exit(); ++attempt) {
            if (!move_to_analyzed_option(index)) {
                Log.warn(
                    __FUNCTION__,
                    std::format("| Could not bring option {} back into view on click attempt {}", index + 1, attempt));
                continue;
            }
            const std::optional<Rect> stable_header = wait_for_analyzed_option_stable(index);
            if (!stable_header.has_value()) {
                Log.warn(
                    __FUNCTION__,
                    std::format("| Option {} did not settle before click attempt {}", index + 1, attempt));
                continue;
            }

            Log.info(
                __FUNCTION__,
                std::format(
                    "| Clicking stable option {}: {} (attempt {})",
                    index + 1,
                    m_option_list[index].text,
                    attempt));
            const Point click_point {
                stable_header->x + stable_header->width / 2 + 100,
                stable_header->y + stable_header->height / 2,
            };
            const bool expedition_choice =
                m_expedition_context.has_value() && !m_expedition_finished && m_expedition_initial_choice == index;
            if (encounter_choice) {
                ++m_encounter_choice.attempt;
                if (!report_encounter_choice(
                        "attempt",
                        { { "click_point", json::array { click_point.x, click_point.y } } },
                        ctrler()->get_image())) {
                    return false;
                }
            }
            if (expedition_choice) {
                ++m_expedition_evidence.choice_attempt;
                if (!report_expedition(
                        "choice_attempt",
                        { { "click_point", json::array { click_point.x, click_point.y } } })) {
                    return false;
                }
            }
            const bool click_issued = ctrler()->click(click_point);
            if (encounter_choice) {
                m_encounter_choice.clicked |= click_issued;
                if (!report_encounter_choice(click_issued ? "click_issued" : "input_failed")) {
                    return false;
                }
                if (!click_issued) {
                    continue;
                }
            }
            if (expedition_choice) {
                if (!click_issued) {
                    if (!report_expedition("choice_input_failed")) {
                        return false;
                    }
                    continue;
                }
                m_expedition_evidence.choice_clicked = true;
                if (!report_expedition("choice_click_issued")) {
                    return false;
                }
            }
            sleep(300);
            if (ProcessTask(*this, { "BlackFlow@Roguelike@StageEncounterLeaveConfirm" })
                    .set_retry_times(ConfirmationRecognitionRetries)
                    .run()) {
                if (encounter_choice && m_encounter_choice.generic) {
                    return observe_encounter_choice_result(ctrler()->get_image());
                }
                return true;
            }

            Log.warn(
                __FUNCTION__,
                std::format(
                    "| Option {} did not respond; retry the same planned option before using a fallback",
                    index + 1));
        }
    }
    else {
        if (!move_to_analyzed_option(index)) {
            return false;
        }

        // click option
        Log.info(__FUNCTION__, std::format("| Clicking option {}: {}", index + 1, m_option_list[index].text));
        Rect click_rect = Task.get("JieGarden@RoguelikeEncounter-ClickOption")->specific_rect;
        click_rect.y = m_option_y_in_view[index];
        for (int j = 0; j < 2; ++j) {
            ctrler()->click(click_rect);
            sleep(300);
        }
        sleep(1500);

        if (hp(ctrler()->get_image()) < 0) {
            return true;
        }
    }

    Log.error(__FUNCTION__, "| The option doesn't respond to click");
    save_img(ctrler()->get_image(), "current screenshot");

    return false;
}

std::optional<asst::Rect> asst::RoguelikeStageEncounterTaskPlugin::wait_for_analyzed_option_stable(size_t index)
{
    constexpr int ObservationAttempts = 15;
    constexpr int ObservationDelay = 120;

    blackflow::EncounterOptionPositionStability stability;
    cv::Mat latest_image;
    for (int attempt = 0; attempt < ObservationAttempts && !need_exit(); ++attempt) {
        latest_image = ctrler()->get_image();
        const Matcher::ResultOpt match = RoguelikeEncounterOptionAnalyzer::match_option(
            m_config->get_theme(),
            latest_image,
            m_option_list[index].templ);
        if (match.has_value()) {
            m_option_y_in_view[index] = match->rect.y;
            m_option_rect_in_view[index] = match->rect;
            if (stability.observe(match->rect.y)) {
                return match->rect;
            }
        }
        else {
            (void)stability.observe(std::nullopt);
        }

        if (attempt + 1 < ObservationAttempts) {
            sleep(ObservationDelay);
        }
    }

    // Refresh the complete visible range so the next click attempt can scroll the target
    // back into view if inertia carried it outside the matcher ROI.
    if (!latest_image.empty()) {
        update_view(latest_image);
    }
    return std::nullopt;
}

void asst::RoguelikeStageEncounterTaskPlugin::reset_option_list_and_view_data()
{
    m_option_list.clear();
    reset_view();
}

void asst::RoguelikeStageEncounterTaskPlugin::report_analyzed_options()
{
    std::vector<json::value> options;

    Log.info("Analyzed Options");
    Log.info(std::string(40, '-'));
    Log.info(std::format("{:^9} | {}", "Enabled", "Text"));
    Log.info(std::string(40, '-'));
    for (const auto& [enabled, templ, text, red_description] : m_option_list) {
        json::value option = json::object {
            { "enabled", enabled },
            { "text", text },
            { "red_description", red_description },
        };
        options.emplace_back(std::move(option));
        Log.info(std::format("{:^9} | {}", enabled ? "Y" : "N", text));
    }
    Log.info(std::string(40, '-'));

    json::value info = basic_info_with_what("RoguelikeEncounterOptions");
    info["details"]["options"] = std::move(options);
    callback(AsstMsg::SubTaskExtraInfo, info);
}

void asst::RoguelikeStageEncounterTaskPlugin::update_view(const cv::Mat& image)
{
    LogTraceFunction;

    reset_view();

    cv::Mat img = image.empty() ? ctrler()->get_image() : image;

    for (size_t i = 0; i < m_option_list.size(); ++i) {
        if (const Matcher::ResultOpt option_match_ret =
                RoguelikeEncounterOptionAnalyzer::match_option(m_config->get_theme(), img, m_option_list[i].templ);
            option_match_ret) {
            if (i < m_view_begin) {
                m_view_begin = i;
            }
            if (i >= m_view_end) {
                m_view_end = i + 1;
            }
            m_option_y_in_view[i] = option_match_ret.value().rect.y;
            m_option_rect_in_view[i] = option_match_ret.value().rect;
        }
    }

    Log.info(__FUNCTION__, std::format("| Current view is [{}, {}]", m_view_begin + 1, m_view_end));
}

void asst::RoguelikeStageEncounterTaskPlugin::reset_view()
{
    m_view_begin = m_option_list.size();
    m_view_end = 0;
    m_option_y_in_view.assign(m_option_list.size(), UNDEFINED);
    m_option_rect_in_view.assign(m_option_list.size(), Rect { UNDEFINED, UNDEFINED, 0, 0 });
}

bool asst::RoguelikeStageEncounterTaskPlugin::move_to_analyzed_option(size_t index)
{
    LogTraceFunction;

    // sanity check
    if (index >= m_option_list.size()) [[unlikely]] {
        Log.error(
            __FUNCTION__,
            std::format("| Attempt to move to option {} out of {}", index + 1, m_option_list.size()));
        return false;
    }

    Log.info(__FUNCTION__, std::format("Moving to option {}: {}", index + 1, m_option_list[index].text));

    cv::Mat image;
    int completed_attempts = 0;
    while (!need_exit() && blackflow::encounter_option_navigation_should_continue(completed_attempts)) {
        if (index < m_view_begin) {
            move_backward();
            ++completed_attempts;
            image = ctrler()->get_image();
            update_view(image);
            continue;
        }
        if (index >= m_view_end) {
            move_forward();
            ++completed_attempts;
            image = ctrler()->get_image();
            update_view(image);
            continue;
        }
        if (m_option_y_in_view[index] == UNDEFINED) {
            Log.error(__FUNCTION__, "| y for option {} in view is not updated", index + 1);
            save_img(image, "lastly used screenshot");
            save_img(m_option_list[index].templ, "option template");
            image = ctrler()->get_image();
            update_view(image);
            ++completed_attempts;
            continue;
        }
        return true;
    }

    Log.error(
        __FUNCTION__,
        std::format("| Failed to bring option {} into view after {} attempts", index + 1, completed_attempts));
    return false;
}

void asst::RoguelikeStageEncounterTaskPlugin::move_to_option_list_head()
{
    LogTraceFunction;

    ProcessTask(*this, { m_config->get_theme() + "@RoguelikeEncounter-InitialMoveUp" }).run();
}

void asst::RoguelikeStageEncounterTaskPlugin::move_forward()
{
    LogTraceFunction;

    ProcessTask(*this, { m_config->get_theme() + "@RoguelikeEncounter-MoveDown" }).run();
}

void asst::RoguelikeStageEncounterTaskPlugin::move_backward()
{
    LogTraceFunction;

    ProcessTask(*this, { m_config->get_theme() + "@RoguelikeEncounter-MoveUp" }).run();
}

std::optional<std::string> asst::RoguelikeStageEncounterTaskPlugin::next_event(const std::string& next_event_name)
{
    LogTraceFunction;

    if (next_event_name.empty()) {
        return std::nullopt;
    }

    const auto& task = Task.get("Roguelike@StageEncounterJudgeClick");
    const bool is_blackflow = m_config->get_theme() == RoguelikeTheme::BlackFlow;
    const auto observe_page_state = [&]() {
        const cv::Mat image = ctrler()->get_image();
        bool map_visible = false;
        if (is_blackflow) {
            for (const std::string_view map_task : {
                     std::string_view { "BlackFlow@Roguelike@MapPrepare-Ready" },
                     std::string_view { "BlackFlow@Roguelike@MapPrepare-ZoomOut" },
                 }) {
                Matcher map_matcher(image);
                map_matcher.set_task_info(std::string { map_task });
                if (map_matcher.analyze().has_value()) {
                    map_visible = true;
                    break;
                }
            }
        }
        return blackflow::classify_chained_encounter_page(is_blackflow, map_visible, hp(image) >= 0);
    };
    const auto stop_if_returned_to_map = [&]() {
        if (observe_page_state() != blackflow::ChainedEncounterPageState::MapReturned) {
            return false;
        }
        Log.info("BlackFlow chained encounter returned to map; finish event without another click");
        return true;
    };
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 2; ++j) {
            // 地图和事件页都有生命值 HUD，不能再用 hp() 单独判断链式事件是否已经出现。
            // 每次点击前后都判图；有些选项会在本次点击后直接退回地图。
            if (stop_if_returned_to_map()) {
                return std::nullopt;
            }
            ctrler()->click(task->specific_rect);
            sleep(500);
            if (stop_if_returned_to_map()) {
                return std::nullopt;
            }
        }
        if (observe_page_state() == blackflow::ChainedEncounterPageState::EventReady) {
            Log.debug("HP restored, going to next_event:", next_event_name);
            // 多点一次，确保选项恢复
            if (stop_if_returned_to_map()) {
                return std::nullopt;
            }
            ctrler()->click(task->specific_rect);
            sleep(500);
            if (stop_if_returned_to_map()) {
                return std::nullopt;
            }
            return next_event_name;
        }
    }

    return std::nullopt;
}

bool asst::RoguelikeStageEncounterTaskPlugin::save_img(const cv::Mat& image, const std::string_view description)
{
    return utils::save_debug_image(image, utils::path("debug") / "roguelike" / "encounter", true, description);
}
