#include "BlackFlowDataCollectionTaskPlugin.h"

#include <exception>
#include <string>
#include <string_view>

#include "Utils/DebugImageHelper.hpp"
#include "Utils/Logger.hpp"

namespace
{
struct CaptureMetadata
{
    std::string_view directory;
    std::string_view description;
};

CaptureMetadata capture_metadata(asst::BlackFlowCaptureTarget target)
{
    switch (target) {
    case asst::BlackFlowCaptureTarget::StoreOverview:
        return { "black_flow_store", "Black Flow store overview" };
    case asst::BlackFlowCaptureTarget::ProcessingItemPopup:
        return { "black_flow/processing_items", "Black Flow processing item popup" };
    case asst::BlackFlowCaptureTarget::PartsBox:
        return { "black_flow/parts_boxes", "Black Flow parts box" };
    case asst::BlackFlowCaptureTarget::None:
        break;
    }
    return { "unknown", "Black Flow data collection" };
}
} // namespace

bool asst::BlackFlowDataCollectionTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    m_capture_target = black_flow_capture_target(msg, details);
    return m_capture_target != BlackFlowCaptureTarget::None;
}

bool asst::BlackFlowDataCollectionTaskPlugin::_run()
{
    const auto target = m_capture_target;
    const auto metadata = capture_metadata(target);
    const auto image = get_hit_image();
    if (!image) {
        Log.error(__FUNCTION__, "| collection image is unavailable; stopping the BlackFlow cycle");
        stop_process_task();
        return false;
    }

    const auto relative_dir = utils::path("debug") / "roguelike" / utils::path(std::string(metadata.directory));
    try {
        if (utils::save_debug_image(
                *image,
                relative_dir,
                /* auto_clean = */ false,
                metadata.description)) {
            return true;
        }
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to save BlackFlow collection image:", e.what());
        stop_process_task();
        return false;
    }

    Log.error(__FUNCTION__, "| failed to save BlackFlow collection image at", relative_dir);
    stop_process_task();
    return false;
}

void asst::BlackFlowDataCollectionTaskPlugin::stop_process_task() const
{
    if (m_task_ptr) {
        m_task_ptr->set_enable(false);
    }
}
