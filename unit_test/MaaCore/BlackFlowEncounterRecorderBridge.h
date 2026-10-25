#pragma once

#include <filesystem>
#include <memory>
#include <meojson/json.hpp>
#include <opencv2/core.hpp>
#include <string>
#include <utility>
#include <vector>

// Separate TU: the controller replay cannot substitute the production recorder's
// Session, identity validation, TaskPort, or logger with its permissive I/O stubs.
class BlackFlowEncounterRecorderBridge
{
public:
    static void load(const std::filesystem::path& repo);
    static std::vector<std::pair<std::string, std::string>> configured_links();
    BlackFlowEncounterRecorderBridge(const std::filesystem::path& output, const std::string& root);
    ~BlackFlowEncounterRecorderBridge();
    std::pair<std::uint64_t, std::uint64_t> revisions() const;
    bool record(std::string_view name, std::string_view phase, json::object details, const cv::Mat& image);
    json::array events() const;
    std::string error() const;
    bool terminated() const;
    void complete_page();
    void change_node();
    void fail_next_capture();
    void fail_next_save();

private:
    struct State;
    std::unique_ptr<State> m_state;
};
