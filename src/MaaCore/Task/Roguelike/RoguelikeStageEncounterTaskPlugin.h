#pragma once
#include <functional>

#include "AbstractRoguelikeTaskPlugin.h"
#include "Config/Roguelike/RoguelikeStageEncounterConfig.h"
#include "Task/Roguelike/BlackFlow/BlackFlowEncounterRules.h"
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

    void set_event_observer(std::function<void(std::string_view)> observer)
    {
        m_event_observer = std::move(observer);
    }

    void set_event_capture_observer(std::function<void(std::string_view, const cv::Mat&)> observer)
    {
        m_event_capture_observer = std::move(observer);
    }

    void set_blackflow_encounter_context_provider(
        std::function<std::optional<blackflow::LakeFairyContext>()> provider)
    {
        m_blackflow_encounter_context_provider = std::move(provider);
    }

    void set_blackflow_encounter_choice_provider(
        std::function<std::optional<std::size_t>(std::string_view)> provider)
    {
        m_blackflow_encounter_choice_provider = std::move(provider);
    }

    void set_blackflow_encounter_choice_order_provider(
        std::function<std::optional<std::vector<std::string>>(std::string_view)> provider)
    {
        m_blackflow_encounter_choice_order_provider = std::move(provider);
    }

    virtual bool verify(AsstMsg msg, const json::value& details) const override;

protected:
    virtual bool _run() override;

    std::optional<std::string> handle_single_event(const std::string& event_name);
    std::optional<std::string> handle_blackflow_lake_fairy(const Config::RoguelikeEvent& event);
    static bool satisfies_condition(const Config::ChoiceRequire& requirement, int special_val);
    static size_t process_task(const Config::RoguelikeEvent& event, int special_val);
    int hp(const cv::Mat& image) const;

private:
    bool update_option_list(std::string_view event_name);
    bool select_analyzed_option(size_t index);
    void reset_option_list_and_view_data();
    void report_analyzed_options();
    void update_view(const cv::Mat& image = cv::Mat());
    void reset_view();
    void move_to_analyzed_option(size_t index);
    void move_to_option_list_head();
    void move_forward();
    void move_backward();

    std::optional<std::string> next_event(const std::string& next_event_name);

    static bool save_img(const cv::Mat& image, std::string_view description = "image");

    OptionAnalyzer::Result m_option_list;
    size_t m_view_begin = 0;
    size_t m_view_end = 0;
    std::vector<int> m_option_y_in_view;
    std::vector<Rect> m_option_rect_in_view;
    std::function<void(std::string_view)> m_event_observer;
    std::function<void(std::string_view, const cv::Mat&)> m_event_capture_observer;
    std::function<std::optional<blackflow::LakeFairyContext>()> m_blackflow_encounter_context_provider;
    std::function<std::optional<std::size_t>(std::string_view)> m_blackflow_encounter_choice_provider;
    std::function<std::optional<std::vector<std::string>>(std::string_view)>
        m_blackflow_encounter_choice_order_provider;
    std::optional<blackflow::LakeFairyChoicePlan> m_lake_fairy_plan;
    size_t m_lake_fairy_initial_choice_index = 0;
    bool m_lake_fairy_unique_choice_selected = false;

    static constexpr size_t MAX_SWIPE_TIMES = 1;
    static constexpr size_t BLACKFLOW_MAX_SWIPE_TIMES = 8;

    static constexpr int UNDEFINED = -1;
};
}
