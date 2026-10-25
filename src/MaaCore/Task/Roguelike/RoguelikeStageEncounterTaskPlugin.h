#pragma once

#include <map>
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

    void set_event_observer(std::function<void(std::string_view)> observer) { m_event_observer = std::move(observer); }

    void set_event_capture_observer(std::function<void(std::string_view, const cv::Mat&)> observer)
    {
        m_event_capture_observer = std::move(observer);
    }

    void set_blackflow_encounter_context_provider(std::function<std::optional<blackflow::LakeFairyContext>()> provider)
    {
        m_blackflow_encounter_context_provider = std::move(provider);
    }

    void set_blackflow_encounter_choice_provider(std::function<std::optional<std::size_t>(std::string_view)> provider)
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

    void set_blackflow_expedition_evidence_observer(
        std::function<bool(std::string_view, json::object, const cv::Mat&)> observer)
    {
        m_expedition_evidence_observer = std::move(observer);
    }

    virtual void reset_in_run_variables() override;

    void set_blackflow_sacrifice_context_provider(std::function<std::optional<blackflow::SacrificeContext>()> provider)
    {
        m_sacrifice_context_provider = std::move(provider);
    }

    void set_event_detail_observer(
        std::function<bool(std::string_view, std::string_view, json::object, const cv::Mat&)> observer)
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
    blackflow::SacrificeInventoryScanResult scan_sacrifice_natural_items(std::string_view phase);
    bool report_sacrifice(std::string_view phase, json::object details, const cv::Mat& image);
    bool prepare_encounter_choice(std::string_view event_name, int round, const cv::Mat& image);
    bool plan_encounter_choice(size_t index, std::string_view policy);
    bool randomize_encounter_choice_order(std::vector<size_t>& order);
    bool report_encounter_choice(std::string_view phase, json::object details = { }, const cv::Mat& image = { });
    bool confirm_encounter_choice(const cv::Mat& image, std::string_view basis);
    bool observe_encounter_choice_result(const cv::Mat& image);
    bool confirm_cargo_choice();
    void refresh_expedition_context();
    bool capture_expedition_candidates(const cv::Mat& image);
    bool handle_expedition_choice();
    bool report_expedition(std::string_view phase, json::object details = { }, const cv::Mat& image = { });
    bool abort_expedition(std::string_view reason);
    bool wait_for_secondary_event(std::string_view picker_task);
    bool handle_expedition_picker();
    bool confirm_expedition_dispatch(const cv::Mat& image, std::string_view basis);
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
    cv::Mat m_option_list_image;
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
    std::function<bool(std::string_view, json::object, const cv::Mat&)> m_expedition_evidence_observer;

    struct ExpeditionEvidence
    {
        json::object details;
        int sequence = 0;
        int choice_attempt = 0;
        int dispatch_attempt = 0;
        bool candidates_saved = false;
        bool choice_clicked = false;
        bool choice_confirmed = false;
        bool failed = false;
        cv::Mat confirmation_image;
        std::string confirmation_basis;
    } m_expedition_evidence;

    std::function<std::optional<blackflow::SacrificeContext>()> m_sacrifice_context_provider;
    std::function<bool(std::string_view, std::string_view, json::object, const cv::Mat&)> m_event_detail_observer;

    struct EncounterChoiceEvidence
    {
        std::optional<blackflow::SacrificeContext> context;
        std::string event_name;
        int round = 0;
        int step = 0;
        bool generic = false;
        int sequence = 0;
        int attempt = 0;
        bool failed = false;
        bool clicked = false;
        bool confirmed = false;
        bool capture_clicks = false;
        std::optional<size_t> selected;
        std::vector<size_t> order;
        std::map<std::vector<size_t>, std::vector<size_t>> group_orders;
        json::object details;
    } m_encounter_choice;

    struct SacrificeState
    {
        std::optional<blackflow::SacrificeContext> context;
        blackflow::SacrificePhase phase = blackflow::SacrificePhase::Initial;
        int exchanges = 0;
        std::optional<size_t> initial_choice;
        std::optional<Rect> selected_card;
        blackflow::SacrificePickerAttempts picker_attempts;
        std::string selected_name;
        std::optional<std::vector<std::string>> naturals_before;
        bool civilization_inventory_scanned = false;
        bool civilization_transition_seen = false;
        bool evidence_failed = false;
        int evidence_sequence = 0;
    } m_sacrifice;

    static constexpr size_t MAX_SWIPE_TIMES = 1;
    static constexpr size_t BLACKFLOW_MAX_SWIPE_TIMES = 8;

    static constexpr int UNDEFINED = -1;
};
}
