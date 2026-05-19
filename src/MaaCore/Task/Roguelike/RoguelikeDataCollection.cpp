#include "RoguelikeDataCollection.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <vector>

#include "MaaUtils/Encoding.h"
#include "MaaUtils/ImageIo.h"
#include "MaaUtils/Time.hpp"
#include "Task/Roguelike/RoguelikeConfig.h"
#include "Utils/Logger.hpp"
#include "Utils/Platform.hpp"
#include "Utils/WorkingDir.hpp"

namespace
{
constexpr const char* ImagesDir = "images";
constexpr const char* EncountersDir = "encounters";
constexpr const char* LegendsDir = "legends";
constexpr const char* BoonsDir = "boons";
constexpr const char* TradersDir = "traders";
constexpr const char* YiTradersDir = "yi_traders";
constexpr const char* AgentsDir = "agents";
constexpr const char* LootsDir = "loots";
constexpr const char* StoneMountainsDir = "stone_mountains";
constexpr const char* TaotieCorridorsDir = "taotie_corridors";
constexpr const char* EncounterCollectiblesDir = "encounter_collectibles";

struct JieGardenFloorRule
{
    std::wstring_view normalized;
    std::string_view canonical;
    int index = 0;
};

constexpr std::array JieGardenFloorRules = {
    JieGardenFloorRule { L"洪陆楼", "洪陆楼", 1 },
    JieGardenFloorRule { L"山水阁", "山水阁", 2 },
    JieGardenFloorRule { L"云瓦亭", "云瓦亭", 3 },
    JieGardenFloorRule { L"汝吾门", "汝吾门", 4 },
    JieGardenFloorRule { L"见字祠", "见字祠", 5 },
    JieGardenFloorRule { L"始末陵", "始末陵", 6 },
    JieGardenFloorRule { L"是非境", "是非境", 0 },
};

bool is_floor_text_separator(wchar_t ch)
{
    if (static_cast<unsigned long>(ch) <= 0x7F &&
        (std::isspace(static_cast<unsigned char>(ch)) || std::ispunct(static_cast<unsigned char>(ch)))) {
        return true;
    }

    constexpr std::wstring_view Separators = L"　，。、；：！？（）【】《》“”‘’·—…";
    return Separators.find(ch) != std::wstring_view::npos;
}

wchar_t normalize_floor_char(wchar_t ch)
{
    switch (ch) {
    case L'陸':
        return L'陆';
    case L'樓':
        return L'楼';
    case L'閣':
        return L'阁';
    case L'雲':
        return L'云';
    case L'門':
        return L'门';
    case L'見':
        return L'见';
    case L'亨':
        return L'亭';
    default:
        return ch;
    }
}

std::wstring normalize_floor_text(std::string_view ocr_text)
{
    std::wstring result;
    for (wchar_t ch : MAA_NS::to_u16(ocr_text)) {
        ch = normalize_floor_char(ch);
        if (is_floor_text_separator(ch)) {
            continue;
        }
        result.push_back(ch);
    }
    return result;
}

bool contains_char(const std::wstring& text, wchar_t ch)
{
    return text.find(ch) != std::wstring::npos;
}

bool contains_any_char(const std::wstring& text, std::wstring_view chars)
{
    return std::ranges::any_of(chars, [&](wchar_t ch) { return contains_char(text, ch); });
}

std::string match_jiegarden_floor_by_signature(const std::wstring& text)
{
    if (contains_char(text, L'洪') && contains_any_char(text, L"陆楼")) {
        return "洪陆楼";
    }
    if (contains_char(text, L'山') && contains_any_char(text, L"水阁")) {
        return "山水阁";
    }
    if ((contains_char(text, L'云') && contains_any_char(text, L"瓦亭")) ||
        (contains_char(text, L'瓦') && contains_char(text, L'亭'))) {
        return "云瓦亭";
    }
    if ((contains_char(text, L'汝') && contains_any_char(text, L"吾门")) ||
        (contains_char(text, L'吾') && contains_char(text, L'门'))) {
        return "汝吾门";
    }
    if ((contains_char(text, L'见') && contains_any_char(text, L"字祠")) ||
        (contains_char(text, L'字') && contains_char(text, L'祠'))) {
        return "见字祠";
    }
    if ((contains_char(text, L'始') && contains_any_char(text, L"末陵")) ||
        (contains_char(text, L'末') && contains_char(text, L'陵'))) {
        return "始末陵";
    }
    if ((contains_char(text, L'是') && contains_any_char(text, L"非境")) ||
        (contains_char(text, L'非') && contains_char(text, L'境'))) {
        return "是非境";
    }
    return {};
}

std::string loot_node_type_name(std::string_view raw_node_type)
{
    if (raw_node_type == "CombatOps") {
        return "普通作战";
    }
    if (raw_node_type == "EmergencyOps") {
        return "紧急作战";
    }
    if (raw_node_type == "Guidance") {
        return "指点迷津作战";
    }
    if (raw_node_type == "DreadfulFoe") {
        return "险路恶敌";
    }
    if (raw_node_type.empty()) {
        return "未知节点";
    }
    return std::string(raw_node_type);
}

std::string loot_image_key(std::string_view type)
{
    if (type == "Copper") {
        return "copper_image";
    }
    if (type == "Recruit") {
        return "recruit_image";
    }
    if (type == "Collectible") {
        return "collectible_image";
    }
    return "image";
}

size_t edit_distance(std::wstring_view lhs, std::wstring_view rhs)
{
    std::vector<size_t> prev(rhs.size() + 1);
    std::vector<size_t> cur(rhs.size() + 1);
    for (size_t i = 0; i <= rhs.size(); ++i) {
        prev[i] = i;
    }

    for (size_t i = 1; i <= lhs.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= rhs.size(); ++j) {
            const size_t cost = lhs[i - 1] == rhs[j - 1] ? 0 : 1;
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
        }
        std::swap(prev, cur);
    }
    return prev[rhs.size()];
}

