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
    static std::mt19937 random(std::random_device {}());
    return std::uniform_int_distribution<std::size_t>(0, size - 1)(random);
}
}

bool asst::RoguelikeStageEncounterTaskPlugin::refresh_sacrifice_context()
{
    const auto context = m_sacrifice_context_provider ? m_sacrifice_context_provider() : std::nullopt;
    if (context != m_sacrifice.context) {
        m_sacrifice = {};
        m_sacrifice.context = context;
    }
    return context.has_value();
}

void asst::RoguelikeStageEncounterTaskPlugin::report_sacrifice(
    std::string_view phase,
    json::object details,
    const cv::Mat& image)
{
    details["exchange_round"] = m_sacrifice.exchanges + 1;
    details["capture_kind"] = "sacrifice";
    Log.info("BlackFlow sacrifice", phase, details);
    if (m_event_detail_observer) {
        m_event_detail_observer(blackflow::SacrificeEventName, phase, std::move(details), image);
    }
}

bool asst::RoguelikeStageEncounterTaskPlugin::handle_sacrifice_event()
{
    using blackflow::SacrificePhase;
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
        report_sacrifice(
            "exchange-reward",
            json::object { { "selected_name", m_sacrifice.selected_name },
                           { "reward_option", m_option_list.front().text } },
            ctrler()->get_image());
        if (select_analyzed_option(0)) {
            ++m_sacrifice.exchanges;
            m_sacrifice.phase = SacrificePhase::AwaitRepeat;
            m_sacrifice.selected_card.reset();
            m_sacrifice.picker_attempts = {};
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
    report_sacrifice(
        "event-choice",
        json::object { { "option", m_option_list[index].text }, { "option_index", index + 1 } },
        ctrler()->get_image());
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
    const auto leave_picker = [&]() {
        ProcessTask back(*this, { std::string(Prefix) + "SacrificeBack" });
        back.set_retry_times(2);
        if (back.run()) {
            return true;
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
    if (!m_sacrifice.picker_attempts.begin()) {
        Log.warn("BlackFlow sacrifice picker retry limit reached; leaving the exchange");
        report_sacrifice(
            "picker-recovery",
            json::object { { "reason", "retry_limit_reached" },
                           { "attempts", m_sacrifice.picker_attempts.count } },
            ctrler()->get_image());
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
        auto cards = blackflow::perception::sacrifice_visible_items(ctrler()->get_image());
        sleep(350);
        const auto stable = blackflow::perception::sacrifice_visible_items(ctrler()->get_image());
        if (cards.empty() || cards != stable) {
            Log.warn("BlackFlow sacrifice item list not stable; retrying without selecting an empty slot");
            return true;
        }
        m_sacrifice.selected_card = cards[random_index(cards.size())];
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
        report_sacrifice("item-selected", json::object { { "selected_name", name } }, image);
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

std::optional<std::vector<std::string>>
    asst::RoguelikeStageEncounterTaskPlugin::scan_sacrifice_natural_items(std::string_view phase)
{
    const auto single_task = [&](std::string_view suffix) {
        ProcessTask task(*this, { std::string(Prefix) + std::string(suffix) });
        task.set_retry_times(0);
        return task.run();
    };
    const auto set_panel = [&](bool expanded) {
        int consecutive = 0;
        int cooldown = 0;
        int clicks = 0;
        for (int sample = 0; sample < 22 && !need_exit(); ++sample) {
            const cv::Mat image = ctrler()->get_image();
            const bool is_expanded = recognize(image, "SacrificeInventoryExpanded");
            const bool is_collapsed = !is_expanded && recognize(image, "SacrificeInventoryOpen") &&
                                      (recognize(image, "StageEncounterOcr") || recognize(image, "MapPrepare-Ready") ||
                                       recognize(image, "MapPrepare-ZoomOut"));
            consecutive = (expanded ? is_expanded : is_collapsed) ? consecutive + 1 : 0;
            if (consecutive >= 2) {
                return true;
            }
            if ((expanded ? is_collapsed : is_expanded) && cooldown == 0 && clicks < 3) {
                if (!single_task(expanded ? "SacrificeInventoryOpen" : "SacrificeInventoryClose")) {
                    return false;
                }
                ++clicks;
                cooldown = 5;
            }
            sleep(300);
            cooldown = std::max(0, cooldown - 1);
        }
        return false;
    };
    // 重新展开必定位于首屏；全过程只做局部单步操作，不调用地图移动或丢弃零件流程。
    if (!set_panel(false) || !set_panel(true)) {
        (void)set_panel(false);
        report_sacrifice(phase, json::object { { "inventory_status", "open_failed" } }, ctrler()->get_image());
        return std::nullopt;
    }
    const auto scan = [&]() -> std::optional<std::vector<std::string>> {
        OCRer capacity(ctrler()->get_image());
        capacity.set_task_info(std::string(Prefix) + "SacrificeInventoryCount");
        std::optional<int> count;
        if (capacity.analyze()) {
            const std::regex fraction(R"((\d+)\s*/\s*\d+)");
            for (const auto& result : capacity.get_result()) {
                std::smatch match;
                if (std::regex_search(result.text, match, fraction)) {
                    count = std::stoi(match[1].str());
                    break;
                }
            }
        }
        if (!count.has_value() || *count > 3 * (2 + blackflow::InventoryMaximumSwipes)) {
            return std::nullopt;
        }
        std::vector<std::string> naturals;
        int observed = 0;
        cv::Mat previous_page;
        const auto natural_names = Task.get<OcrTaskInfo>(std::string(Prefix) + "InventoryNaturalPriority")->text;
        for (int page = 0; page <= blackflow::InventoryMaximumSwipes && !need_exit(); ++page) {
            if (page > 0 && !single_task("MovementInventorySwipe")) {
                return std::nullopt;
            }
            // 每次手势移动一列；首屏检查两列，以后只纳入右侧新列，保留相同名称的各个实例。
            // 容量计数用于交叉校验，右侧截断的第三列不计入。
            using Slots = std::map<std::pair<int, int>, std::string>;
            Slots previous;
            std::optional<Slots> stable;
            cv::Mat image;
            for (int sample = 0; sample < 5 && !need_exit(); ++sample) {
                sleep(300);
                image = ctrler()->get_image();
                if (!recognize(image, "SacrificeInventoryExpanded")) {
                    continue;
                }
                OCRer names(image);
                names.set_task_info(std::string(Prefix) + "MovementInventoryAllItems");
                Slots slots;
                if (names.analyze()) {
                    for (const auto& result : names.get_result()) {
                        const int x = result.rect.x + result.rect.width / 2;
                        const int y = result.rect.y + result.rect.height / 2;
                        if (x < (page == 0 ? 348 : 760) || x >= 1190) {
                            continue;
                        }
                        const int row = (y - 245) / 162;
                        if (row < 0 || row >= 3 || std::abs(y - (270 + row * 162)) > 30) {
                            continue;
                        }
                        slots[{ x < 760 ? 0 : 1, row }] = result.text;
                    }
                }
                if (sample > 0 && slots == previous && (!slots.empty() || *count == 0)) {
                    stable = std::move(slots);
                    break;
                }
                previous = std::move(slots);
            }
            if (!stable.has_value()) {
                return std::nullopt;
            }
            if (!previous_page.empty()) {
                double largest_difference = 0;
                for (int row = 0; row < 3; ++row) {
                    const Rect names_roi { 348, 245 + row * 162, 844, 45 };
                    const auto old_names = make_roi(previous_page, names_roi);
                    const auto new_names = make_roi(image, names_roi);
                    largest_difference = std::max(
                        largest_difference,
                        cv::norm(old_names, new_names, cv::NORM_L1) /
                            static_cast<double>(new_names.total() * new_names.channels()));
                }
                if (largest_difference <= 3.0) {
                    // 手势丢失或列表未动时，不能把同一列再加一次来凑满容量。
                    return std::nullopt;
                }
            }
            previous_page = image.clone();
            for (const auto& [slot, name] : *stable) {
                ++observed;
                if (std::ranges::find(natural_names, name) != natural_names.end()) {
                    naturals.push_back(name);
                }
            }
            report_sacrifice(
                phase,
                json::object { { "scan_page", page },
                               { "inventory_count", *count },
                               { "observed_count", observed },
                               { "natural_items", json::array(naturals) } },
                image);
            if (observed == *count) {
                return naturals;
            }
            if (observed > *count) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    };
    auto result = scan();
    const bool closed = set_panel(false);
    if (!closed) {
        result.reset();
    }
    report_sacrifice(
        phase,
        json::object {
            { "inventory_status", result.has_value() ? "complete" : "incomplete" },
            { "panel_closed", closed },
            { "natural_items", result.has_value() ? json::value(json::array(*result)) : json::value(nullptr) } },
        ctrler()->get_image());
    return result;
}

bool asst::RoguelikeStageEncounterTaskPlugin::finish_sacrifice_civilization()
{
    using blackflow::SacrificePhase;
    const auto continue_later = [&] {
        Task.set_task_base(std::string(Prefix) + "StageEncounterResult", std::string(Prefix) + "SacrificeContinuation");
        return !need_exit();
    };
    if (m_sacrifice.phase == SacrificePhase::BeforeCivilization) {
        m_sacrifice.naturals_before = scan_sacrifice_natural_items("civilization-before");
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
                report_sacrifice("civilization-popup", {}, image);
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
        auto after = scan_sacrifice_natural_items("civilization-after");
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
        report_sacrifice("civilization-consumed", std::move(details), ctrler()->get_image());
        m_sacrifice.phase = SacrificePhase::Finished;
        return true;
    }
    return continue_later();
}
