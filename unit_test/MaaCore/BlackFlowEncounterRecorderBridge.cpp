#include "BlackFlowEncounterRecorderBridge.h"

#include "Config/ResourceLoader.h"
#include "Config/Roguelike/RoguelikeStageEncounterConfig.h"
#include "Task/Roguelike/BlackFlow/BlackFlowEncounterEvidence.h"
#include "Task/Roguelike/BlackFlow/BlackFlowExpeditionEvidence.h"
#include "Task/Roguelike/BlackFlow/BlackFlowMapObservationSource.h"
#include "Utils/WorkingDir.hpp"
#include <fstream>
#include <stdexcept>

using namespace asst;
using namespace asst::blackflow;

namespace
{
void check(bool value, const std::string& error)
{
    if (!value) {
        throw std::runtime_error(error);
    }
}

}

struct BlackFlowEncounterRecorderBridge::State
{
    std::filesystem::path output, previous;
    std::shared_ptr<BlackFlowSession> session = std::make_shared<BlackFlowSession>();
    BlackFlowTaskPort port { { }, nullptr, "Roguelike", std::make_shared<BlackFlowMapObservationSource>() };
    std::string last_error;
    bool fail_capture = false;
    bool fail_save = false;
    int floor = 1;
    BlackFlowPerceptionSnapshot frame;

    void move_to(int from, int to)
    {
        MoveCandidate move;
        move.action_id = "multistage-replay-move";
        move.source = *make_stable_node_id(floor, { 0, from });
        move.target = move.landing = *make_stable_node_id(floor, { 0, to });
        move.path = move.possible_landings = { move.target };
        move.landing_action_point_gains.emplace(move.target, 0);
        move.movement = MovementKind::Walk;
        move.predicted_action_point_cost = 1;
        std::string error;
        check(session->begin_transaction(move, &error), error);
        check(
            session->accept_preview({ PreviewReachability::Reachable, 1 }, &error) == PreviewDisposition::ReadyToCommit,
            error);
        check(session->commit({ }, &error), error);
        check(session->mark_page_running(&error), error);
    }
};

void BlackFlowEncounterRecorderBridge::load(const std::filesystem::path& repo)
{
    check(ResourceLoader::get_instance().load(repo / "resource"), "production resources");
}

std::vector<std::pair<std::string, std::string>> BlackFlowEncounterRecorderBridge::configured_links()
{
    std::vector<std::pair<std::string, std::string>> links;
    const auto& events = RoguelikeStageEncounter.get_events(
        std::string(RoguelikeTheme::BlackFlow),
        RoguelikeMode::BlackFlowAutomationCollection);
    for (const auto& [name, event] : events) {
        if (!event.next_event.empty()) {
            links.emplace_back(name, event.next_event);
        }
        for (const auto& group : event.choice_groups) {
            if (group.next_event && !group.next_event->empty()) {
                links.emplace_back(name, *group.next_event);
            }
        }
    }
    std::ranges::sort(links);
    links.erase(std::unique(links.begin(), links.end()), links.end());
    return links;
}

BlackFlowEncounterRecorderBridge::BlackFlowEncounterRecorderBridge(
    const std::filesystem::path& output,
    const std::string& root,
    int floor) :
    m_state(std::make_unique<State>())
{
    auto& state = *m_state;
    state.floor = floor;
    state.output = output;
    state.previous = UserDir.empty() ? std::filesystem::current_path() : UserDir.get();
    std::filesystem::create_directories(output);
    check(UserDir.set(output), "replay UserDir");
    check(state.session->initialize("automation_collection") && state.session->set_current_floor(floor), "initialize");
    auto& frame = state.frame;
    auto& observation = frame.observation;
    observation.observation_id = "multi-stage-map";
    observation.sequence = observation.viewport_revision = 1;
    observation.floor = floor;
    observation.floor_from_ocr = observation.recognition_ok = observation.graph_connected = true;
    observation.current_marker_temporary_id = 0;
    observation.current_marker_score = 1;
    observation.hud_action_points = frame.run.action_points = 7;
    frame.run.active_movement = MovementKind::Walk;
    for (int i = 0; i < 2; ++i) {
        PerceptionNodeObservation node;
        node.temporary_id = i;
        node.position = { 0, i };
        node.exists = node.detected_by_vision = true;
        node.confidence = 1;
        node.type = "incident";
        node.icon_rect = { 400 + 120 * i, 200, 60, 60 };
        observation.nodes.push_back(node);
    }
    observation.edges.push_back({ 0, 1, true, true, false, 1, "visual" });
    check(state.session->update(frame), "map");
    state.move_to(0, 1);
    check(state.session->observe_page_content(root, "RoguelikeEvent"), "root title");
    state.port.set_collection_popup_session(state.session);
}