int jiegarden_floor_index(std::string_view floor)
{
    auto it = std::ranges::find_if(JieGardenFloorRules, [&](const JieGardenFloorRule& rule) {
        return rule.canonical == floor;
    });
    return it == JieGardenFloorRules.end() ? 0 : it->index;
}

std::string bosky_agent_source_from_event(std::string_view event_name)
{
    if (event_name == "天圆地方") {
        return "筹谋";
    }
    if (event_name == "所见") {
        return "拾遗";
    }
    if (event_name == "点睛") {
        return "故肆";
    }
    if (event_name == "三缺一" || event_name == "种因得果" || event_name == "掷地有声") {
        return "常乐";
    }
    return {};
}

std::string agent_source_node_type(
    std::string_view raw_node_type,
    std::string_view event_name,
    std::string_view record_type,
    std::string_view floor)
{
    if (record_type == "Legend" || raw_node_type == "Legend") {
        return "传说";
    }
    if (floor == "是非境") {
        if (std::string bosky_source = bosky_agent_source_from_event(event_name); !bosky_source.empty()) {
            return bosky_source;
        }
    }

    if (raw_node_type == "Encounter") {
        return "不期而遇";
    }
    if (raw_node_type == "SafeHouse") {
        return "安全的角落";
    }
    if (raw_node_type == "LostAndFound") {
        return "失与得";
    }
    if (raw_node_type == "Guidance") {
        return "指点迷津";
    }
    if (raw_node_type == "Scout") {
        return "先行一步";
    }
    if (raw_node_type == "Boons") {
        return "得偿所愿";
    }
    if (raw_node_type == "RogueTrader") {
        return "诡异行商";
    }
    if (raw_node_type == "BoskyPassage") {
        return "误入奇境";
    }
    if (raw_node_type == "Omissions") {
        return "拾遗";
    }
    if (raw_node_type == "Doubts") {
        return "杂疑";
    }
    if (raw_node_type == "Playtime") {
        return "常乐";
    }
    if (raw_node_type == "OldShop") {
        return "故肆";
    }
    if (raw_node_type == "YiTrader") {
        return "易与";
    }
    if (raw_node_type == "Scheme") {
        return "筹谋";
    }
    if (raw_node_type == "Disaster") {
        return "祸乱";
    }
    return raw_node_type.empty() ? "Unknown" : std::string(raw_node_type);
}
}

