#pragma once

#include "Task/AbstractTaskPlugin.h"

namespace asst
{
class BlackFlowStoreScreenshotTaskPlugin final : public AbstractTaskPlugin
{
public:
    using AbstractTaskPlugin::AbstractTaskPlugin;
    virtual ~BlackFlowStoreScreenshotTaskPlugin() override = default;

    virtual bool verify(AsstMsg msg, const json::value& details) const override;

protected:
    virtual bool _run() override;

private:
    void stop_process_task() const;
};
} // namespace asst
