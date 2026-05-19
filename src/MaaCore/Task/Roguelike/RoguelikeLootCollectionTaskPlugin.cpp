#include "RoguelikeLootCollectionTaskPlugin.h"

#include <algorithm>
#include <initializer_list>
#include <string_view>

#include "Controller/Controller.h"
#include "Task/Roguelike/RoguelikeDataCollection.h"
#include "Utils/Logger.hpp"
#include "Vision/OCRer.h"

namespace
{
constexpr int LootPageStableDelay = 1000;

std::string_view leaf_task_name(std::string_view task_name)
{
    if (const size_t pos = task_name.rfind('@'); pos != std::string_view::npos) {
        task_name.remove_prefix(pos + 1);
    }
    if (const size_t pos = task_name.find('#'); pos != std::string_view::npos) {
        task_name.remove_suffix(task_name.size() - pos);
    }
    return task_name;
}

bool is_direct_loot_take_task(std::string_view task_leaf)
{
    return task_leaf == "GetDrop1" || task_leaf == "GetDrop2" || task_leaf == "GetDropRecruit";
}

bool is_multi_select_loot_task(std::string_view task_leaf)
{
    return task_leaf == "GetDropSelectRecruit" || task_leaf == "GetDropSelectReward" ||
           task_leaf == "GetDropSwitch";
}

bool is_select_entry_task(std::string_view task_leaf)
{
    return task_leaf == "GetDropSelect" || task_leaf == "GetDropSelect_default";
}

bool is_collectible_popup_task(std::string_view task_leaf)
{
    return task_leaf == "CloseCollectionClose" || task_leaf == "CloseCollectionContinue";
}

bool is_recruit_screen_task(std::string_view task_leaf)
{
    return task_leaf == "ChooseOperFlag" || task_leaf == "ChooseOper" || task_leaf == "RecruitMain" ||
           task_leaf == "RecruitOther";
}

bool is_direct_loot_terminal_task(std::string_view task_leaf)
{
    return task_leaf == "DropsFlag" || task_leaf == "GetDropCompleted" || task_leaf == "GetDropLeave" ||
           task_leaf == "ClickToDrops" || task_leaf == "Stages" || task_leaf == "Routing-DataCollection";
}

bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool is_loot_page_reset_task(std::string_view task_leaf)
{
    return task_leaf == "GetDropCompleted" || task_leaf == "GetDropLeave" ||
           starts_with(task_leaf, "GetDropLeave") || task_leaf == "Stages" ||
           task_leaf == "Routing-DataCollection" || task_leaf == "NextLevel" ||
           task_leaf == "RandomPickAfterNextLevel" || task_leaf == "CloseEvent" ||
           task_leaf == "ExitThenAbandon" || task_leaf == "Abandon" || task_leaf == "Begin" ||
           task_leaf == "StrategyChange" || is_recruit_screen_task(task_leaf) || starts_with(task_leaf, "Stage");
}

bool contains_any(std::string_view text, std::initializer_list<std::string_view> needles)
{
    return std::ranges::any_of(needles, [&](std::string_view needle) { return text.find(needle) != std::string_view::npos; });
}

bool read_result_rect(const json::value& details, asst::Rect& rect)
{
    const int x = details.get("details", "result", "rect", 0, -1);
    const int y = details.get("details", "result", "rect", 1, -1);
    const int width = details.get("details", "result", "rect", 2, -1);
    const int height = details.get("details", "result", "rect", 3, -1);
    if (x < 0 || y < 0 || width <= 0 || height <= 0) {
        return false;
    }

    rect = asst::Rect(x, y, width, height);
    return true;
}

asst::Rect clamp_to_image(asst::Rect rect, const cv::Mat& image)
{
    rect.x = std::clamp(rect.x, 0, image.cols);
    rect.y = std::clamp(rect.y, 0, image.rows);
    rect.width = std::clamp(rect.width, 0, image.cols - rect.x);
    rect.height = std::clamp(rect.height, 0, image.rows - rect.y);
    return rect;
}

bool is_meaningful_collectible_text(std::string_view text)
{
    if (text.size() < 6) {
        return false;
    }

    return !contains_any(
        text,
        {
            "收下",
            "直接离开",
            "队伍中",
            "已有",
            "招募",
            "干员",
            "抉择",
            "选择一枚通宝",
            "选择一张通宝",
            "源石锭",
            "希望",
            "目标生命",
            "目标生命值",
            "指挥等级",
            "收藏品",
            "编队",
            "LV",
        });
}
} // namespace

bool asst::RoguelikeLootCollectionTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (msg != AsstMsg::SubTaskStart || details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }

    if (m_config->get_theme() != RoguelikeTheme::JieGarden ||
        m_config->get_mode() != RoguelikeMode::DataCollection) {
        return false;
    }

    const std::string task_name = details.get("details", "task", "");
    const std::string_view task_leaf = leaf_task_name(task_name);
    m_capture_action = CaptureAction::None;
    m_loot_type.clear();
    m_image_suffix.clear();
    m_current_capture_is_collectible_choice = false;

    if (task_leaf == "ClickToDrops") {
        reset_loot_page_state();
        m_in_loot_page = true;
        return false;
    }

    if (!m_pending_direct_loot_image.empty()) {
        if (is_collectible_popup_task(task_leaf)) {
            m_capture_action = CaptureAction::FinalizePending;
            m_loot_type = "Collectible";
            m_image_suffix = "collectible";
            return true;
        }
        if (is_recruit_screen_task(task_leaf)) {
            m_capture_action = CaptureAction::FinalizePending;
            m_loot_type = "Recruit";
            m_image_suffix = "recruit";
            return true;
        }
        if (is_direct_loot_terminal_task(task_leaf) || is_select_entry_task(task_leaf)) {
            m_capture_action = CaptureAction::ClassifyPending;
            return true;
        }
        if (is_direct_loot_take_task(task_leaf) || is_multi_select_loot_task(task_leaf)) {
            clear_pending_direct_loot();
        }
    }

    if (task_leaf == "DropsFlag") {
        if (m_in_loot_page) {
            m_collectible_choice_captured_before_popup = false;
        }
        return false;
    }

    if (is_collectible_popup_task(task_leaf) && m_in_loot_page) {
        if (m_collectible_choice_captured_before_popup) {
            m_collectible_choice_captured_before_popup = false;
            return false;
        }

        m_loot_type = "Collectible";
        m_image_suffix = "collectible";
        m_capture_action = CaptureAction::Immediate;
        return true;
    }

    if (task_leaf == "GetDropSelectRecruit") {
        m_loot_type = "Recruit";
        m_image_suffix = "recruit";
        m_capture_action = CaptureAction::Immediate;
        return true;
    }
    if (task_leaf == "GetDropSelectReward") {
        m_loot_type = "Collectible";
        m_image_suffix = "collectible";
        m_current_capture_is_collectible_choice = true;
        m_capture_action = CaptureAction::Immediate;
        return true;
    }
    if (task_leaf == "GetDropSwitch") {
        m_loot_type = "Copper";
        m_image_suffix = "copper";
        m_capture_action = CaptureAction::Immediate;
        return true;
    }
    if (is_direct_loot_take_task(task_leaf)) {
        m_pending_direct_loot_task = std::string(task_leaf);
        m_has_pending_direct_loot_button_rect = read_result_rect(details, m_pending_direct_loot_button_rect);
        m_capture_action = CaptureAction::PendingDirect;
        return true;
    }

    if (is_select_entry_task(task_leaf) && m_in_loot_page && !m_loot_page_fallback_captured) {
        m_capture_action = CaptureAction::LootPageFallback;
        return true;
    }
    if ((task_leaf == "GetDropCompleted" || starts_with(task_leaf, "GetDropLeave")) && m_in_loot_page &&
        !m_loot_page_fallback_captured) {
        m_capture_action = CaptureAction::LootPageFallback;
        return true;
    }

    if (is_loot_page_reset_task(task_leaf)) {
        reset_loot_page_state();
    }

    return false;
}