void asst::RoguelikeDataCollection::start_session(const RoguelikeConfig& config, const json::value& params)
{
    std::lock_guard lock(m_mutex);

    m_session_dir = UserDir.get() / "debug" / "roguelike" / "data_collection" /
                    utils::path(MAA_NS::format_now_for_filename());
    std::filesystem::create_directories(m_session_dir / ImagesDir);
    std::filesystem::create_directories(m_session_dir / EncountersDir);
    std::filesystem::create_directories(m_session_dir / LegendsDir);
    std::filesystem::create_directories(m_session_dir / TradersDir);
    std::filesystem::create_directories(m_session_dir / YiTradersDir);
    std::filesystem::create_directories(m_session_dir / AgentsDir);
    std::filesystem::create_directories(m_session_dir / LootsDir);
    std::filesystem::create_directories(m_session_dir / StoneMountainsDir);
    std::filesystem::create_directories(m_session_dir / TaotieCorridorsDir);
    std::filesystem::create_directories(m_session_dir / EncounterCollectiblesDir);
    m_current_floor = "未知层";
    m_floor_index = 0;
    m_run_active = true;
    m_pending_abandon_reason.clear();
    m_pending_abandon_details.clear();
    m_encounter_summary.clear();
    m_boon_summary.clear();
    m_trader_summary.clear();
    m_agent_summary.clear();
    m_loot_summary.clear();
    m_stone_mountain_summary.clear();
    m_taotie_corridor_summary.clear();
    m_encounter_collectible_summary.clear();
    m_record_map_encounters = false;
    m_map_encounter_type = "Encounter";
    m_last_selected_node_type = "Unknown";
    m_pending_agent_source_node_type = "Unknown";
    m_pending_agent_source_event_name.clear();
    m_pending_agent_source_record_type = "Encounter";

    json::object params_summary {
        { "starts_count", params.get("starts_count", -1) },
        { "roles", params.get("roles", "") },
        { "use_support", params.get("use_support", false) },
        { "use_nonfriend_support", params.get("use_nonfriend_support", false) },
        { "start_with_seed", params.get("start_with_seed", "") },
    };

    json::value session = json::object {
        { "theme", config.get_theme() },
        { "mode", static_cast<int>(config.get_mode()) },
        { "difficulty", config.get_difficulty() },
        { "squad", params.get("squad", "") },
        { "core_char", params.get("core_char", "") },
        { "start_time", MAA_NS::format_now() },
        { "params_summary", std::move(params_summary) },
    };

    std::ofstream ofs(m_session_dir / "session.json");
    ofs << session.format();
    m_enabled = true;

    Log.info(__FUNCTION__, "| started roguelike data collection session", utils::path_to_utf8_string(m_session_dir));
}

void asst::RoguelikeDataCollection::stop_session(std::string_view reason)
{
    log_event("session_end", json::object { { "reason", std::string(reason) } });
    flush_encounter_summary(true);
    flush_boon_summary(true);
    flush_trader_summary(true);
    flush_agent_summary(true);
    flush_loot_summary(true);
    flush_stone_mountain_summary(true);
    flush_taotie_corridor_summary(true);
    flush_encounter_collectible_summary(true);
    disable();
}

void asst::RoguelikeDataCollection::finish_run(std::string_view reason)
{
    log_event("run_end", json::object { { "reason", std::string(reason) } });
    flush_encounter_summary(true);
    flush_boon_summary(true);
    flush_trader_summary(true);
    flush_agent_summary(true);
    flush_loot_summary(true);
    flush_stone_mountain_summary(true);
    flush_taotie_corridor_summary(true);
    flush_encounter_collectible_summary(true);

    std::lock_guard lock(m_mutex);
    m_current_floor = "未知层";
    m_floor_index = 0;
    m_run_active = false;
    m_pending_abandon_reason.clear();
    m_pending_abandon_details.clear();
    m_record_map_encounters = false;
    m_map_encounter_type = "Encounter";
    m_last_selected_node_type = "Unknown";
    m_pending_agent_source_node_type = "Unknown";
    m_pending_agent_source_event_name.clear();
    m_pending_agent_source_record_type = "Encounter";
}

void asst::RoguelikeDataCollection::finish_run_if_active(std::string_view reason)
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_enabled || m_session_dir.empty() || !m_run_active) {
            return;
        }
    }

    log_event("run_end", json::object { { "reason", std::string(reason) } });
    flush_encounter_summary(false);
    flush_boon_summary(false);
    flush_trader_summary(false);
    flush_agent_summary(false);
    flush_loot_summary(false);
    flush_stone_mountain_summary(false);
    flush_taotie_corridor_summary(false);
    flush_encounter_collectible_summary(false);

    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty()) {
        return;
    }
    m_current_floor = "未知层";
    m_floor_index = 0;
    m_run_active = false;
    m_pending_abandon_reason.clear();
    m_pending_abandon_details.clear();
    m_record_map_encounters = false;
    m_map_encounter_type = "Encounter";
    m_last_selected_node_type = "Unknown";
    m_pending_agent_source_node_type = "Unknown";
    m_pending_agent_source_event_name.clear();
    m_pending_agent_source_record_type = "Encounter";
}

