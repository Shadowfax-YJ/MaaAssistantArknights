#pragma once

#include "BlackFlowDataCollectionPlan.hpp"
#include "BlackFlowDataCollectionTrigger.hpp"
#include "Task/AbstractTaskPlugin.h"

namespace asst
{
class BlackFlowDataCollectionTaskPlugin final : public AbstractTaskPlugin
{
public:
    using AbstractTaskPlugin::AbstractTaskPlugin;
    virtual ~BlackFlowDataCollectionTaskPlugin() override = default;

    virtual bool verify(AsstMsg msg, const json::value& details) const override;
    bool failed() const noexcept { return m_failed; }

protected:
    virtual bool _run() override;

private:
    void stop_process_task() const;
    void save_failure();
    bool swipe_from_card();

    mutable BlackFlowCaptureTarget m_capture_target = BlackFlowCaptureTarget::None;
    mutable bool m_reset_group = false;
    mutable int m_squad_swipe = 0;
    mutable json::value m_failure_details;
    mutable std::string m_last_opening_task;
    bool m_failed = false;
    BlackFlowOpeningCaptureGroup m_capture_group;
};
} // namespace asst
