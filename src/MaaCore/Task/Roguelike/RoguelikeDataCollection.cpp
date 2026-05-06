#include "RoguelikeDataCollection.h"

#include <fstream>

#include "MaaUtils/ImageIo.h"
#include "MaaUtils/Time.hpp"
#include "Task/Roguelike/RoguelikeConfig.h"
#include "Utils/Logger.hpp"
#include "Utils/Platform.hpp"
#include "Utils/WorkingDir.hpp"

void asst::RoguelikeDataCollection::start_session(const RoguelikeConfig& config, const json::value& params)
{
    std::lock_guard lock(m_mutex);

    m_session_dir = UserDir.get() / "debug" / "roguelike" / "data_collection" /
                    utils::path(MAA_NS::format_now_for_filename());
    std::filesystem::create_directories(m_session_dir / "images");
    m_current_floor = "未知层";
    m_floor_index = 0;
    m_encounter_summary.clear();
    m_record_map_encounters = false;

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
    disable();
}

void asst::RoguelikeDataCollection::finish_run(std::string_view reason)
{
    log_event("run_end", json::object { { "reason", std::string(reason) } });
    flush_encounter_summary(true);

    std::lock_guard lock(m_mutex);
    m_current_floor = "未知层";
    m_floor_index = 0;
    m_record_map_encounters = false;
}

void asst::RoguelikeDataCollection::finish_run_if_has_cached_encounters(std::string_view reason)
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_enabled || m_session_dir.empty() || m_encounter_summary.empty()) {
            return;
        }
    }

    log_event("run_end", json::object { { "reason", std::string(reason) } });
    flush_encounter_summary(false);

    std::lock_guard lock(m_mutex);
    m_current_floor = "未知层";
    m_floor_index = 0;
    m_record_map_encounters = false;
}

void asst::RoguelikeDataCollection::disable()
{
    flush_encounter_summary();

    std::lock_guard lock(m_mutex);
    m_enabled = false;
    m_session_dir.clear();
    m_current_floor.clear();
    m_floor_index = 0;
    m_encounter_summary.clear();
    m_record_map_encounters = false;
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
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty() || image.empty()) {
        return {};
    }

    try {
        const auto filename =
            utils::path(std::format("{}_{}.png", MAA_NS::format_now_for_filename(), std::string(suffix)));
        const auto path = m_session_dir / "images" / filename;
        if (MAA_NS::imwrite(path, image)) {
            return utils::path_to_utf8_string(path);
        }
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to save data collection image:", e.what());
    }

    return {};
}

void asst::RoguelikeDataCollection::note_strategy_change(std::string_view strategy)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty()) {
        return;
    }

    if (strategy == "_dataCollection" || strategy == "_exit") {
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
        m_current_floor = "是非境";
    }
}

void asst::RoguelikeDataCollection::set_record_map_encounters(bool enabled)
{
    std::lock_guard lock(m_mutex);
    m_record_map_encounters = enabled;
}

bool asst::RoguelikeDataCollection::should_record_map_encounters() const
{
    std::lock_guard lock(m_mutex);
    return m_enabled && !m_session_dir.empty() && m_record_map_encounters;
}

void asst::RoguelikeDataCollection::record_encounter(std::string_view name, std::string_view image_path)
{
    std::lock_guard lock(m_mutex);
    if (!m_enabled || m_session_dir.empty() || !m_record_map_encounters) {
        return;
    }

    const std::string floor = m_current_floor.empty() ? "未知层" : m_current_floor;
    if (!m_encounter_summary.contains(floor)) {
        m_encounter_summary[floor] = json::array {};
    }

    m_encounter_summary[floor].as_array().emplace_back(json::object {
        { "type", "Encounter" },
        { "name", std::string(name) },
        { "image", std::string(image_path) },
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