void asst::RoguelikeDataCollection::finish_run_if_has_cached_encounters(std::string_view reason)
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_enabled || m_session_dir.empty() ||
            (m_encounter_summary.empty() && m_boon_summary.empty() && m_trader_summary.empty() && m_agent_summary.empty() &&
             m_loot_summary.empty() && m_stone_mountain_summary.empty() && m_taotie_corridor_summary.empty() &&
             m_encounter_collectible_summary.empty())) {
            return;
        }
    }

    log_event("run_end", json::object { { "reason", std::string(reason) } });
    flush_encounter_summary(false);
    flush_boon_summary(false);
    flush_trader_summary(false);
    flush_agent_summary(false);
    flush_loot_summary(false);
    flush_stone_mountain_summary(false);
    flush_taotie_corridor_summary(false);
    flush_encounter_collectible_summary(false);

    std::lock_guard lock(m_mutex);
    m_current_floor = "未知层";
    m_floor_index = 0;
    m_run_active = false;
    m_pending_abandon_reason.clear();
    m_pending_abandon_details.clear();
    m_record_map_encounters = false;
    m_map_encounter_type = "Encounter";
    m_last_selected_node_type = "Unknown";
    m_pending_agent_source_node_type = "Unknown";
    m_pending_agent_source_event_name.clear();
    m_pending_agent_source_record_type = "Encounter";
}

void asst::RoguelikeDataCollection::start_run_if_enabled()
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty()) {
        return;
    }

    if (!m_run_active) {
        m_current_floor = "未知层";
        m_floor_index = 0;
        m_pending_abandon_reason.clear();
        m_pending_abandon_details.clear();
        m_record_map_encounters = false;
        m_map_encounter_type = "Encounter";
        m_last_selected_node_type = "Unknown";
        m_pending_agent_source_node_type = "Unknown";
        m_pending_agent_source_event_name.clear();
        m_pending_agent_source_record_type = "Encounter";
    }
    m_run_active = true;
}

void asst::RoguelikeDataCollection::set_pending_abandon_reason(std::string_view reason, json::object details)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty()) {
        return;
    }

    m_run_active = true;
    m_pending_abandon_reason = reason;
    m_pending_abandon_details = std::move(details);
}

void asst::RoguelikeDataCollection::finish_run_as_abandoned(
    std::string_view default_reason,
    const cv::Mat& image,
    json::object details)
{
    std::string reason;
    json::object pending_details;
    {
        std::lock_guard lock(m_mutex);
        if (!m_enabled || m_session_dir.empty() || !m_run_active) {
            return;
        }

        reason = m_pending_abandon_reason.empty() ? std::string(default_reason) : std::move(m_pending_abandon_reason);
        pending_details = std::move(m_pending_abandon_details);
        m_pending_abandon_reason.clear();
        m_pending_abandon_details.clear();
    }

    pending_details |= std::move(details);
    pending_details["reason"] = reason;
    const std::string image_path = save_image(image, "abandon");
    if (!image_path.empty()) {
        pending_details["image"] = image_path;
    }
    log_event("run_abandon", std::move(pending_details));
    finish_run_if_active(reason);
}

void asst::RoguelikeDataCollection::disable()
{
    flush_encounter_summary();
    flush_boon_summary();
    flush_trader_summary();
    flush_agent_summary();
    flush_loot_summary();
    flush_stone_mountain_summary();
    flush_taotie_corridor_summary();
    flush_encounter_collectible_summary();

    std::lock_guard lock(m_mutex);
    m_enabled = false;
    m_session_dir.clear();
    m_current_floor.clear();
    m_floor_index = 0;
    m_run_active = false;
    m_pending_abandon_reason.clear();
    m_pending_abandon_details.clear();
    m_encounter_summary.clear();
    m_boon_summary.clear();
    m_trader_summary.clear();
    m_agent_summary.clear();
    m_loot_summary.clear();
    m_stone_mountain_summary.clear();
    m_taotie_corridor_summary.clear();
    m_encounter_collectible_summary.clear();
    m_record_map_encounters = false;
    m_map_encounter_type = "Encounter";
    m_last_selected_node_type = "Unknown";
    m_pending_agent_source_node_type = "Unknown";
    m_pending_agent_source_event_name.clear();
    m_pending_agent_source_record_type = "Encounter";
}

bool asst::RoguelikeDataCollection::enabled() const
{
    std::lock_guard lock(m_mutex);
    return m_enabled;
}

const std::filesystem::path& asst::RoguelikeDataCollection::session_dir() const
{
    return m_session_dir;
}

void asst::RoguelikeDataCollection::log_event(std::string_view type, json::object details)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty()) {
        return;
    }

    json::value event = json::object {
        { "time", MAA_NS::format_now() },
        { "type", std::string(type) },
        { "details", std::move(details) },
    };

    try {
        std::filesystem::create_directories(m_session_dir);
        std::ofstream ofs(m_session_dir / "events.jsonl", std::ios::app);
        ofs << event.to_string() << '\n';
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to write data collection event:", e.what());
    }
}

std::string asst::RoguelikeDataCollection::save_image(const cv::Mat& image, std::string_view suffix)
{
    return save_image(image, suffix, ImagesDir);
}

std::string asst::RoguelikeDataCollection::save_encounter_image(const cv::Mat& image)
{
    return save_image(image, "encounter", EncountersDir);
}

