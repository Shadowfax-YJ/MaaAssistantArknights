#include "BlackFlowStoreScreenshotTaskPlugin.h"

#include <exception>

#include "BlackFlowStoreScreenshotTrigger.hpp"
#include "Utils/DebugImageHelper.hpp"
#include "Utils/Logger.hpp"

bool asst::BlackFlowStoreScreenshotTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    return should_capture_black_flow_store_overview(msg, details);
}

bool asst::BlackFlowStoreScreenshotTaskPlugin::_run()
{
    const auto image = get_hit_image();
    if (!image) {
        Log.error(__FUNCTION__, "| store overview image is unavailable; stopping before investment");
        stop_process_task();
        return false;
    }

    const auto relative_dir = utils::path("debug") / "roguelike" / "black_flow_store";
    try {
        if (utils::save_debug_image(
                *image,
                relative_dir,
                /* auto_clean = */ false,
                "Black Flow store overview")) {
            return true;
        }
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to save store overview; stopping before investment:", e.what());
        stop_process_task();
        return false;
    }

    Log.error(__FUNCTION__, "| failed to save store overview; stopping before investment at", relative_dir);
    stop_process_task();
    return false;
}

void asst::BlackFlowStoreScreenshotTaskPlugin::stop_process_task() const
{
    if (m_task_ptr) {
        m_task_ptr->set_enable(false);
    }
}
