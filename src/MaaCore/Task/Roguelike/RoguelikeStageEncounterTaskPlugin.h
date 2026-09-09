#pragma once
#include <functional>

#include "AbstractRoguelikeTaskPlugin.h"
#include "Config/Roguelike/RoguelikeStageEncounterConfig.h"
#include "Task/Roguelike/BlackFlow/BlackFlowEncounterRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowExpeditionRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowSacrificeRules.h"
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

    void
        set_blackflow_expedition_context_provider(std::function<std::optional<blackflow::ExpeditionContext>()> provider)
    {
        m_expedition_context_provider = std::move(provider);
    }

    void set_blackflow_expedition_dispatch_observer(std::function<void(std::string_view)> observer)
    {
        m_expedition_dispatch_observer = std::move(observer);
    }

    virtual void reset_in_run_variables() override;

    void set_blackflow_sacrifice_context_provider(std::function<std::optional<blackflow::SacrificeContext>()> provider)
    {
        m_sacrifice_context_provider = std::move(provider);
    }

    void set_event_detail_observer(
        std::function<void(std::string_view, std::string_view, json::object, const cv::Mat&)> observer)
    {
        m_event_detail_observer = std::move(observer);
    }

    void set_blackflow_portal_handlers(
        std::function<std::optional<std::size_t>(const std::vector<std::string>&)> choice,
        std::function<void(std::string)> selected)
    {
        m_portal_choice = std::move(choice);
        m_portal_selected = std::move(selected);
    }

protected:
    virtual bool _run() override;

    std::optional<std::string> handle_single_event(const std::string& event_name);
    std::optional<std::string> handle_blackflow_lake_fairy(const Config::RoguelikeEvent& event);
    static bool satisfies_condition(const Config::ChoiceRequire& requirement, int special_val);
    static size_t process_task(const Config::RoguelikeEvent& event, int special_val);
    int hp(const cv::Mat& image) const;

private:
    bool refresh_sacrifice_context();
    bool handle_sacrifice_event();
    bool handle_sacrifice_picker();
    bool finish_sacrifice_civilization();
    std::optional<std::vector<std::string>> scan_sacrifice_natural_items(std::string_view phase);
    void report_sacrifice(std::string_view phase, json::object details, const cv::Mat& image);
    void refresh_expedition_context();
    bool wait_for_secondary_event(std::string_view picker_task);
    bool handle_expedition_picker();
    void confirm_expedition_dispatch();
    bool update_option_list(std::string_view event_name);
    bool select_analyzed_option(size_t index);
    std::optional<Rect> wait_for_analyzed_option_stable(size_t index);
    void reset_option_list_and_view_data();
    void report_analyzed_options();
    void update_view(const cv::Mat& image = cv::Mat());
    void reset_view();
    bool move_to_analyzed_option(size_t index);
    void move_to_option_list_head();
    void move_forward();
    void move_backward();

    std::optional<std::string> next_event(const std::string& next_event_name);

    static bool save_img(const cv::Mat& image, std::string_view description = "image");

    std::function<std::optional<std::size_t>(const std::vector<std::string>&)> m_portal_choice;
    std::function<void(std::string)> m_portal_selected;
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
    std::function<std::optional<blackflow::ExpeditionContext>()> m_expedition_context_provider;
    std::function<void(std::string_view)> m_expedition_dispatch_observer;
    std::optional<blackflow::ExpeditionContext> m_expedition_context;
    std::string m_expedition_pending_operator;
    bool m_expedition_finished = false;
    std::optional<size_t> m_expedition_initial_choice;

    std::function<std::optional<blackflow::SacrificeContext>()> m_sacrifice_context_provider;
    std::function<void(std::string_view, std::string_view, json::object, const cv::Mat&)> m_event_detail_observer;

    struct SacrificeState
    {
        std::optional<blackflow::SacrificeContext> context;
        blackflow::SacrificePhase phase = blackflow::SacrificePhase::Initial;
        int exchanges = 0;
        std::optional<size_t> initial_choice;
        std::optional<Rect> selected_card;
        std::string selected_name;
        std::optional<std::vector<std::string>> naturals_before;
        bool civilization_inventory_scanned = false;
        bool civilization_transition_seen = false;
    } m_sacrifice;

    static constexpr size_t MAX_SWIPE_TIMES = 1;
    static constexpr size_t BLACKFLOW_MAX_SWIPE_TIMES = 8;

    static constexpr int UNDEFINED = -1;
};
}
