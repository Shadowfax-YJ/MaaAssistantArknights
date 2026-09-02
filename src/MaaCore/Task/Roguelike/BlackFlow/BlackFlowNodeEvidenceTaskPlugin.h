#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "Task/AbstractTaskPlugin.h"

namespace asst::blackflow
{
class BlackFlowNodeEvidenceTaskPlugin final : public AbstractTaskPlugin
{
public:
    using Capture =
        std::function<bool(std::string_view task, std::optional<Rect> selected_button, std::string* error)>;

    BlackFlowNodeEvidenceTaskPlugin(
        const AsstCallback& callback,
        Assistant* inst,
        std::string_view task_chain,
        Capture capture);
    ~BlackFlowNodeEvidenceTaskPlugin() override = default;

    bool verify(AsstMsg msg, const json::value& details) const override;

protected:
    bool _run() override;

private:
    Capture m_capture;
    mutable std::string m_pending_task;
};
} // namespace asst::blackflow
