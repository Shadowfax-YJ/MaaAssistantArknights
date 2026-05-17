#pragma once
#include "AbstractRoguelikeTaskPlugin.h"
#include "Config/Roguelike/RoguelikeStageEncounterConfig.h"
#include "Vision/Roguelike/RoguelikeEncounterOptionAnalyzer.h"

namespace asst
{
class RoguelikeStageEncounterTaskPlugin : public AbstractRoguelikeTaskPlugin
{
public:
    using Config = RoguelikeStageEncounterConfig;
    using OptionAnalyzer = RoguelikeEncounterOptionAnalyzer;

    using AbstractRoguelikeTaskPlugin::AbstractRoguelikeTaskPlugin;
    virtual ~RoguelikeStageEncounterTaskPlugin() override = default;

    virtual bool verify(AsstMsg msg, const json::value& details) const override;
    virtual void reset_in_run_variables() override;

protected:
    virtual bool _run() override;

    std::optional<std::string> handle_single_event(const std::string& event_name);
    static bool satisfies_condition(const Config::ChoiceRequire& requirement, int special_val);
    static size_t process_task(const Config::RoguelikeEvent& event, int special_val);
    int hp(const cv::Mat& image) const;

private:
    enum class RunAction
    {
        HandleEncounter,
        CaptureEventCollectiblePopup,
    };

    bool update_option_list();
    std::optional<std::string> handle_jiegarden_agent_event(
        const Config::RoguelikeEvent& event,
        const Config::RoguelikeEvent* treasure_event);
    std::string capture_and_handle_jiegarden_agent_treasure(const Config::RoguelikeEvent& event);
    bool select_analyzed_option(size_t index, bool accept_battle_detail = false);
    bool jiegarden_guidance_battle_detail_visible() const;
    void reset_option_list_and_view_data();
    void report_analyzed_options();
    void update_view(const cv::Mat& image = cv::Mat());
    void reset_view();
    void record_agent_event_if_needed(const Config::RoguelikeEvent& event);
    void record_stone_mountain_event_if_needed(const Config::RoguelikeEvent& event);
    void record_taotie_corridor_next_event_if_needed(const Config::RoguelikeEvent& event);
    void mark_jiegarden_candle_chapel_battle_if_needed(const Config::RoguelikeEvent& event);
    bool close_jiegarden_meet_collectible_draw_copper_result();
    bool arm_jiegarden_event_collectible_popups(const Config::RoguelikeEvent& event);
    bool capture_pending_jiegarden_event_collectible_popup();
    void record_agent_event(
        std::string_view agent_name,
        const cv::Mat& agent_image,
        json::object extra_details = {});
    void move_to_analyzed_option(size_t index);
    void move_to_option_list_head();
    void move_forward();
    void move_backward();

    std::optional<std::string> next_event(const Config::RoguelikeEvent& event);
    bool advance_to_next_event(std::string_view next_event_name);

    static bool save_img(const cv::Mat& image, std::string_view description = "image");

    OptionAnalyzer::Result m_option_list;
    cv::Mat m_option_list_image;
    size_t m_pending_stone_mountain_choice = 0;
    std::string m_pending_stone_mountain_option_text;
    std::string m_pending_taotie_corridor_initial_image;
    size_t m_pending_taotie_corridor_choice = 0;
    std::string m_pending_taotie_corridor_option_text;
    mutable RunAction m_run_action = RunAction::HandleEncounter;
    std::string m_pending_event_collectible_name;
    size_t m_pending_event_collectible_expected = 0;
    size_t m_pending_event_collectible_captured = 0;
    size_t m_view_begin = 0;
    size_t m_view_end = 0;
    std::vector<int> m_option_y_in_view;

    static constexpr size_t MAX_SWIPE_TIMES = 1;

    static constexpr int UNDEFINED = -1;
};
}
