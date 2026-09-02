#pragma once

#include "BlackFlowTaskPluginBase.h"

namespace asst::blackflow
{
// 记录 ProcessTask 的完整语义轨迹。它不阻断后续插件：同一个回调仍要继续交给
// 路由、节点执行、商店等业务插件处理。
class BlackFlowRunLogTaskPlugin final : public BlackFlowTaskPluginBase
{
public:
    using BlackFlowTaskPluginBase::BlackFlowTaskPluginBase;

    bool load_params(const json::value& params) override;
    bool verify(AsstMsg msg, const json::value& details) const override;

protected:
    bool _run() override;

private:
    mutable AsstMsg m_pending_message = AsstMsg::SubTaskExtraInfo;
    mutable json::value m_pending_details;
};
} // namespace asst::blackflow
