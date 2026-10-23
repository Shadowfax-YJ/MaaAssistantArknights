#include "BlackFlowDataCollectionTaskPlugin.h"
#include "BlackFlowSquadSwipe.hpp"

#include <exception>
#include <fstream>
#include <string>

#include "BlackFlowDataCollectionPlan.hpp"
#include "Controller/Controller.h"
#include "MaaUtils/Time.hpp"
#include "Utils/DebugImageHelper.hpp"
#include "Utils/Logger.hpp"

bool asst::BlackFlowDataCollectionTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    m_capture_target = black_flow_capture_target(msg, details);
    m_reset_group = false;
    m_squad_swipe = 0;
    m_failure_details = json::value();
    if (details.get("subtask", std::string()) == "ProcessTask") {
        const auto task = details.get("details", "task", std::string());
        if (msg == AsstMsg::SubTaskStart && task.starts_with("MiniGame@BlackFlow@")) {
            m_last_opening_task = task;
            m_reset_group = task == "MiniGame@BlackFlow@StartExplore";
            if (task == "MiniGame@BlackFlow@SwipeToPioneerSquad") {
                m_squad_swipe = 1;
            }
            else if (task == "MiniGame@BlackFlow@SwipeBackToPioneerSquad") {
                m_squad_swipe = -1;
            }
            if (task == "MiniGame@BlackFlow@OpeningFailed" || task == "MiniGame@BlackFlow@NetworkDisconnected") {
                m_failure_details = details;
            }
        }
        if (msg == AsstMsg::SubTaskError && !m_last_opening_task.empty()) {
            m_failure_details = details;
        }
    }
    if (m_reset_group || m_squad_swipe != 0 || !m_failure_details.empty()) {
        return true;
    }
    return m_capture_target != BlackFlowCaptureTarget::None;
}

bool asst::BlackFlowDataCollectionTaskPlugin::_run()
{
    if (m_reset_group) {
        m_capture_group = BlackFlowOpeningCaptureGroup {};
        return true;
    }
    if (!m_failure_details.empty()) {
        m_failed = true;
        save_failure();
        return true;
    }
    if (m_squad_swipe != 0) {
        return swipe_from_card();
    }
    const auto target = m_capture_target;
    auto plan = black_flow_capture_plan(target, m_capture_group.id());

    if (plan.group_role == BlackFlowCaptureGroupRole::BeginsGroup) {
        m_capture_group.begin(OpeningCaptureGroupId { MAA_NS::format_now_for_filename() });
        plan = black_flow_capture_plan(target, m_capture_group.id());
    }
    else if (!m_capture_group.can_capture(plan)) {
        Log.warn(
            __FUNCTION__,
            "| parts-box snapshot has no matching processing-item cycle; skipping unpaired snapshot");
        return true;
    }

    cv::Mat image;
    if (plan.image_source == BlackFlowCaptureImageSource::RecognitionFrame) {
        if (const auto hit_image = get_hit_image()) {
            image = *hit_image;
        }
    }
    else if (const auto controller = ctrler()) {
        // Opening capture callbacks run after their configured stability delay. Fetch again here instead of
        // saving the recognition frame captured before that delay.
        image = controller->get_image();
    }

    if (image.empty()) {
        Log.error(__FUNCTION__, "| collection image is unavailable; stopping the BlackFlow cycle");
        m_failed = true;
        stop_process_task();
        return false;
    }

    try {
        if (utils::save_debug_image(
                image,
                plan.relative_dir,
                /* auto_clean = */ false,
                plan.description,
                plan.suffix)) {
            m_capture_group.capture_succeeded(plan);
            return true;
        }
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to save BlackFlow collection image:", e.what());
        m_failed = true;
        stop_process_task();
        return false;
    }

    Log.error(__FUNCTION__, "| failed to save BlackFlow collection image at", plan.relative_dir);
    m_failed = true;
    stop_process_task();
    return false;
}

bool asst::BlackFlowDataCollectionTaskPlugin::swipe_from_card()
{
    const auto process = dynamic_cast<ProcessTask*>(m_task_ptr);
    const auto hit = process ? process->get_last_hit() : nullptr;
    const auto swipe = hit ? black_flow_squad_swipe(hit->rect, m_squad_swipe < 0) : std::nullopt;
    const auto controller = ctrler();
    if (swipe && controller && controller->swipe(swipe->first, swipe->second, 350, true, 3, 0)) {
        Log.info("BlackFlow squad card drag", swipe->first, swipe->second);
        return true;
    }
    Log.error("BlackFlow squad card drag failed or has no valid recognized anchor");
    m_failed = true;
    m_failure_details = basic_info();
    save_failure();
    stop_process_task();
    return false;
}

void asst::BlackFlowDataCollectionTaskPlugin::save_failure()
{
    // Diagnostics are deliberately outside opening_cycles: they are not collection samples.
    const auto dir = std::filesystem::path("debug/roguelike/black_flow/opening_failures") /
                     MAA_NS::format_now_for_filename();
    auto details = m_failure_details;
    details["last_opening_task"] = m_last_opening_task;
    details["reason"] = m_last_opening_task == "MiniGame@BlackFlow@NetworkDisconnected" ?
                            "game server connection lost; reconnect in game before restarting collection" :
                            "opening task failed or exhausted its bounded recovery";
    Log.error("BlackFlow opening failed", details.to_string());
    try {
        const auto controller = ctrler();
        const auto image = controller ? controller->get_image() : cv::Mat();
        details["screenshot_saved"] = utils::save_debug_image(image, dir, false, "BlackFlow opening failure", "failure");
        const auto absolute_dir = UserDir.get() / dir;
        std::filesystem::create_directories(absolute_dir);
        std::ofstream output(absolute_dir / "failure.json");
        output << details.to_string();
        if (!output) {
            Log.error("BlackFlow opening failure metadata could not be saved", absolute_dir);
        }
    }
    catch (const std::exception& e) {
        Log.error("BlackFlow opening failure evidence could not be saved", e.what());
    }
}

void asst::BlackFlowDataCollectionTaskPlugin::stop_process_task() const
{
    if (m_task_ptr) {
        m_task_ptr->set_enable(false);
    }
}
