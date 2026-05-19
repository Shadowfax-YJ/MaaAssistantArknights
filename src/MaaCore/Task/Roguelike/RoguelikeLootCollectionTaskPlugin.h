#pragma once

#include <string>
#include <string_view>

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
        LootPageFallback,
    };

    enum class PendingDirectLootKind
    {
        Unknown,
        Other,
        Recruit,
        Collectible,
    };

    void clear_pending_direct_loot() const;
    void reset_loot_page_state() const;
    bool capture_loot_action_image(cv::Mat& image);
    bool save_loot_page_fallback_if_needed(const cv::Mat& image);
    bool save_loot_image_and_record(const cv::Mat& image);
    bool save_loot_image_and_record(std::string_view type, std::string_view suffix, const cv::Mat& image);
    PendingDirectLootKind classify_pending_direct_loot() const;

private:
    mutable CaptureAction m_capture_action = CaptureAction::None;
    mutable std::string m_loot_type;
    mutable std::string m_image_suffix;
    mutable cv::Mat m_pending_direct_loot_image;
    mutable std::string m_pending_direct_loot_task;
    mutable Rect m_pending_direct_loot_button_rect;
    mutable bool m_has_pending_direct_loot_button_rect = false;
    mutable bool m_in_loot_page = false;
    mutable bool m_loot_page_fallback_captured = false;
    mutable bool m_collectible_choice_captured_before_popup = false;
    mutable bool m_current_capture_is_collectible_choice = false;
};
} // namespace asst
