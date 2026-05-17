#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#include "MaaUtils/NoWarningCVMat.hpp"
#include "MaaUtils/SingletonHolder.hpp"
#include "meojson/json.hpp"

namespace asst
{
class RoguelikeConfig;

class RoguelikeDataCollection final : public MAA_NS::SingletonHolder<RoguelikeDataCollection>
{
public:
    void start_session(const RoguelikeConfig& config, const json::value& params);
    void stop_session(std::string_view reason = "stopped");
    void finish_run(std::string_view reason = "run_finished");
    void finish_run_if_active(std::string_view reason = "run_finished");
    void finish_run_if_has_cached_encounters(std::string_view reason = "cached_encounters");
    void start_run_if_enabled();
    void set_pending_abandon_reason(std::string_view reason, json::object details = {});
    void finish_run_as_abandoned(
        std::string_view default_reason,
        const cv::Mat& image,
        json::object details = {});
    void disable();

    [[nodiscard]] bool enabled() const;
    [[nodiscard]] const std::filesystem::path& session_dir() const;

    void log_event(std::string_view type, json::object details = {});
    std::string save_image(const cv::Mat& image, std::string_view suffix);
    std::string save_encounter_image(const cv::Mat& image);
    std::string save_legend_image(const cv::Mat& image);
    std::string save_trader_image(const cv::Mat& image);
    std::string save_yi_trader_image(const cv::Mat& image);
    std::string save_agent_image(const cv::Mat& image);
    std::string save_agent_treasure_image(const cv::Mat& image);
    std::string save_agent_collectible_image(const cv::Mat& image);
    std::string save_loot_image(const cv::Mat& image, std::string_view suffix);
    std::string save_stone_mountain_image(const cv::Mat& image);
    std::string save_taotie_corridor_image(const cv::Mat& image, std::string_view suffix);
    std::string save_encounter_collectible_image(const cv::Mat& image, std::string_view suffix);
    [[nodiscard]] static std::string normalize_jiegarden_floor_name(std::string_view ocr_text);
    void note_floor_ocr(std::string_view ocr_text);
    void note_strategy_change(std::string_view strategy);
    void note_selected_node_type(std::string_view node_type);
    void note_agent_source(std::string_view event_name, std::string_view record_type);
    [[nodiscard]] bool current_floor_is_bosky_passage() const;
    void set_record_map_encounters(bool enabled, std::string_view type = "Encounter");
    [[nodiscard]] bool should_record_map_encounters() const;
    [[nodiscard]] std::string map_encounter_type() const;
    void record_encounter(std::string_view name, std::string_view image_path, std::string_view type = "Encounter");
    void record_trader(std::string_view name, std::string_view image_path, bool is_yi_trader);
    json::object record_agent(std::string_view name, std::string_view image_path, json::object extra_details = {});
    json::object record_loot(std::string_view type, std::string_view image_path);
    void record_stone_mountain(
        std::string_view image_path,
        size_t selected_choice,
        std::string_view selected_option);
    void record_taotie_corridor(
        std::string_view image_path,
        size_t selected_choice,
        std::string_view selected_option,
        std::string_view next_event_image_path,
        size_t next_event_selected_choice,
        std::string_view next_event_selected_option);
    void record_encounter_collectible(
        std::string_view event_name,
        std::string_view image_path,
        size_t popup_index);

private:
    std::string save_image(const cv::Mat& image, std::string_view suffix, std::string_view category_dir);
    bool link_to_images_dir(const std::filesystem::path& target, const std::filesystem::path& filename);
    [[nodiscard]] json::object build_agent_source_details_locked() const;
    void flush_encounter_summary(bool force = false);
    void flush_trader_summary(bool force = false);
    void flush_agent_summary(bool force = false);
    void flush_loot_summary(bool force = false);
    void flush_stone_mountain_summary(bool force = false);
    void flush_taotie_corridor_summary(bool force = false);
    void flush_encounter_collectible_summary(bool force = false);

    mutable std::mutex m_mutex;
    bool m_enabled = false;
    std::filesystem::path m_session_dir;
    std::string m_current_floor;
    int m_floor_index = 0;
    bool m_run_active = false;
    std::string m_pending_abandon_reason;
    json::object m_pending_abandon_details;
    json::object m_encounter_summary;
    json::object m_trader_summary;
    json::object m_agent_summary;
    json::object m_loot_summary;
    json::object m_stone_mountain_summary;
    json::object m_taotie_corridor_summary;
    json::object m_encounter_collectible_summary;
    bool m_record_map_encounters = false;
    std::string m_map_encounter_type = "Encounter";
    std::string m_last_selected_node_type = "Unknown";
    std::string m_pending_agent_source_node_type = "Unknown";
    std::string m_pending_agent_source_event_name;
    std::string m_pending_agent_source_record_type = "Encounter";
};

inline static auto& RoguelikeDataCollector = RoguelikeDataCollection::get_instance();
} // namespace asst
