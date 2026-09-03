#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "BlackFlowTaskPort.h"
#include "Vision/Roguelike/BlackFlow/BlackFlowMapAnalyzer.h"

namespace asst::blackflow
{
class BlackFlowMapObservationSource final : public IBlackFlowMapObservationSource
{
public:
    bool prepare(std::string* error = nullptr);
    void release() noexcept;

    bool recognize(
        const cv::Mat& image,
        const BlackFlowObservationRequest& request,
        BlackFlowMapObservation& observation,
        FactStore& observed_facts,
        std::string* error) override;

    void reset_run() override;
    bool queue_current_run_archive(std::string* error = nullptr) override;
    void configure_diagnostics(const DiagnosticSettings& settings) override;
    bool persist_diagnostics(const DiagnosticArtifactRequest& request, std::string* error) override;
    bool record_run_event(
        std::uint64_t run_revision,
        const RunLogEvent& event,
        const cv::Mat* image,
        std::string* error) override;
    bool record_node_attribution(
        std::uint64_t run_revision,
        const std::filesystem::path& relative_directory,
        std::string_view attribution,
        std::string* error) override;

private:
    std::shared_ptr<const perception::BlackFlowMapAnalyzer> m_analyzer;
    std::string m_initialization_error;
    DiagnosticSettings m_diagnostics;
    perception::MapRecognitionResult m_last_result;
    std::string m_last_attempt_id;
    std::int64_t m_accumulated_screenshot_us = 0;
    std::int64_t m_accumulated_recognition_us = 0;
    int m_last_attempt_count = 0;
    int m_last_retry_count = 0;
    std::uint64_t m_sequence = 0;
    std::optional<std::uint64_t> m_topology_map_generation;
    std::int64_t m_diagnostic_run_revision = -1;
    std::uint64_t m_diagnostic_event_sequence = 0;
    std::string m_diagnostic_run_timestamp;
    std::vector<std::string> m_processing_item_history;
    bool m_routing_history_artifacts_initialized = false;
    BlackFlowRunLog m_run_log;
};
} // namespace asst::blackflow