bool asst::RoguelikeLootCollectionTaskPlugin::_run()
{
    if (!RoguelikeDataCollector.enabled()) {
        return true;
    }

    switch (m_capture_action) {
    case CaptureAction::Immediate: {
        cv::Mat image;
        if (!capture_loot_action_image(image)) {
            return false;
        }
        const std::string loot_type = m_loot_type;
        const bool collectible_choice_capture = m_current_capture_is_collectible_choice;
        const bool ret = save_loot_image_and_record(image);
        if (loot_type == "Collectible" && collectible_choice_capture) {
            m_collectible_choice_captured_before_popup = true;
        }
        else if (loot_type == "Recruit") {
            reset_loot_page_state();
        }
        return ret;
    }
    case CaptureAction::PendingDirect: {
        cv::Mat image;
        if (!capture_loot_action_image(image)) {
            return false;
        }
        m_pending_direct_loot_image = image.clone();
        if (m_has_pending_direct_loot_button_rect) {
            Log.info(
                __FUNCTION__,
                "| pending direct loot screenshot:",
                m_pending_direct_loot_task,
                "button:",
                m_pending_direct_loot_button_rect);
        }
        else {
            Log.info(__FUNCTION__, "| pending direct loot screenshot:", m_pending_direct_loot_task);
        }
        return true;
    }
    case CaptureAction::FinalizePending: {
        const std::string loot_type = m_loot_type;
        const bool ret = save_loot_image_and_record(m_pending_direct_loot_image);
        clear_pending_direct_loot();
        if (loot_type == "Recruit") {
            reset_loot_page_state();
        }
        return ret;
    }
    case CaptureAction::ClassifyPending: {
        const PendingDirectLootKind kind = classify_pending_direct_loot();
        if (kind == PendingDirectLootKind::Collectible) {
            m_loot_type = "Collectible";
            m_image_suffix = "collectible";
            const bool ret = save_loot_image_and_record(m_pending_direct_loot_image);
            clear_pending_direct_loot();
            return ret;
        }
        if (kind == PendingDirectLootKind::Recruit) {
            Log.trace(__FUNCTION__, "| keep recruit direct loot screenshot:", m_pending_direct_loot_task);
            return true;
        }

        Log.trace(__FUNCTION__, "| clear unclassified direct loot screenshot:", m_pending_direct_loot_task);
        clear_pending_direct_loot();
        return true;
    }
    case CaptureAction::ClearPending:
        Log.trace(__FUNCTION__, "| clear unclassified direct loot screenshot:", m_pending_direct_loot_task);
        clear_pending_direct_loot();
        return true;
    case CaptureAction::LootPageFallback: {
        cv::Mat image;
        return capture_loot_action_image(image);
    }
    case CaptureAction::None:
        return true;
    }

    return true;
}

void asst::RoguelikeLootCollectionTaskPlugin::clear_pending_direct_loot() const
{
    m_pending_direct_loot_image.release();
    m_pending_direct_loot_task.clear();
    m_has_pending_direct_loot_button_rect = false;
    m_pending_direct_loot_button_rect = Rect();
}

void asst::RoguelikeLootCollectionTaskPlugin::reset_loot_page_state() const
{
    clear_pending_direct_loot();
    m_in_loot_page = false;
    m_loot_page_fallback_captured = false;
    m_collectible_choice_captured_before_popup = false;
    m_current_capture_is_collectible_choice = false;
}

bool asst::RoguelikeLootCollectionTaskPlugin::capture_loot_action_image(cv::Mat& image)
{
    if (m_in_loot_page && !m_loot_page_fallback_captured && !sleep(LootPageStableDelay)) {
        return false;
    }

    image = ctrler()->get_image();
    return save_loot_page_fallback_if_needed(image);
}

bool asst::RoguelikeLootCollectionTaskPlugin::save_loot_page_fallback_if_needed(const cv::Mat& image)
{
    if (!m_in_loot_page || m_loot_page_fallback_captured) {
        return true;
    }

    const bool ret = save_loot_image_and_record("LootPage", "loot_page", image);
    m_loot_page_fallback_captured = true;
    return ret;
}

