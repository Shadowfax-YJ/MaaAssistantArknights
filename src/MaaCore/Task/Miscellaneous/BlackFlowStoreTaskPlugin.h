#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "BlackFlowClientGuard.hpp"
#include "BlackFlowStoreOrchestrator.hpp"
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
    struct PendingCollection
    {
        BlackFlowStoreCollectionSummary summary;
        std::string exploration_id;
    };

    void remember_collection(const BlackFlowStoreCollectionSummary& summary, std::string_view exploration_id);
    void finalize_collection(bool safely_exited);

    std::optional<BlackFlowClientType> m_client_type;
    std::optional<PendingCollection> m_pending_collection;
    mutable BlackFlowStorePluginInvocation m_invocation = BlackFlowStorePluginInvocation::None;
};
} // namespace asst