std::string asst::RoguelikeDataCollection::save_legend_image(const cv::Mat& image)
{
    return save_image(image, "legend", LegendsDir);
}

std::string asst::RoguelikeDataCollection::save_boon_image(const cv::Mat& image)
{
    return save_image(image, "boons", BoonsDir);
}

std::string asst::RoguelikeDataCollection::save_trader_image(const cv::Mat& image)
{
    return save_image(image, "trader", TradersDir);
}

std::string asst::RoguelikeDataCollection::save_yi_trader_image(const cv::Mat& image)
{
    return save_image(image, "yi_trader", YiTradersDir);
}

std::string asst::RoguelikeDataCollection::save_agent_image(const cv::Mat& image)
{
    return save_image(image, "agents", AgentsDir);
}

std::string asst::RoguelikeDataCollection::save_agent_treasure_image(const cv::Mat& image)
{
    return save_image(image, "agent_treasure", AgentsDir);
}

std::string asst::RoguelikeDataCollection::save_agent_collectible_image(const cv::Mat& image)
{
    return save_image(image, "agent_collectible", AgentsDir);
}

std::string asst::RoguelikeDataCollection::save_loot_image(const cv::Mat& image, std::string_view suffix)
{
    return save_image(image, suffix, LootsDir);
}

std::string asst::RoguelikeDataCollection::save_stone_mountain_image(const cv::Mat& image)
{
    return save_image(image, "stone_mountain", StoneMountainsDir);
}

std::string asst::RoguelikeDataCollection::save_taotie_corridor_image(
    const cv::Mat& image,
    std::string_view suffix)
{
    return save_image(image, suffix, TaotieCorridorsDir);
}

std::string asst::RoguelikeDataCollection::save_encounter_collectible_image(
    const cv::Mat& image,
    std::string_view suffix)
{
    return save_image(image, suffix, EncounterCollectiblesDir);
}

std::string asst::RoguelikeDataCollection::normalize_jiegarden_floor_name(std::string_view ocr_text)
{
    const std::wstring normalized = normalize_floor_text(ocr_text);
    if (normalized.empty()) {
        return {};
    }

    for (const auto& rule : JieGardenFloorRules) {
        if (normalized.find(rule.normalized) != std::wstring::npos) {
            return std::string(rule.canonical);
        }
    }

    if (std::string signature_match = match_jiegarden_floor_by_signature(normalized); !signature_match.empty()) {
        return signature_match;
    }

    size_t best_distance = std::numeric_limits<size_t>::max();
    size_t second_best_distance = std::numeric_limits<size_t>::max();
    const JieGardenFloorRule* best_rule = nullptr;
    for (const auto& rule : JieGardenFloorRules) {
        const size_t distance = edit_distance(normalized, rule.normalized);
        if (distance < best_distance) {
            second_best_distance = best_distance;
            best_distance = distance;
            best_rule = &rule;
        }
        else if (distance < second_best_distance) {
            second_best_distance = distance;
        }
    }

    if (best_rule != nullptr && best_distance <= 1 && best_distance < second_best_distance) {
        return std::string(best_rule->canonical);
    }

    return {};
}

std::string asst::RoguelikeDataCollection::save_image(
    const cv::Mat& image,
    std::string_view suffix,
    std::string_view category_dir)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty() || image.empty()) {
        return {};
    }

    try {
        const auto filename =
            utils::path(std::format("{}_{}.png", MAA_NS::format_now_for_filename(), std::string(suffix)));
        const auto path = m_session_dir / utils::path(std::string(category_dir)) / filename;
        std::filesystem::create_directories(path.parent_path());
        if (MAA_NS::imwrite(path, image)) {
            if (category_dir != ImagesDir) {
                link_to_images_dir(path, filename);
            }
            return utils::path_to_utf8_string(path);
        }
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to save data collection image:", e.what());
    }

    return {};
}

bool asst::RoguelikeDataCollection::link_to_images_dir(
    const std::filesystem::path& target,
    const std::filesystem::path& filename)
{
    try {
        const auto link_path = m_session_dir / ImagesDir / filename;
        std::filesystem::create_directories(link_path.parent_path());
        if (std::filesystem::exists(link_path)) {
            std::filesystem::remove(link_path);
        }
        std::filesystem::create_hard_link(target, link_path);
        return true;
    }
    catch (const std::exception& e) {
        Log.error(
            __FUNCTION__,
            "| failed to create data collection image link:",
            utils::path_to_utf8_string(target),
            e.what());
    }

    return false;
}

