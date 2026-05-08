#pragma once
#include "AbstractRoguelikeTaskPlugin.h"

namespace asst
{
class RoguelikeShoppingTaskPlugin : public AbstractRoguelikeTaskPlugin
{
public:
    using AbstractRoguelikeTaskPlugin::AbstractRoguelikeTaskPlugin;
    virtual ~RoguelikeShoppingTaskPlugin() override = default;

    virtual bool verify(AsstMsg msg, const json::value& details) const override;

protected:
    virtual bool _run() override;

private:
    enum class TaskAction
    {
        Shopping,
        SaveDataCollectionTrader,
    };

    // 购买一次
    bool buy_once();
    bool save_data_collection_trader_image();

    mutable TaskAction m_action = TaskAction::Shopping;
};
}
