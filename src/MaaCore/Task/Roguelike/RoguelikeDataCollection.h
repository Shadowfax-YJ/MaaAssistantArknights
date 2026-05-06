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
    void finish_run_if_has_cached_encounters(std::string_view reason = "cached_encounters");
    void disable();

    [[nodiscard]] bool enabled() const;
    [[nodiscard]] const std::filesystem::path& session_dir() const;

    void log_event(std::string_view type, json::object details = {});
    std::string save_image(const cv::Mat& image, std::string_view suffix);
    void note_strategy_change(std::string_view strategy);
    void set_record_map_encounters(bool enabled);
    [[nodiscard]] bool should_record_map_encounters() const;
    void record_encounter(std::string_view name, std::string_view image_path);

private:
    void flush_encounter_summary(bool force = false);

    mutable std::mutex m_mutex;
    bool m_enabled = false;
    std::filesystem::path m_session_dir;
    std::string m_current_floor;
    int m_floor_index = 0;
    json::object m_encounter_summary;
    bool m_record_map_encounters = false;
};

inline static auto& RoguelikeDataCollector = RoguelikeDataCollection::get_instance();
} // namespace asst