void asst::RoguelikeDataCollection::note_floor_ocr(std::string_view ocr_text)
{
    const std::string floor = normalize_jiegarden_floor_name(ocr_text);
    if (floor.empty()) {
        return;
    }

    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty()) {
        return;
    }

    m_current_floor = floor;
    m_run_active = true;
    if (const int floor_index = jiegarden_floor_index(floor); floor_index > 0) {
        m_floor_index = floor_index;
    }
}

void asst::RoguelikeDataCollection::note_strategy_change(std::string_view strategy)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty()) {
        return;
    }

    if (strategy == "_dataCollection" || strategy == "_exit") {
        m_run_active = true;
        ++m_floor_index;
        switch (m_floor_index) {
        case 1:
            m_current_floor = "洪陆楼";
            break;
        case 2:
            m_current_floor = "山水阁";
            break;
        case 3:
            m_current_floor = "云瓦亭";
            break;
        case 4:
            m_current_floor = "汝吾门";
            break;
        case 5:
            m_current_floor = "见字祠";
            break;
        case 6:
            m_current_floor = "始末陵";
            break;
        default:
            m_current_floor = std::format("第{}层", m_floor_index);
            break;
        }
    }
    else if (strategy == "_leaveBoskyPassage" || strategy == "_boskyPassageDefault") {
        m_run_active = true;
        m_current_floor = "是非境";
    }
}

void asst::RoguelikeDataCollection::note_selected_node_type(std::string_view node_type)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty()) {
        return;
    }

    m_run_active = true;
    m_last_selected_node_type = node_type.empty() ? "Unknown" : std::string(node_type);
    m_pending_agent_source_node_type = "Unknown";
    m_pending_agent_source_event_name.clear();
    m_pending_agent_source_record_type = "Encounter";
}

void asst::RoguelikeDataCollection::note_agent_source(std::string_view event_name, std::string_view record_type)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty()) {
        return;
    }

    m_run_active = true;
    m_pending_agent_source_node_type = m_last_selected_node_type.empty() ? "Unknown" : m_last_selected_node_type;
    m_pending_agent_source_event_name = std::string(event_name);
    m_pending_agent_source_record_type = record_type.empty() ? "Encounter" : std::string(record_type);
}

std::string asst::RoguelikeDataCollection::selected_node_type() const
{
    std::lock_guard lock(m_mutex);
    return m_last_selected_node_type;
}

bool asst::RoguelikeDataCollection::current_floor_is_bosky_passage() const
{
    std::lock_guard lock(m_mutex);
    return m_enabled && !m_session_dir.empty() && m_current_floor == "是非境";
}

void asst::RoguelikeDataCollection::set_record_map_encounters(bool enabled, std::string_view type)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty()) {
        return;
    }
    m_run_active = true;
    m_record_map_encounters = enabled;
    m_map_encounter_type = enabled ? std::string(type) : "Encounter";
}

bool asst::RoguelikeDataCollection::should_record_map_encounters() const
{
    std::lock_guard lock(m_mutex);
    return m_enabled && !m_session_dir.empty() && m_record_map_encounters;
}

std::string asst::RoguelikeDataCollection::map_encounter_type() const
{
    std::lock_guard lock(m_mutex);
    return m_map_encounter_type;
}

void asst::RoguelikeDataCollection::record_encounter(
    std::string_view name,
    std::string_view image_path,
    std::string_view type)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty() || !m_record_map_encounters) {
        return;
    }

    const std::string floor = m_current_floor.empty() ? "未知层" : m_current_floor;
    m_run_active = true;
    if (!m_encounter_summary.contains(floor)) {
        m_encounter_summary[floor] = json::array {};
    }

    m_encounter_summary[floor].as_array().emplace_back(json::object {
        { "type", std::string(type) },
        { "name", std::string(name) },
        { "image", std::string(image_path) },
    });
}

void asst::RoguelikeDataCollection::record_boon(
    std::string_view name,
    std::string_view image_path,
    json::array options,
    size_t selected_choice,
    std::string_view selected_option,
    bool ocr_failed)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty()) {
        return;
    }

    const std::string floor = m_current_floor.empty() ? "未知层" : m_current_floor;
    m_run_active = true;
    if (!m_boon_summary.contains(floor)) {
        m_boon_summary[floor] = json::array {};
    }

    json::object record {
        { "type", "Boons" },
        { "node_type", "得偿所愿" },
        { "source_node_type", "Boons" },
        { "name", std::string(name) },
        { "image", std::string(image_path) },
        { "floor", floor },
        { "floor_index", m_floor_index },
        { "options", std::move(options) },
        { "selected_choice", static_cast<int>(selected_choice) },
        { "selected_option", std::string(selected_option) },
        { "ocr_failed", ocr_failed },
    };

    m_boon_summary[floor].as_array().emplace_back(std::move(record));
}

