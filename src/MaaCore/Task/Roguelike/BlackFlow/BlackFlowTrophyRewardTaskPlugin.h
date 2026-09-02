#pragma once

#include "BlackFlowTaskPluginBase.h"

namespace asst::blackflow
{
class BlackFlowTrophyRewardTaskPlugin final : public BlackFlowTaskPluginBase
{
public:
    using BlackFlowTaskPluginBase::BlackFlowTaskPluginBase;

    bool verify(AsstMsg msg, const json::value& details) const override;

protected:
    bool _run() override;
};
} // namespace asst::blackflow
