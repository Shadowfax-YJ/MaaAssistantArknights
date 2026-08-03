#include "BlackFlowDataCollectionTaskPlugin.h"

#include <exception>
#include <string>

#include "BlackFlowDataCollectionPlan.hpp"
#include "Controller/Controller.h"
#include "MaaUtils/Time.hpp"
#include "Utils/DebugImageHelper.hpp"
#include "Utils/Logger.hpp"

bool asst::BlackFlowDataCollectionTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    m_capture_target = black_flow_capture_target(msg, details);
    return m_capture_target != BlackFlowCaptureTarget::None;
}

bool asst::BlackFlowDataCollectionTaskPlugin::_run()
{
    const auto target = m_capture_target;
    auto plan = black_flow_capture_plan(target, m_group_id);

    if (plan.group_role == BlackFlowCaptureGroupRole::BeginsGroup) {
        m_group_id = OpeningCaptureGroupId { MAA_NS::format_now_for_filename() };
        plan = black_flow_capture_plan(target, m_group_id);
    }
    else if (plan.group_role == BlackFlowCaptureGroupRole::RequiresGroup && !m_group_id) {
        Log.error(__FUNCTION__, "| parts-box snapshot has no matching processing-item cycle; stopping collection");
        stop_process_task();
        return false;
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
            return true;
        }
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to save BlackFlow collection image:", e.what());
        stop_process_task();
        return false;
    }

    Log.error(__FUNCTION__, "| failed to save BlackFlow collection image at", plan.relative_dir);
    stop_process_task();
    return false;
}

void asst::BlackFlowDataCollectionTaskPlugin::stop_process_task() const
{
    if (m_task_ptr) {
        m_task_ptr->set_enable(false);
    }
}
