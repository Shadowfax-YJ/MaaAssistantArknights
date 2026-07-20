#include "BlackFlowStoreScreenshotTaskPlugin.h"

#include <cstdint>
#include <exception>
#include <span>
#include <string_view>
#include <vector>

#include "BlackFlowStorePageRepository.hpp"
#include "BlackFlowStoreScreenshotTrigger.hpp"
#include "Config/Miscellaneous/BlackFlowStoreConfig.h"
#include "Controller/Controller.h"
#include "MaaUtils/ImageIo.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Utils/Logger.hpp"
#include "Utils/Platform.hpp"
#include "Utils/WorkingDir.hpp"
#include "Vision/Roguelike/BlackFlowStoreImageAnalyzer.h"

namespace
{
std::string_view page_status_name(asst::BlackFlowPageStatus status)
{
    switch (status) {
    case asst::BlackFlowPageStatus::Complete:
        return "complete";
    case asst::BlackFlowPageStatus::Partial:
        return "partial";
    case asst::BlackFlowPageStatus::Failed:
        return "failed";
    }
    return "failed";
}
} // namespace

bool asst::BlackFlowStoreScreenshotTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    return should_capture_black_flow_store_overview(msg, details);
}

bool asst::BlackFlowStoreScreenshotTaskPlugin::_run()
{
    // The legacy trigger's action enters investment. Disable its ProcessTask before any fallible work.
    stop_process_task();

    if (!m_client_type) {
        Log.error(__FUNCTION__, "| missing admitted BlackFlow client; refusing capture before artifact creation");
        return false;
    }

    const auto first_frame = get_hit_image();
    if (!first_frame) {
        Log.error(__FUNCTION__, "| store overview image is unavailable; stopping before investment");
        return false;
    }

    try {
        const auto& config = BlackFlowStoreConfig::get_instance().get_data();
        BlackFlowStoreImageAnalyzer analyzer(config);
        constexpr BlackFlowStoreReadiness LegacyTriggerReadiness {
            .store_anchor_visible = true,
            .refresh_control_visible = true,
            .overlay_visible = false,
        };

        const auto previous = analyzer.observe(*first_frame, LegacyTriggerReadiness);
        if (!previous || !sleep(100)) {
            Log.error(__FUNCTION__, "| first store overview observation is invalid or capture was interrupted");
            return false;
        }

        auto stable_frame = ctrler()->get_image();
        const auto current = analyzer.observe(stable_frame, LegacyTriggerReadiness);
        if (!current || classify_black_flow_store_page(previous.value(), current.value(), std::nullopt) !=
                            BlackFlowStorePageClassification::StableInitial) {
            Log.error(__FUNCTION__, "| store overview did not satisfy the two-frame stability window");
            return false;
        }

        std::vector<std::uint8_t> encoded_png;
        if (!cv::imencode(".png", stable_frame, encoded_png)) {
            Log.error(__FUNCTION__, "| failed to encode stable store overview as PNG");
            return false;
        }

        const auto relative_root = utils::path("debug") / "roguelike" / "black_flow_store";
        const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<BlackFlowPngDimensions> {
            const auto image = MAA_NS::imread(path);
            if (image.empty()) {
                return std::nullopt;
            }
            return BlackFlowPngDimensions { .width = image.cols, .height = image.rows };
        };
        BlackFlowStorePageRepository repository(UserDir.get() / relative_root, config, png_verifier);
        const auto exploration = repository.begin_exploration(m_client_type.value());
        if (!exploration) {
            Log.error(__FUNCTION__, "| failed to establish a unique BlackFlow exploration directory");
            return false;
        }

        const auto result = repository.capture_page(
            exploration.value(),
            1,
            1,
            std::as_bytes(std::span(encoded_png)),
            [&](const std::filesystem::path& committed_png) {
                return analyzer.analyze_slots(MAA_NS::imread(committed_png));
            });

        if (result.disposition == BlackFlowJsonDisposition::FirstCommit ||
            result.disposition == BlackFlowJsonDisposition::Improved) {
            auto info = basic_info_with_what("BlackFlowStoreSnapshotCommitted");
            info["details"] = json::object {
                { "exploration_id", exploration->id() },
                { "page_index", 1 },
                { "attempt", result.attempt },
                { "page_status", std::string(page_status_name(result.page_status)) },
                { "origin", "live" },
                { "png_path", MAA_NS::path_to_utf8_string(relative_root / result.png_relative_path) },
                { "json_path", MAA_NS::path_to_utf8_string(relative_root / result.json_relative_path) },
            };
            callback(AsstMsg::SubTaskExtraInfo, info);
        }

        if (!result.advances_completed_pages) {
            Log.error(
                __FUNCTION__,
                "| BlackFlow store overview commit did not complete the page:",
                result.error_code,
                result.error_message);
            return false;
        }
        return true;
    }
    catch (const std::exception& e) {
        Log.error(__FUNCTION__, "| failed to commit store overview; stopping before investment:", e.what());
        return false;
    }
}

asst::BlackFlowStoreScreenshotTaskPlugin&
    asst::BlackFlowStoreScreenshotTaskPlugin::set_client_type(std::optional<BlackFlowClientType> client_type) noexcept
{
    m_client_type = std::move(client_type);
    return *this;
}

void asst::BlackFlowStoreScreenshotTaskPlugin::stop_process_task() const
{
    if (m_task_ptr) {
        m_task_ptr->set_enable(false);
    }
}
