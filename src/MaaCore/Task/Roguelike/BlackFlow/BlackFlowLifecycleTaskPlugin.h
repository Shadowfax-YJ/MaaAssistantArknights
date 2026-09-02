#pragma once

#include "BlackFlowTaskPluginBase.h"
#include "BlackFlowLifecycleRules.h"

namespace asst::blackflow
{
class BlackFlowLifecycleTaskPlugin final : public BlackFlowTaskPluginBase
{
public:
    using BlackFlowTaskPluginBase::BlackFlowTaskPluginBase;

    virtual bool load_params(const json::value& params) override;
    virtual bool verify(AsstMsg msg, const json::value& details) const override;
    virtual void reset_in_run_variables() override;

protected:
    virtual bool _run() override;

private:
    void finish_current_run(bool start_next_run);

    enum class PendingWork
    {
        None,
        PrepareRecoveryRetry,
        RecordCurrentFloor,
        ResolveHuntedAction,
        ResolveTerminalAction,
        ResetAfterAbandon,
    };

    mutable PendingWork m_pending = PendingWork::None;
    mutable json::value m_pending_details;
    mutable std::string m_recovery_retry_target;
    mutable std::string m_terminal_trigger;
    mutable std::string m_terminal_pre_task;
    bool m_stop_after_abandon = false;
};
} // namespace asst::blackflow