void asst::RoguelikeDataCollection::record_trader(
    std::string_view name,
    std::string_view image_path,
    bool is_yi_trader)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty()) {
        return;
    }

    const std::string floor = is_yi_trader ? "是非境" : (m_current_floor.empty() ? "未知层" : m_current_floor);
    m_run_active = true;
    if (!m_trader_summary.contains(floor)) {
        m_trader_summary[floor] = json::array {};
    }

    m_trader_summary[floor].as_array().emplace_back(json::object {
        { "type", is_yi_trader ? "YiTrader" : "Trader" },
        { "name", std::string(name) },
        { "image", std::string(image_path) },
    });
}

json::object asst::RoguelikeDataCollection::build_agent_source_details_locked() const
{
    const std::string raw_node_type =
        m_pending_agent_source_node_type.empty() ? "Unknown" : m_pending_agent_source_node_type;
    const std::string record_type =
        m_pending_agent_source_record_type.empty() ? "Encounter" : m_pending_agent_source_record_type;
    const std::string node_type =
        agent_source_node_type(raw_node_type, m_pending_agent_source_event_name, record_type, m_current_floor);

    json::object details {
        { "node_type", node_type },
        { "source_node_type", raw_node_type },
        { "source_record_type", record_type },
    };
    if (!m_pending_agent_source_event_name.empty()) {
        details["source_event_name"] = m_pending_agent_source_event_name;
    }
    return details;
}

json::object asst::RoguelikeDataCollection::record_agent(
    std::string_view name,
    std::string_view image_path,
    json::object extra_details)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty()) {
        return {};
    }

    const std::string floor = m_current_floor.empty() ? "未知层" : m_current_floor;
    m_run_active = true;
    if (!m_agent_summary.contains(floor)) {
        m_agent_summary[floor] = json::array {};
    }

    json::object source_details = build_agent_source_details_locked();
    json::object record {
        { "type", "Agents" },
        { "name", std::string(name) },
        { "image", std::string(image_path) },
    };
    record |= source_details;
    record |= extra_details;
    m_agent_summary[floor].as_array().emplace_back(std::move(record));

    source_details |= std::move(extra_details);
    return source_details;
}

json::object asst::RoguelikeDataCollection::record_loot(std::string_view type, std::string_view image_path)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty()) {
        return {};
    }

    const std::string floor = m_current_floor.empty() ? "未知层" : m_current_floor;
    const std::string raw_node_type = m_last_selected_node_type.empty() ? "Unknown" : m_last_selected_node_type;
    const std::string image_key = loot_image_key(type);
    m_run_active = true;
    if (!m_loot_summary.contains(floor)) {
        m_loot_summary[floor] = json::array {};
    }

    json::object record {
        { "type", std::string(type) },
        { "image", std::string(image_path) },
        { "floor", floor },
        { "floor_index", m_floor_index },
        { "node_type", loot_node_type_name(raw_node_type) },
        { "source_node_type", raw_node_type },
    };
    record[image_key] = std::string(image_path);

    m_loot_summary[floor].as_array().emplace_back(record);
    return record;
}

void asst::RoguelikeDataCollection::record_stone_mountain(
    std::string_view image_path,
    size_t selected_choice,
    std::string_view selected_option)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty()) {
        return;
    }

    const std::string floor = m_current_floor.empty() ? "未知层" : m_current_floor;
    m_run_active = true;
    if (!m_stone_mountain_summary.contains(floor)) {
        m_stone_mountain_summary[floor] = json::array {};
    }

    m_stone_mountain_summary[floor].as_array().emplace_back(json::object {
        { "type", "StoneMountain" },
        { "name", "石山" },
        { "image", std::string(image_path) },
        { "selected_choice", static_cast<int>(selected_choice) },
        { "selected_option", std::string(selected_option) },
    });
}

void asst::RoguelikeDataCollection::record_taotie_corridor(
    std::string_view image_path,
    size_t selected_choice,
    std::string_view selected_option,
    std::string_view next_event_image_path,
    size_t next_event_selected_choice,
    std::string_view next_event_selected_option)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty()) {
        return;
    }

    const std::string floor = m_current_floor.empty() ? "未知层" : m_current_floor;
    m_run_active = true;
    if (!m_taotie_corridor_summary.contains(floor)) {
        m_taotie_corridor_summary[floor] = json::array {};
    }

    json::object record {
        { "type", "TaotieCorridor" },
        { "name", "饕餮廊" },
        { "image", std::string(image_path) },
        { "initial_image", std::string(image_path) },
        { "selected_choice", static_cast<int>(selected_choice) },
        { "selected_option", std::string(selected_option) },
    };
    if (!next_event_image_path.empty()) {
        record["next_event_image"] = std::string(next_event_image_path);
        record["next_event_selected_choice"] = static_cast<int>(next_event_selected_choice);
        record["next_event_selected_option"] = std::string(next_event_selected_option);
    }

    m_taotie_corridor_summary[floor].as_array().emplace_back(std::move(record));
}

