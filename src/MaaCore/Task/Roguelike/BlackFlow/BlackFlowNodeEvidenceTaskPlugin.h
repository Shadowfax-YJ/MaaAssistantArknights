#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "BlackFlowTaskPluginBase.h"

namespace asst::blackflow
{
class BlackFlowNodeEvidenceTaskPlugin final : public BlackFlowTaskPluginBase
{
public:
    using Capture = std::function<bool(std::string_view task, std::optional<Rect> selected_button, std::string* error)>;

    BlackFlowNodeEvidenceTaskPlugin(
        const AsstCallback& callback,
        Assistant* inst,
        std::string_view task_chain,
        const std::shared_ptr<RoguelikeConfig>& config,
        const std::shared_ptr<RoguelikeControlTaskPlugin>& control,
        std::shared_ptr<BlackFlowSession> session,
        std::shared_ptr<IBlackFlowTaskPort> port,
        Capture capture);
    ~BlackFlowNodeEvidenceTaskPlugin() override = default;

    bool verify(AsstMsg msg, const json::value& details) const override;
    void reset_in_run_variables() override;

protected:
    bool _run() override;

private:
    void click_drop_with_progress_check();
    Capture m_capture;
    mutable std::string m_pending_task;
    std::string m_reward_scope;
    int m_stalled_reward_rounds = 0;
};
} // namespace asst::blackflow
