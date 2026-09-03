#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <meojson/json.hpp>

namespace cv
{
class Mat;
}

namespace asst::blackflow
{
inline constexpr int BlackFlowRunLogSchemaVersion = 1;
inline constexpr int BlackFlowRunLogJpegQuality = 76;
inline constexpr int BlackFlowRunLogMaximumImageWidth = 1280;
inline constexpr int BlackFlowRunLogMaximumImageHeight = 720;
inline constexpr double BlackFlowRunLogStableFrameMaximumMeanDifference = 3.0;
inline constexpr double BlackFlowRunLogNearDuplicateMaximumMeanDifference = 0.15;
inline constexpr std::string_view BlackFlowNodeAttributionFileName = "attribution.txt";

// 地图节点 ID 把楼层编码在高 16 位，正常值远超 32 位 int。运行日志状态中的
// visited_nodes 必须按完整的无符号 64 位读取，否则首个已访问节点出现后会阻断
// 后续所有事件、战斗和商店证据写盘。
[[nodiscard]] inline std::optional<std::uint64_t> run_log_node_id(const json::value& value) noexcept
{
    if (!value.is_number()) {
        return std::nullopt;
    }
    try {
        return static_cast<std::uint64_t>(value.as_unsigned_long_long());
    }
    catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] inline std::vector<std::uint64_t>
    run_log_collection_nodes_to_materialize(const json::object& state) noexcept
{
    std::vector<std::uint64_t> nodes;
    if (const json::value* visited = state.find_value("visited_nodes");
        visited != nullptr && visited->is_array()) {
        for (const json::value& item : visited->as_array()) {
            if (const auto node = run_log_node_id(item); node.has_value() && *node != 0) {
                nodes.emplace_back(*node);
            }
        }
    }
    // current_node 在刚进入新层时是入口林间空地，并不表示它是一个实际走过、
    // 需要保存证据的节点。真实走过的节点由 visited_nodes 物化；截图落盘也会
    // 按需创建目标目录。
    return nodes;
}

enum class RunLogLevel
{
    Trace,
    Debug,
    Info,
    Warning,
    Error,
};

[[nodiscard]] constexpr std::string_view to_string(RunLogLevel level) noexcept
{
    switch (level) {
    case RunLogLevel::Trace:
        return "TRACE";
    case RunLogLevel::Debug:
        return "DEBUG";
    case RunLogLevel::Info:
        return "INFO";
    case RunLogLevel::Warning:
        return "WARN";
    case RunLogLevel::Error:
        return "ERROR";
    }
    return "ERROR";
}

[[nodiscard]] constexpr bool is_run_log_level(std::string_view level) noexcept
{
    return level == "TRACE" || level == "DEBUG" || level == "INFO" || level == "WARN" || level == "ERROR";
}

// ProcessTask 的识别任务同样会进入事件流，但只有这些 action 会改变游戏界面，
// 因而需要同时保存操作前、操作后的画面。
[[nodiscard]] constexpr bool process_action_changes_ui(std::string_view action) noexcept
{
    return action == "BasicClick" || action == "ClickSelf" || action == "ClickRect" || action == "Swipe" ||
           action == "Input" || action == "Stop";
}

enum class RunLogImageCaptureMode
{
    Omit,
    ProvidedObservation,
    ImmediateSnapshot,
    StableSnapshot,
};

// 图像策略与事件持久化解耦，便于保证文字事件完整、图片只保留可重放的语义证据。
[[nodiscard]] constexpr RunLogImageCaptureMode run_log_image_capture_mode(
    std::string_view action,
    std::string_view phase,
    bool has_provided_image,
    bool capture_requested) noexcept
{
    const bool process_event = action.starts_with("process.");
    const bool abnormal_terminal = phase == "failed" || phase == "stopped";
    if (process_event) {
        // ProcessTask 内部会产生大量识别任务事件；它们保留文字轨迹，只在异常终止时留稳定画面。
        return abnormal_terminal && capture_requested ? RunLogImageCaptureMode::StableSnapshot
                                                      : RunLogImageCaptureMode::Omit;
    }
    if (has_provided_image) {
        return RunLogImageCaptureMode::ProvidedObservation;
    }
    if (!capture_requested) {
        return RunLogImageCaptureMode::Omit;
    }
    // started 表示操作前决策证据，必须立即抓取；操作后画面则必须等动画稳定。
    return phase == "started" ? RunLogImageCaptureMode::ImmediateSnapshot
                              : RunLogImageCaptureMode::StableSnapshot;
}

[[nodiscard]] constexpr bool run_log_frame_is_stable(double mean_difference) noexcept
{
    return mean_difference >= 0.0 && mean_difference <= BlackFlowRunLogStableFrameMaximumMeanDifference;
}

[[nodiscard]] constexpr bool should_reuse_run_log_image(
    double mean_difference,
    bool same_state,
    bool same_action) noexcept
{
    // 完全相同的像素永远可以复用；轻微噪声只在语义状态和动作都相同时才视为冗余。
    return mean_difference == 0.0 ||
           (mean_difference >= 0.0 && mean_difference <= BlackFlowRunLogNearDuplicateMaximumMeanDifference &&
            same_state && same_action);
}

struct RunLogEvent
{
    RunLogLevel level = RunLogLevel::Info;
    std::string action;
    std::string phase = "completed";
    std::string outcome = "success";
    std::string task;
    std::string transaction_id;
    json::object state;
    json::object details;
};

// 供离线重放器在真正读取图片/执行动作前做廉价的流完整性检查。
// JSONL 每行解析成一个 json::value 后按原序传入即可。
[[nodiscard]] inline bool validate_run_log_replay_stream(
    const std::vector<json::value>& events,
    std::string* error = nullptr)
{
    std::int64_t expected_sequence = 1;
    std::int64_t previous_elapsed_ms = -1;
    for (const json::value& event : events) {
        if (!event.is_object()) {
            if (error != nullptr) {
                *error = "run log event is not an object";
            }
            return false;
        }
        if (event.get("schema_version", 0) != BlackFlowRunLogSchemaVersion) {
            if (error != nullptr) {
                *error = "run log schema version is unsupported";
            }
            return false;
        }
        const std::int64_t sequence = event.get("sequence", std::int64_t { -1 });
        if (sequence != expected_sequence) {
            if (error != nullptr) {
                *error = "run log sequence is not continuous at " + std::to_string(expected_sequence);
            }
            return false;
        }
        const std::int64_t elapsed_ms = event.get("elapsed_ms", std::int64_t { -1 });
        if (elapsed_ms < previous_elapsed_ms) {
            if (error != nullptr) {
                *error = "run log monotonic elapsed time moved backwards";
            }
            return false;
        }
        if (event.get("timestamp", std::string()).empty() ||
            !is_run_log_level(event.get("level", std::string())) ||
            event.get("action", std::string()).empty()) {
            if (error != nullptr) {
                *error = "run log event is missing timestamp, level, or action";
            }
            return false;
        }
        const std::string image_path = event.get("image", "path", std::string());
        if (!image_path.empty()) {
            const std::filesystem::path path(image_path);
            std::string extension = path.extension().string();
            for (char& character : extension) {
                if (character >= 'A' && character <= 'Z') {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }
            if ((extension != ".jpg" && extension != ".jpeg") || path.is_absolute() ||
                image_path.find("..") != std::string::npos) {
                if (error != nullptr) {
                    *error = "run log image reference is not a safe relative JPEG path";
                }
                return false;
            }
        }
        ++expected_sequence;
        previous_elapsed_ms = elapsed_ms;
    }
    return true;
}

class BlackFlowRunLog
{
public:
    bool prepare(
        const std::filesystem::path& root_directory,
        std::uint64_t run_revision,
        std::string* error = nullptr);
    bool record(
        const std::filesystem::path& root_directory,
        std::uint64_t run_revision,
        const RunLogEvent& event,
        const cv::Mat* image,
        std::string* error = nullptr);
    bool append_node_attribution(
        const std::filesystem::path& root_directory,
        std::uint64_t run_revision,
        const std::filesystem::path& relative_directory,
        std::string_view attribution,
        std::string* error = nullptr);
    [[nodiscard]] std::filesystem::path close_current_run() noexcept;
    void reset() noexcept;

    [[nodiscard]] const std::filesystem::path& run_directory() const noexcept { return m_run_directory; }
    [[nodiscard]] std::uint64_t sequence() const noexcept { return m_sequence; }

private:
    bool ensure_started(
        const std::filesystem::path& root_directory,
        std::uint64_t run_revision,
        std::string* error);
    bool write_image(
        const cv::Mat& image,
        const std::string& stem,
        const std::filesystem::path& relative_directory,
        bool preserve_full_height,
        json::object& image_details,
        std::string* error) const;
    bool ensure_node_attribution_file(
        const std::filesystem::path& relative_directory,
        std::string* error) const;
    bool ensure_collection_node_directories(const json::object& state, std::string* error) const;

    mutable std::mutex m_mutex;
    std::filesystem::path m_run_directory;
    std::ofstream m_jsonl;
    std::ofstream m_text;
    std::ofstream m_replay_data;
    std::chrono::steady_clock::time_point m_monotonic_start;
    std::shared_ptr<cv::Mat> m_last_image;
    json::object m_last_image_details;
    std::string m_last_image_state;
    std::string m_last_image_action;
    std::uint64_t m_last_image_sequence = 0;
    std::uint64_t m_run_revision = 0;
    std::uint64_t m_sequence = 0;
};
} // namespace asst::blackflow
