#pragma once

#include <string>

#include "AbstractRoguelikeTaskPlugin.h"
#include "MaaUtils/NoWarningCVMat.hpp"

namespace asst
{
class RoguelikeLootCollectionTaskPlugin : public AbstractRoguelikeTaskPlugin
{
public:
    using AbstractRoguelikeTaskPlugin::AbstractRoguelikeTaskPlugin;
    virtual ~RoguelikeLootCollectionTaskPlugin() override = default;

    virtual bool verify(AsstMsg msg, const json::value& details) const override;

protected:
    virtual bool _run() override;

private:
    enum class CaptureAction
    {
        None,
        Immediate,
        PendingDirect,
        FinalizePending,
        ClassifyPending,
        ClearPending,
    };

    enum class PendingDirectLootKind
    {
        Unknown,
        Other,
        Recruit,
        Collectible,
    };

    void clear_pending_direct_loot() const;
    bool save_loot_image_and_record(const cv::Mat& image);
    PendingDirectLootKind classify_pending_direct_loot() const;

private:
    mutable CaptureAction m_capture_action = CaptureAction::None;
    mutable std::string m_loot_type;
    mutable std::string m_image_suffix;
    mutable cv::Mat m_pending_direct_loot_image;
    mutable std::string m_pending_direct_loot_task;
    mutable Rect m_pending_direct_loot_button_rect;
    mutable bool m_has_pending_direct_loot_button_rect = false;
};
} // namespace asst
