#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "Task/AbstractTaskPlugin.h"

namespace asst::blackflow
{
class BlackFlowCollectionPopupTaskPlugin final : public AbstractTaskPlugin
{
public:
    using Capture = std::function<bool(std::string_view task, std::string* error)>;
    using Failure = std::function<void(std::string_view task, std::string_view error)>;

    BlackFlowCollectionPopupTaskPlugin(
        const AsstCallback& callback,
        Assistant* inst,
        std::string_view task_chain,
        Capture capture,
        Failure failure = {});

    bool verify(AsstMsg msg, const json::value& details) const override;

protected:
    bool _run() override;

private:
    Capture m_capture;
    Failure m_failure;
    mutable std::string m_pending_task;
};
} // namespace asst::blackflow
