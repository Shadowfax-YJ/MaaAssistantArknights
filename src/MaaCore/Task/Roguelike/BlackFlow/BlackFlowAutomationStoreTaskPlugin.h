#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BlackFlowAutomationStoreRules.h"
#include "BlackFlowTaskPluginBase.h"
#include "Common/AsstTypes.h"

namespace asst::blackflow
{
class BlackFlowAutomationStoreTaskPlugin final : public BlackFlowTaskPluginBase
{
public:
    using BlackFlowTaskPluginBase::BlackFlowTaskPluginBase;

    virtual bool verify(AsstMsg msg, const json::value& details) const override;
    virtual void reset_in_run_variables() override;

protected:
    virtual bool _run() override;

private:
    enum class ShelfPage
    {
        Top,
        Bottom,
    };

    struct ShelfSlot
    {
        Rect rect;
        ShelfPage page = ShelfPage::Top;
    };

    struct StoreSelection
    {
        TextRect good;
        ShelfPage page = ShelfPage::Top;
        std::optional<int> ingots_before;
    };

    struct CachedShopGood
    {
        TextRect good;
        ShelfPage page = ShelfPage::Top;
        std::optional<int> price;
        std::optional<bool> buyable_when_scanned;
    };

    enum class PendingWork
    {
        None,
        ShopEnter,
        ShopDecision,
        ShopBuyConfirmed,
        ShopBuyMissed,
        ShopSellDecision,
        ShopSellConfirmed,
        ShopSellMissed,
        ShopRefreshCompleted,
        ShopLeave,
        ScrapShopEnter,
        ScrapShopDecision,
        ScrapShopBuyConfirmed,
        ScrapShopBuyMissed,
        ScrapShopSellDecision,
        ScrapShopSellConfirmed,
        ScrapShopSellMissed,
        ScrapShopRefreshCompleted,
        ScrapShopClickAtMost,
        ScrapShopReadHarvest,
        ScrapShopLeave,
    };

    [[nodiscard]] bool automation_collection_enabled() const noexcept;
    [[nodiscard]] std::vector<TextRect> recognize(const cv::Mat& image, const std::string& task) const;
    [[nodiscard]] int read_number(const cv::Mat& image, const std::string& task) const;
    [[nodiscard]] std::optional<int> read_optional_number(const cv::Mat& image, const std::string& task) const;
    [[nodiscard]] bool same_shelf_slot(const Rect& lhs, const Rect& rhs) const noexcept;
    [[nodiscard]] bool already_recorded(const Rect& rect, const std::vector<Rect>& recorded) const;
    [[nodiscard]] bool already_recorded(const ShelfSlot& slot, const std::vector<ShelfSlot>& recorded) const;
    [[nodiscard]] bool already_recorded(std::string_view name, const std::vector<std::string>& recorded) const;
    [[nodiscard]] std::optional<bool> shop_good_is_buyable(const cv::Mat& image, const Rect& name_rect) const;
    [[nodiscard]] std::optional<int> read_good_price(const cv::Mat& image, const Rect& name_rect) const;
    [[nodiscard]] bool scan_shop_goods();
    [[nodiscard]] bool select_shop_good(std::optional<StoreSelection>& selected);
    [[nodiscard]] bool select_scrap_shop_good(std::optional<StoreSelection>& selected);
    [[nodiscard]] bool relocate_selection(StoreSelection& selection, std::string_view recognition_task);
    [[nodiscard]] bool run_goods_swipe(std::string_view task);
    [[nodiscard]] std::vector<RunResources::MovementInstance> projected_entry_processing_items() const;
    void reset_shop_resume_task() const;
    [[nodiscard]] AutomationStoreIdentity current_store_identity(AutomationStoreKind kind) const noexcept;
    void clear_pending_purchase() noexcept;
    void finalize_pending_purchase(AutomationStoreKind kind);
    void capture_store_snapshot(AutomationStoreKind kind, std::string_view phase, int refresh_index) const;
    void queue_eerie_store_snapshot(std::string phase, int refresh_index);
    void capture_pending_eerie_store_snapshot(
        const cv::Mat& top_image,
        const cv::Mat& bottom_image,
        const std::vector<TextRect>& top_goods,
        const std::vector<TextRect>& bottom_goods);

    mutable PendingWork m_pending = PendingWork::None;

    std::optional<ShelfSlot> m_pending_purchase;
    std::optional<std::string> m_pending_purchase_name;
    std::optional<int> m_pending_purchase_ingots_before;
    bool m_pending_purchase_confirmed = false;
    std::optional<Rect> m_pending_sale;
    std::optional<std::string> m_pending_sale_name;

    std::vector<ShelfSlot> m_shop_purchased;
    std::vector<std::string> m_shop_purchased_names;
    std::vector<ShelfSlot> m_shop_attempted;
    std::vector<std::string> m_shop_attempted_names;
    std::vector<Rect> m_shop_sell_attempted;
    std::vector<CachedShopGood> m_shop_goods;
    std::vector<RunResources::MovementInstance> m_shop_entry_processing_items;
    int m_shop_refresh_count = 0;
    std::size_t m_shop_collectibles_purchased_in_run = 0;
    std::optional<AutomationStoreIdentity> m_active_shop_identity;
    bool m_shop_sold_in_cycle = false;
    ShelfPage m_shop_shelf_page = ShelfPage::Top;
    std::optional<std::pair<std::string, int>> m_pending_eerie_store_snapshot;

    std::vector<ShelfSlot> m_scrap_shop_purchased;
    std::vector<std::string> m_scrap_shop_purchased_names;
    std::vector<ShelfSlot> m_scrap_shop_buy_attempted;
    std::vector<std::string> m_scrap_shop_buy_attempted_names;
    std::vector<Rect> m_scrap_shop_sell_attempted;
    std::vector<RunResources::MovementInstance> m_scrap_shop_entry_processing_items;
    int m_scrap_shop_refresh_count = 0;
    std::optional<AutomationStoreIdentity> m_active_scrap_shop_identity;
    bool m_scrap_shop_sold_in_cycle = false;
    bool m_scrap_shop_has_board_vine = false;
    // 进店或买到种子时请求一次培育；培育弹窗内会连续点击“最多”3 次，选中当前持有的全部种子。
    bool m_scrap_shop_cultivation_requested = false;
    AutomationStoreRefreshLedger m_refresh_ledger;
};
} // namespace asst::blackflow
