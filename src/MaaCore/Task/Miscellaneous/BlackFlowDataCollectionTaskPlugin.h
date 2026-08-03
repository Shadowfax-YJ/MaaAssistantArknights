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

protected:
    virtual bool _run() override;

private:
    void stop_process_task() const;

    mutable BlackFlowCaptureTarget m_capture_target = BlackFlowCaptureTarget::None;
    BlackFlowOpeningCaptureGroup m_capture_group;
};
} // namespace asst