BlackFlowEncounterRecorderBridge::~BlackFlowEncounterRecorderBridge()
{
    UserDir.set(m_state->previous);
}

std::pair<std::uint64_t, std::uint64_t> BlackFlowEncounterRecorderBridge::revisions() const
{
    return { m_state->session->run_revision(), m_state->session->page_context()->page_revision };
}

bool BlackFlowEncounterRecorderBridge::record(
    std::string_view name,
    std::string_view phase,
    json::object details,
    const cv::Mat& image)
{
    const auto save = [&] {
        return record_encounter_evidence(
            *m_state->session,
            m_state->port,
            name,
            phase,
            std::move(details),
            std::exchange(m_state->fail_capture, false) ? cv::Mat { } : image,
            &m_state->last_error);
    };
    if (!std::exchange(m_state->fail_save, false)) {
        return save();
    }
    // A real filesystem error in this fresh replay run, not a substitute success
    // result from a mocked logger. Preserve and restore its earlier evidence.
    const auto logs = m_state->output / "debug/BlackFlow";
    for (const auto& entry : std::filesystem::directory_iterator(logs)) {
        const auto node = entry.path() / "collection-popups" / ("floor-" + std::to_string(m_state->floor)) /
                          ("node-" + std::to_string(m_state->session->page_context()->node));
        if (!std::filesystem::is_directory(node)) {
            continue;
        }
        const std::filesystem::path preserved = node.string() + ".preserved";
        std::filesystem::rename(node, preserved);
        std::ofstream(node).close();
        bool result = false;
        try {
            result = save();
        }
        catch (...) {
            std::filesystem::remove(node);
            std::filesystem::rename(preserved, node);
            throw;
        }
        std::filesystem::remove(node);
        std::filesystem::rename(preserved, node);
        return result;
    }
    throw std::runtime_error("no replay evidence directory to block");
}

bool BlackFlowEncounterRecorderBridge::record_expedition(
    std::string_view phase, json::object details, const cv::Mat& image)
{
    return record_expedition_evidence(
        *m_state->session, m_state->port, phase, std::move(details), image, &m_state->last_error);
}

json::array BlackFlowEncounterRecorderBridge::events() const
{
    json::array result;
    const auto root = m_state->output / "debug/BlackFlow";
    if (!std::filesystem::exists(root)) {
        return result;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory()) {
            continue;
        }
        std::ifstream stream(entry.path() / "run-events.jsonl");
        std::string line;
        while (std::getline(stream, line)) {
            result.emplace_back(json::parse(line).value());
        }
    }
    return result;
}

std::string BlackFlowEncounterRecorderBridge::error() const
{
    return m_state->last_error;
}

bool BlackFlowEncounterRecorderBridge::terminated() const
{
    return m_state->session->terminated();
}

void BlackFlowEncounterRecorderBridge::complete_page()
{
    NodeTaskResult result;
    result.kind = NodeTaskResultKind::PageCompleted;
    check(m_state->session->apply_node_task_result(result, json::object { }), "complete page");
}

void BlackFlowEncounterRecorderBridge::change_node(const std::string& name)
{
    complete_page();
    m_state->session->cancel_transaction();
    auto& observation = m_state->frame.observation;
    observation.observation_id = "multi-stage-next-node-map";
    observation.sequence = observation.viewport_revision = 2;
    observation.current_marker_temporary_id = 1;
    if (name == ExpeditionEventName) {
        observation.nodes.front().type = "expedition";
    }
    observation.hud_action_points = m_state->frame.run.action_points = 6;
    check(m_state->session->update(m_state->frame), "next node map");
    m_state->move_to(1, 0);
    check(m_state->session->observe_page_content(name, "RoguelikeEvent"), "new node title");
}

void BlackFlowEncounterRecorderBridge::fail_next_capture()
{
    m_state->fail_capture = true;
}

void BlackFlowEncounterRecorderBridge::fail_next_save()
{
    m_state->fail_save = true;
}
