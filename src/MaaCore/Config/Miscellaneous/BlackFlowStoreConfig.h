#pragma once

#include "Config/AbstractConfig.h"
#include "Config/Miscellaneous/BlackFlowStoreConfigContract.hpp"

#include "MaaUtils/SingletonHolder.hpp"

namespace asst
{
class BlackFlowStoreConfig final : public MAA_NS::SingletonHolder<BlackFlowStoreConfig>, public AbstractConfig
{
public:
    const BlackFlowStoreConfigContract& get_data() const noexcept { return m_data; }

protected:
    bool parse(const json::value& root) override
    {
        auto parsed = parse_black_flow_store_config(root);
        if (!parsed) {
            return false;
        }
        m_data = std::move(parsed.value());
        return true;
    }

private:
    BlackFlowStoreConfigContract m_data;
};
} // namespace asst
