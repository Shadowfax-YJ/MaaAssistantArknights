#pragma once

#include "BlackFlowEvidenceFrame.h"
#include "Vision/Matcher.h"
#include "Vision/OCRer.h"

namespace asst::blackflow
{
// Positive page evidence, rather than a notification-text blacklist: stacked
// toasts must expose the instruction, card headers and cost icons. Also reject
// known toast text during a translucent fade, when the anchors may show through.
[[nodiscard]] inline bool employ_capture_frame_is_clear(const cv::Mat& image)
{
    if (image.empty()) {
        return false;
    }
    for (const char* task : { "BlackFlow@Roguelike@EmployLeave",
                              "BlackFlow@Roguelike@EmployCaptureCardTop",
                              "BlackFlow@Roguelike@EmployCaptureCardMiddle",
                              "BlackFlow@Roguelike@EmployCaptureCardBottom",
                              "BlackFlow@Roguelike@EmployCaptureCostTop",
                              "BlackFlow@Roguelike@EmployCaptureCostMiddle" }) {
        Matcher matcher(image);
        matcher.set_task_info(task);
        if (!matcher.analyze().has_value()) {
            return false;
        }
    }
    OCRer instruction(image);
    instruction.set_task_info("BlackFlow@Roguelike@EmployCaptureInstruction");
    const auto result = instruction.analyze();
    if (!result.has_value() || result->empty()) {
        return false;
    }
    OCRer notification(image);
    notification.set_task_info("BlackFlow@Roguelike@EmployCaptureNotification");
    const auto toast = notification.analyze();
    return !toast.has_value() || toast->empty();
}

// Inject device I/O so the exact production wait policy can also be replayed.
// A failed/empty observation resets the clean streak. Never accept a static toast.
template <typename Capture, typename IsClear, typename Wait, typename Cancelled>
bool capture_unobstructed_employ_frame(
    Capture&& capture,
    IsClear&& is_clear,
    Wait&& wait,
    Cancelled&& cancelled,
    cv::Mat& image,
    int& sampled_frames)
{
    image.release();
    sampled_frames = 0;
    cv::Mat previous;
    int clear_frames = 0;
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (cancelled() || (attempt != 0 && !wait(250)) || cancelled()) {
            return false;
        }
        cv::Mat current = capture();
        ++sampled_frames;
        if (!is_clear(current)) {
            previous.release();
            clear_frames = 0;
            continue;
        }
        clear_frames = evidence_frames_match(previous, current, cv::Rect(480, 125, 760, 440)) ? clear_frames + 1 : 1;
        previous = current.clone();
        if (clear_frames >= 3 && !cancelled()) {
            image = std::move(current);
            return true;
        }
    }
    return false;
}
} // namespace asst::blackflow
