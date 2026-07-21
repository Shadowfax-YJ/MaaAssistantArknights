#include "BlackFlowStoreTaskPlugin.h"

#include "BlackFlowStoreCycleAdapter.hpp"
#include "BlackFlowStorePageRepository.hpp"
#include "BlackFlowStoreTaskPolicy.hpp"
#include "BlackFlowStoreTrigger.hpp"
#include "Config/Miscellaneous/BlackFlowStoreConfig.h"
#include "Controller/Controller.h"
#include "MaaUtils/ImageIo.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Task/ProcessTask.h"
#include "Utils/Logger.hpp"
#include "Utils/Platform.hpp"
#include "Utils/WorkingDir.hpp"
#include "Vision/Roguelike/BlackFlowStoreImageAnalyzer.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
class BlackFlowStoreProductionRuntime final : public asst::BlackFlowStoreCycleRuntime
{
public:
    using NamedTaskRunner = std::function<bool(std::string_view)>;
    using ImageProvider = std::function<cv::Mat()>;
    using Pause = std::function<bool(unsigned)>;
    using StopRequested = std::function<bool()>;
    using SnapshotCallback = std::function<void(const asst::BlackFlowStoreSnapshot&)>;
    using CollectionEndedCallback = std::function<void(const asst::BlackFlowStoreCollectionSummary&, std::string_view)>;

    BlackFlowStoreProductionRuntime(
        asst::BlackFlowStoreImageAnalyzer& analyzer,
        NamedTaskRunner named_task_runner,
        ImageProvider image_provider,
        Pause pause,
        StopRequested stop_requested,
        SnapshotCallback snapshot_callback,
        CollectionEndedCallback collection_ended_callback) :
        m_analyzer(analyzer),
        m_named_task_runner(std::move(named_task_runner)),
        m_image_provider(std::move(image_provider)),
        m_pause(std::move(pause)),
        m_stop_requested(std::move(stop_requested)),
        m_snapshot_callback(std::move(snapshot_callback)),
        m_collection_ended_callback(std::move(collection_ended_callback))
    {
    }

    bool run_named_task(std::string_view task_name) override { return m_named_task_runner(task_name); }

    std::optional<asst::BlackFlowStoreObservedPage> observe_store_page(
        const std::optional<asst::BlackFlowStoreTitleFingerprint>& last_committed_fingerprint) override
    {
        constexpr asst::BlackFlowStoreReadiness ReadyStorePage {
            .store_anchor_visible = true,
            .refresh_control_visible = true,
            .overlay_visible = false,
        };

        const auto first_frame = m_image_provider();
        const auto previous = m_analyzer.observe(first_frame, ReadyStorePage);
        if (!previous || !m_pause(100U) || m_stop_requested()) {
            return std::nullopt;
        }

        auto current_frame = m_image_provider();
        const auto current = m_analyzer.observe(current_frame, ReadyStorePage);
        if (!current) {
            return std::nullopt;
        }

        const auto classification =
            asst::classify_black_flow_store_page(previous.value(), current.value(), last_committed_fingerprint);
        m_stable_frame = std::move(current_frame);
        return asst::BlackFlowStoreObservedPage {
            .classification = classification,
            .title_fingerprint = current->title_fingerprint,
        };
    }

    std::optional<std::vector<std::uint8_t>> encode_observed_page() override
    {
        if (m_stable_frame.empty()) {
            return std::nullopt;
        }

        std::vector<std::uint8_t> encoded_png;
        if (!cv::imencode(".png", m_stable_frame, encoded_png)) {
            return std::nullopt;
        }
        return encoded_png;
    }

    asst::BlackFlowStoreSlotsAnalysis analyze_committed_page(const std::filesystem::path& path) override
    {
        return m_analyzer.analyze_slots(MAA_NS::imread(path));
    }

    asst::BlackFlowStoreSlotsAnalysis analyze_recovery_page(
        const std::filesystem::path& path,
        const asst::BlackFlowStoreStopRequested& cancel_requested) override
    {
        if (cancel_requested && cancel_requested()) {
            return {};
        }
        const auto image = MAA_NS::imread(path);
        if (cancel_requested && cancel_requested()) {
            return {};
        }
        return m_analyzer.analyze_slots(image, cancel_requested);
    }

