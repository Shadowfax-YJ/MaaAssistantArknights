#pragma once

#include <optional>

#include "BlackFlowClientGuard.hpp"
#include "Task/AbstractTaskPlugin.h"

namespace asst
{
class BlackFlowStoreTaskPlugin final : public AbstractTaskPlugin
{
public:
    using AbstractTaskPlugin::AbstractTaskPlugin;
    virtual ~BlackFlowStoreTaskPlugin() override = default;

    virtual bool verify(AsstMsg msg, const json::value& details) const override;
    BlackFlowStoreTaskPlugin& set_client_type(std::optional<BlackFlowClientType> client_type) noexcept;

protected:
    virtual bool _run() override;

    virtual bool on_run_fails() override { return false; }

private:
    void stop_process_task() const;

    std::optional<BlackFlowClientType> m_client_type;
};
} // namespace asst