asst::RoguelikeLootCollectionTaskPlugin::PendingDirectLootKind
    asst::RoguelikeLootCollectionTaskPlugin::classify_pending_direct_loot() const
{
    if (m_pending_direct_loot_image.empty()) {
        return PendingDirectLootKind::Unknown;
    }

    if (m_pending_direct_loot_task == "GetDrop2") {
        return PendingDirectLootKind::Other;
    }
    if (m_pending_direct_loot_task == "GetDropRecruit") {
        return PendingDirectLootKind::Recruit;
    }

    Rect roi;
    if (m_has_pending_direct_loot_button_rect) {
        const int left = m_pending_direct_loot_button_rect.x - 40;
        const int top = m_pending_direct_loot_button_rect.y - 310;
        roi = Rect(left, top, m_pending_direct_loot_button_rect.width + 80, 295);
    }
    else {
        roi = Rect(20, 250, 300, 240);
    }
    roi = clamp_to_image(roi, m_pending_direct_loot_image);
    if (roi.width <= 0 || roi.height <= 0) {
        return PendingDirectLootKind::Unknown;
    }

    OCRer analyzer(m_pending_direct_loot_image);
    analyzer.set_roi(roi);
    if (!analyzer.analyze()) {
        return PendingDirectLootKind::Unknown;
    }

    bool has_collectible_text = false;
    bool has_recruit_text = false;
    bool has_other_text = false;
    std::string joined_text;
    for (const auto& result : analyzer.get_result()) {
        const std::string_view text = result.text;
        if (!joined_text.empty()) {
            joined_text += " / ";
        }
        joined_text += result.text;

        if (contains_any(text, { "招募券" }) || (contains_any(text, { "招募" }) && contains_any(text, { "干员" }))) {
            has_recruit_text = true;
        }
        if (contains_any(text, { "源石锭", "目标生命", "指挥等级" }) || text == "希望") {
            has_other_text = true;
        }
        if (is_meaningful_collectible_text(text)) {
            has_collectible_text = true;
        }
    }

    if ((contains_any(joined_text, { "抉择", "选择" }) && contains_any(joined_text, { "通宝" })) ||
        contains_any(joined_text, { "选择一枚通宝", "选择一张通宝", "选择通宝" })) {
        has_other_text = true;
    }

    if (has_recruit_text) {
        Log.trace(__FUNCTION__, "| pending direct loot OCR:", joined_text, "roi:", roi, "kind: Recruit");
        return PendingDirectLootKind::Recruit;
    }
    if (has_other_text) {
        Log.trace(__FUNCTION__, "| pending direct loot OCR:", joined_text, "roi:", roi, "kind: Other");
        return PendingDirectLootKind::Other;
    }

    Log.trace(
        __FUNCTION__,
        "| pending direct loot OCR:",
        joined_text,
        "roi:",
        roi,
        "kind:",
        has_collectible_text ? "Collectible" : "Unknown");
    return has_collectible_text ? PendingDirectLootKind::Collectible : PendingDirectLootKind::Unknown;
}

bool asst::RoguelikeLootCollectionTaskPlugin::save_loot_image_and_record(const cv::Mat& image)
{
    return save_loot_image_and_record(m_loot_type, m_image_suffix, image);
}

bool asst::RoguelikeLootCollectionTaskPlugin::save_loot_image_and_record(
    std::string_view type,
    std::string_view suffix,
    const cv::Mat& image)
{
    if (type.empty() || suffix.empty()) {
        return true;
    }

    const std::string image_path = RoguelikeDataCollector.save_loot_image(image, suffix);
    if (image_path.empty()) {
        Log.warn(__FUNCTION__, "| failed to save loot image:", type);
        return true;
    }

    json::object details = RoguelikeDataCollector.record_loot(type, image_path);
    if (details.empty()) {
        details = json::object {
            { "type", std::string(type) },
            { "image", image_path },
        };
    }
    RoguelikeDataCollector.log_event("loot", std::move(details));
    return true;
}