    void snapshot_committed(const asst::BlackFlowStoreSnapshot& snapshot) override { m_snapshot_callback(snapshot); }

    void
        collection_ended(const asst::BlackFlowStoreCollectionSummary& summary, std::string_view exploration_id) override
    {
        m_collection_ended_callback(summary, exploration_id);
    }

    bool wait_for(std::chrono::milliseconds duration) override
    {
        return m_pause(static_cast<unsigned>(duration.count()));
    }

    bool stop_requested() const noexcept override { return m_stop_requested(); }

    std::chrono::steady_clock::time_point now() const noexcept override { return std::chrono::steady_clock::now(); }

private:
    asst::BlackFlowStoreImageAnalyzer& m_analyzer;
    NamedTaskRunner m_named_task_runner;
    ImageProvider m_image_provider;
    Pause m_pause;
    StopRequested m_stop_requested;
    SnapshotCallback m_snapshot_callback;
    CollectionEndedCallback m_collection_ended_callback;
    cv::Mat m_stable_frame;
};
} // namespace

bool asst::BlackFlowStoreTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    m_invocation = classify_black_flow_store_plugin_invocation(
        should_start_black_flow_store_cycle(msg, details),
        m_pending_collection.has_value(),
        msg == AsstMsg::SubTaskStart && details.get("subtask", std::string()) == "ProcessTask",
        details.get("details", "task", std::string()));
    return m_invocation != BlackFlowStorePluginInvocation::None;
}

bool asst::BlackFlowStoreTaskPlugin::_run()
{
    const auto invocation = std::exchange(m_invocation, BlackFlowStorePluginInvocation::None);
    if (invocation == BlackFlowStorePluginInvocation::FinalizeReturnedToEntry ||
        invocation == BlackFlowStorePluginInvocation::FinalizeFailedExit) {
        finalize_collection(invocation == BlackFlowStorePluginInvocation::FinalizeReturnedToEntry);
        return true;
    }
    if (invocation != BlackFlowStorePluginInvocation::CollectStore) {
        return false;
    }

    if (!m_client_type) {
        Log.error(__FUNCTION__, "| missing admitted BlackFlow client; refusing store collection");
        remember_collection(
            BlackFlowStoreCollectionSummary {
                .status = BlackFlowStoreCycleStatus::Failed,
                .failure = BlackFlowStoreCycleFailure::MissingClient,
            },
            std::string_view {});
        if (need_exit()) {
            finalize_collection(false);
        }
        return false;
    }

    try {
        const auto& config = BlackFlowStoreConfig::get_instance().get_data();
        BlackFlowStoreImageAnalyzer analyzer(config);
        const auto callback_relative_root = utils::path("debug") / "roguelike" / "black_flow_store";
        const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<BlackFlowPngDimensions> {
            const auto image = MAA_NS::imread(path);
            if (image.empty()) {
                return std::nullopt;
            }
            return BlackFlowPngDimensions { .width = image.cols, .height = image.rows };
        };
        BlackFlowStorePageRepository repository(UserDir.get() / callback_relative_root, config, png_verifier);

        const auto run_named_task = [&](std::string_view task_name) {
            if (need_exit()) {
                return false;
            }
            const auto policy = black_flow_store_task_policy(task_name);
            if (!policy) {
                return false;
            }
            ProcessTask task(*this, { std::string(task_name) });
            task.set_task_delay(0);
            task.set_retry_times(policy->retry_times);
            if (policy->times_limit) {
                task.set_times_limit(std::string(task_name), policy->times_limit.value());
            }
            const bool ran = task.run();
            return ran && !need_exit() && task.get_last_task_name() == policy->expected_terminal;
        };

        BlackFlowStoreProductionRuntime runtime(
            analyzer,
            run_named_task,
            [&]() { return ctrler()->get_image(); },
            [&](unsigned milliseconds) { return sleep(milliseconds); },
            [&]() { return need_exit(); },
            [&](const BlackFlowStoreSnapshot& snapshot) {
                auto info = basic_info_with_what("BlackFlowStoreSnapshotCommitted");
                info["details"] = json::object {
                    { "exploration_id", snapshot.exploration_id },
                    { "page_index", snapshot.page_index },
                    { "attempt", snapshot.attempt },
                    { "page_status", snapshot.page_status },
                    { "origin", std::string(black_flow_store_snapshot_origin_name(snapshot.origin)) },
                    { "png_path", snapshot.png_relative_path },
                    { "json_path", snapshot.json_relative_path },
                };
                callback(AsstMsg::SubTaskExtraInfo, info);
            },
            [&](const BlackFlowStoreCollectionSummary& summary, std::string_view exploration_id) {
                remember_collection(summary, exploration_id);
            });
        BlackFlowStoreCycleAdapter port(repository, m_client_type.value(), callback_relative_root, runtime);

        const auto result = run_black_flow_store_cycle(port);
        if (!m_pending_collection) {
            remember_collection(
                BlackFlowStoreCollectionSummary {
                    .status = result.status,
                    .failure = result.failure,
                    .completed_pages = result.completed_pages,
                    .successful_refreshes = result.successful_refreshes,
                },
                std::string_view {});
        }
        if (need_exit()) {
            finalize_collection(false);
        }
        if (result.status == BlackFlowStoreCycleStatus::Failed) {
            Log.error(
                __FUNCTION__,
                "| BlackFlow store collection failed; status=",
                static_cast<int>(result.status),
                "failure=",
                static_cast<int>(result.failure),
                "completed_pages=",
                result.completed_pages,
                "successful_refreshes=",
                result.successful_refreshes);
        }
        return result.status == BlackFlowStoreCycleStatus::Completed;
    }
    catch (const std::exception&) {
        remember_collection(
            BlackFlowStoreCollectionSummary {
                .status = BlackFlowStoreCycleStatus::Failed,
                .failure = BlackFlowStoreCycleFailure::Unexpected,
            },
            std::string_view {});
        if (need_exit()) {
            finalize_collection(false);
        }
        Log.error(__FUNCTION__, "| BlackFlow store collection failed without exposing internal details");
        return false;
    }
}