void asst::RoguelikeDataCollection::record_encounter_collectible(
    std::string_view event_name,
    std::string_view image_path,
    size_t popup_index)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty()) {
        return;
    }

    const std::string floor = m_current_floor.empty() ? "未知层" : m_current_floor;
    m_run_active = true;
    if (!m_encounter_collectible_summary.contains(floor)) {
        m_encounter_collectible_summary[floor] = json::array {};
    }

    m_encounter_collectible_summary[floor].as_array().emplace_back(json::object {
        { "type", "EncounterCollectible" },
        { "event_name", std::string(event_name) },
        { "image", std::string(image_path) },
        { "popup_index", static_cast<int>(popup_index) },
    });
}

void asst::RoguelikeDataCollection::flush_encounter_summary(bool force)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty() || (!force && m_encounter_summary.empty())) {
        return;
    }

    try {
        std::filesystem::create_directories(m_session_dir);
        std::ofstream ofs(m_session_dir / "encounters.jsonl", std::ios::app);
        ofs << json::value(m_encounter_summary).to_string() << '\n';
        m_encounter_summary.clear();
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to write encounter summary:", e.what());
    }
}

void asst::RoguelikeDataCollection::flush_boon_summary(bool force)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty() || (!force && m_boon_summary.empty())) {
        return;
    }

    try {
        std::filesystem::create_directories(m_session_dir);
        std::ofstream ofs(m_session_dir / "boons.jsonl", std::ios::app);
        ofs << json::value(m_boon_summary).to_string() << '\n';
        m_boon_summary.clear();
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to write boon summary:", e.what());
    }
}

void asst::RoguelikeDataCollection::flush_trader_summary(bool force)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty() || (!force && m_trader_summary.empty())) {
        return;
    }

    try {
        std::filesystem::create_directories(m_session_dir);
        std::ofstream ofs(m_session_dir / "traders.jsonl", std::ios::app);
        ofs << json::value(m_trader_summary).to_string() << '\n';
        m_trader_summary.clear();
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to write trader summary:", e.what());
    }
}

void asst::RoguelikeDataCollection::flush_agent_summary(bool force)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty() || (!force && m_agent_summary.empty())) {
        return;
    }

    try {
        std::filesystem::create_directories(m_session_dir);
        std::ofstream ofs(m_session_dir / "agents.jsonl", std::ios::app);
        ofs << json::value(m_agent_summary).to_string() << '\n';
        m_agent_summary.clear();
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to write agent summary:", e.what());
    }
}

void asst::RoguelikeDataCollection::flush_loot_summary(bool force)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty() || (!force && m_loot_summary.empty())) {
        return;
    }

    try {
        std::filesystem::create_directories(m_session_dir);
        std::ofstream ofs(m_session_dir / "loots.jsonl", std::ios::app);
        ofs << json::value(m_loot_summary).to_string() << '\n';
        m_loot_summary.clear();
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to write loot summary:", e.what());
    }
}

void asst::RoguelikeDataCollection::flush_stone_mountain_summary(bool force)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty() || (!force && m_stone_mountain_summary.empty())) {
        return;
    }

    try {
        std::filesystem::create_directories(m_session_dir);
        std::ofstream ofs(m_session_dir / "stone_mountains.jsonl", std::ios::app);
        ofs << json::value(m_stone_mountain_summary).to_string() << '\n';
        m_stone_mountain_summary.clear();
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to write stone mountain summary:", e.what());
    }
}

void asst::RoguelikeDataCollection::flush_taotie_corridor_summary(bool force)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty() || (!force && m_taotie_corridor_summary.empty())) {
        return;
    }

    try {
        std::filesystem::create_directories(m_session_dir);
        std::ofstream ofs(m_session_dir / "taotie_corridors.jsonl", std::ios::app);
        ofs << json::value(m_taotie_corridor_summary).to_string() << '\n';
        m_taotie_corridor_summary.clear();
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to write taotie corridor summary:", e.what());
    }
}

void asst::RoguelikeDataCollection::flush_encounter_collectible_summary(bool force)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty() || (!force && m_encounter_collectible_summary.empty())) {
        return;
    }

    try {
        std::filesystem::create_directories(m_session_dir);
        std::ofstream ofs(m_session_dir / "encounter_collectibles.jsonl", std::ios::app);
        ofs << json::value(m_encounter_collectible_summary).to_string() << '\n';
        m_encounter_collectible_summary.clear();
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to write encounter collectible summary:", e.what());
    }
}
