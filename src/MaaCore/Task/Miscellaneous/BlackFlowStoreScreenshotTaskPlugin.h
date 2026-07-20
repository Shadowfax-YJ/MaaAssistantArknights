#pragma once

#include <optional>

#include "BlackFlowClientGuard.hpp"
#include "Task/AbstractTaskPlugin.h"

namespace asst
{
class BlackFlowStoreScreenshotTaskPlugin final : public AbstractTaskPlugin
{
public:
    using AbstractTaskPlugin::AbstractTaskPlugin;
    virtual ~BlackFlowStoreScreenshotTaskPlugin() override = default;

    virtual bool verify(AsstMsg msg, const json::value& details) const override;
    BlackFlowStoreScreenshotTaskPlugin& set_client_type(std::optional<BlackFlowClientType> client_type) noexcept;

protected:
    virtual bool _run() override;

private:
    void stop_process_task() const;

    std::optional<BlackFlowClientType> m_client_type;
};
} // namespace asst