asst::BlackFlowStoreTaskPlugin&
    asst::BlackFlowStoreTaskPlugin::set_client_type(std::optional<BlackFlowClientType> client_type) noexcept
{
    m_client_type = std::move(client_type);
    return *this;
}

void asst::BlackFlowStoreTaskPlugin::remember_collection(
    const BlackFlowStoreCollectionSummary& summary,
    std::string_view exploration_id)
{
    m_pending_collection = PendingCollection {
        .summary = summary,
        .exploration_id = std::string(exploration_id),
    };
}

void asst::BlackFlowStoreTaskPlugin::finalize_collection(bool safely_exited)
{
    if (!m_pending_collection) {
        return;
    }
    auto pending = std::move(m_pending_collection.value());
    m_pending_collection.reset();

    const auto reason = [&]() -> std::string_view {
        if (!safely_exited) {
            return "incomplete";
        }
        switch (pending.summary.status) {
        case BlackFlowStoreCycleStatus::Completed:
            return "completed";
        case BlackFlowStoreCycleStatus::Failed:
            return "store_collection_failed";
        case BlackFlowStoreCycleStatus::Stopped:
            return "store_collection_stopped";
        }
        return "incomplete";
    }();
    auto info = basic_info_with_what("BlackFlowExplorationEnded");
    info["details"] = json::object {
        { "exploration_id", pending.exploration_id },
        { "completed_pages", pending.summary.completed_pages },
        { "successful_refreshes", pending.summary.successful_refreshes },
        { "safely_exited", safely_exited },
        { "collection_status", std::string(black_flow_store_cycle_status_name(pending.summary.status)) },
        { "failure", std::string(black_flow_store_cycle_failure_name(pending.summary.failure)) },
        { "reason", std::string(reason) },
    };
    callback(AsstMsg::SubTaskExtraInfo, info);
}
