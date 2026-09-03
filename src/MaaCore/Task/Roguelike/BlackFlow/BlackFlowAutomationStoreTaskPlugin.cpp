#include "BlackFlowAutomationStoreTaskPlugin.h"

#include "BlackFlowAutomationStoreRules.h"
#include "BlackFlowAutomationCollectionRules.h"
#include "BlackFlowInventoryRules.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <string_view>

#include <opencv2/imgproc.hpp>

#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "Task/ProcessTask.h"
#include "Utils/Logger.hpp"
#include "Utils/StringMisc.hpp"
#include "Vision/Matcher.h"
#include "Vision/OCRer.h"
#include "Vision/Roguelike/RoguelikeParameterAnalyzer.h"

namespace asst::blackflow
{
namespace
{
constexpr std::string_view ShopEnterTask = "BlackFlow@Roguelike@AutomationShopEnter";
constexpr std::string_view ShopDecisionTask = "BlackFlow@Roguelike@AutomationShopDecision";
constexpr std::string_view ShopDecisionEntry = "BlackFlow@Roguelike@AutomationShopDecision-Enter";
constexpr std::string_view ShopAction = "BlackFlow@Roguelike@AutomationShopAction";
constexpr std::string_view ShopGoodsTask = "BlackFlow@Roguelike@AutomationShopGoods";
constexpr std::string_view ShopGoodsBottomTask = "BlackFlow@Roguelike@AutomationShopGoodsBottom";
constexpr std::string_view ShopCaptureAnchorsTask = "BlackFlow@Roguelike@AutomationShopCaptureAnchors";
constexpr std::string_view ShopGoodsSwipeToTopTask = "BlackFlow@Roguelike@AutomationShopGoodsSwipeToTop";
constexpr std::string_view ShopGoodsSwipeToBottomTask = "BlackFlow@Roguelike@AutomationShopGoodsSwipeToBottom";
constexpr std::string_view ShopBuyConfirmTask = "BlackFlow@Roguelike@AutomationShopBuyConfirm";
constexpr std::string_view ShopBuyConfirmEntry = "BlackFlow@Roguelike@AutomationShopBuyConfirm-Enter";
constexpr std::string_view ShopBuyMissedTask = "BlackFlow@Roguelike@AutomationShopBuyMissed";
constexpr std::string_view ShopSellDecisionTask = "BlackFlow@Roguelike@AutomationShopSellDecision";
constexpr std::string_view ShopSellAction = "BlackFlow@Roguelike@AutomationShopSellAction";
constexpr std::string_view ShopSellItemsTask = "BlackFlow@Roguelike@AutomationShopSellItems";
constexpr std::string_view ShopSellConfirmTask = "BlackFlow@Roguelike@AutomationShopSellConfirm";
constexpr std::string_view ShopSellConfirmEntry = "BlackFlow@Roguelike@AutomationShopSellConfirm-Enter";
constexpr std::string_view ShopSellMissedTask = "BlackFlow@Roguelike@AutomationShopSellMissed";
constexpr std::string_view ShopToggleToSellEntry = "BlackFlow@Roguelike@AutomationShopToggleToSell-Enter";
constexpr std::string_view ShopToggleToBuyEntry = "BlackFlow@Roguelike@AutomationShopToggleToBuy-Enter";
constexpr std::string_view ShopRefreshPrepare = "BlackFlow@Roguelike@AutomationShopRefreshPrepare";
constexpr std::string_view ShopRefreshCompletedTask = "BlackFlow@Roguelike@AutomationShopRefreshCompleted";
constexpr std::string_view ShopLeaveEntry = "BlackFlow@Roguelike@AutomationShopLeave-Enter";
constexpr std::string_view ShopResumeAction = "BlackFlow@Roguelike@AutomationShopResumeAction";
constexpr std::string_view ShopWalletTask = "BlackFlow@Roguelike@StageTraderInvest-Wallet";

constexpr std::string_view ScrapShopEnterTask = "BlackFlow@Roguelike@AutomationCultivateEnter";
constexpr std::string_view ScrapShopDecisionTask = "BlackFlow@Roguelike@AutomationCultivateDecision";
constexpr std::string_view ScrapShopBuyAction = "BlackFlow@Roguelike@AutomationCultivateBuyAction";
constexpr std::string_view ScrapShopBuyGoodsTask = "BlackFlow@Roguelike@AutomationCultivateBuyItems";
constexpr std::string_view ScrapShopBuyConfirmTask = "BlackFlow@Roguelike@AutomationCultivateBuyConfirm";
constexpr std::string_view ScrapShopBuyConfirmEntry = "BlackFlow@Roguelike@AutomationCultivateBuyConfirm-Enter";
constexpr std::string_view ScrapShopBuyMissedTask = "BlackFlow@Roguelike@AutomationCultivateBuyMissed";
constexpr std::string_view ScrapShopSellDecisionTask = "BlackFlow@Roguelike@AutomationCultivateSellDecision";
constexpr std::string_view ScrapShopSellAction = "BlackFlow@Roguelike@AutomationCultivateSellAction";
constexpr std::string_view ScrapShopSellItemsTask = "BlackFlow@Roguelike@AutomationCultivateSellItems";
constexpr std::string_view ScrapShopSellConfirmTask = "BlackFlow@Roguelike@AutomationCultivateSellConfirm";
constexpr std::string_view ScrapShopSellConfirmEntry = "BlackFlow@Roguelike@AutomationCultivateSellConfirm-Enter";
constexpr std::string_view ScrapShopSellMissedTask = "BlackFlow@Roguelike@AutomationCultivateSellMissed";
constexpr std::string_view ScrapShopToggleToSellEntry = "BlackFlow@Roguelike@AutomationCultivateToggleToSell-Enter";
constexpr std::string_view ScrapShopToggleToBuyEntry = "BlackFlow@Roguelike@AutomationCultivateToggleToBuy-Enter";
constexpr std::string_view ScrapShopRefreshPrepare = "BlackFlow@Roguelike@AutomationCultivateRefreshPrepare";
constexpr std::string_view ScrapShopRefreshCompletedTask = "BlackFlow@Roguelike@AutomationCultivateRefreshCompleted";
constexpr std::string_view ScrapShopStartCultivationEntry = "BlackFlow@Roguelike@AutomationCultivateStartButton-Enter";
constexpr std::string_view ScrapShopAtMostTask = "BlackFlow@Roguelike@AutomationCultivateAtMost";
constexpr std::string_view ScrapShopLeaveEntry = "BlackFlow@Roguelike@AutomationCultivateLeave-Enter";
constexpr std::string_view ScrapShopWalletTask = "BlackFlow@Roguelike@CultivateWallet";
constexpr std::string_view HeldSeedCountTask = "BlackFlow@Roguelike@CultivateHeldSeedCount";
constexpr std::string_view CultivateStartButtonTask = "BlackFlow@Roguelike@CultivateStartButton";
constexpr std::string_view HarvestItemsTask = "BlackFlow@Roguelike@CultivateHarvestItems";

constexpr int ShopRefreshReserve = 8;
constexpr int ScrapShopRefreshReserve = 8;
constexpr int SameShelfSlotTolerance = 20;
constexpr int AtMostClickTimes = 3;
constexpr int RepeatedClickInterval = 100;

std::string_view normalized_good_name(std::string_view name) noexcept
{
    return name == "“简易遥控器”" ? std::string_view("简易遥控器") : name;
}
} // namespace

bool BlackFlowAutomationStoreTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (!automation_collection_enabled() || details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }

    const std::string task = details.get("details", "task", "");
    if (msg == AsstMsg::SubTaskStart) {
        if (task == ShopEnterTask) {
            m_pending = PendingWork::ShopEnter;
        }
        else if (task == ShopDecisionTask) {
            m_pending = PendingWork::ShopDecision;
        }
        else if (task == ShopBuyMissedTask) {
            m_pending = PendingWork::ShopBuyMissed;
        }
        else if (task == ShopSellDecisionTask) {
            m_pending = PendingWork::ShopSellDecision;
        }
        else if (task == ShopSellMissedTask) {
            m_pending = PendingWork::ShopSellMissed;
        }
        else if (task == ShopRefreshCompletedTask) {
            m_pending = PendingWork::ShopRefreshCompleted;
        }
        else if (automation_shop_resume_binding_expires_on(task)) {
            m_pending = PendingWork::ShopLeave;
        }
        else if (task == ScrapShopEnterTask) {
            m_pending = PendingWork::ScrapShopEnter;
        }
        else if (task == ScrapShopDecisionTask) {
            m_pending = PendingWork::ScrapShopDecision;
        }
        else if (task == ScrapShopBuyMissedTask) {
            m_pending = PendingWork::ScrapShopBuyMissed;
        }
        else if (task == ScrapShopSellDecisionTask) {
            m_pending = PendingWork::ScrapShopSellDecision;
        }
        else if (task == ScrapShopSellMissedTask) {
            m_pending = PendingWork::ScrapShopSellMissed;
        }
        else if (task == ScrapShopRefreshCompletedTask) {
            m_pending = PendingWork::ScrapShopRefreshCompleted;
        }
        else if (task == ScrapShopAtMostTask) {
            m_pending = PendingWork::ScrapShopClickAtMost;
        }
        else if (automation_store_should_capture_cultivation_result(task)) {
            m_pending = PendingWork::ScrapShopReadHarvest;
        }
        else if (task == ScrapShopLeaveEntry) {
            m_pending = PendingWork::ScrapShopLeave;
        }
        else {
            return false;
        }
        return true;
    }

    if (msg == AsstMsg::SubTaskCompleted) {
        if (task == ShopBuyConfirmTask) {
            m_pending = PendingWork::ShopBuyConfirmed;
        }
        else if (task == ShopSellConfirmTask) {
            m_pending = PendingWork::ShopSellConfirmed;
        }
        else if (task == ScrapShopBuyConfirmTask) {
            m_pending = PendingWork::ScrapShopBuyConfirmed;
        }
        else if (task == ScrapShopSellConfirmTask) {
            m_pending = PendingWork::ScrapShopSellConfirmed;
        }
        else {
            return false;
        }
        return true;
    }
    return false;
}

void BlackFlowAutomationStoreTaskPlugin::reset_in_run_variables()
{
    m_pending = PendingWork::None;
    m_pending_purchase.reset();
    m_pending_purchase_name.reset();
    m_pending_purchase_ingots_before.reset();
    m_pending_purchase_confirmed = false;
    m_pending_sale.reset();
    m_pending_sale_name.reset();
    m_shop_purchased.clear();
    m_shop_purchased_names.clear();
    m_shop_attempted.clear();
    m_shop_attempted_names.clear();
    m_shop_sell_attempted.clear();
    m_shop_goods.clear();
    m_shop_entry_processing_items.clear();
    m_shop_refresh_count = 0;
    m_shop_collectibles_purchased_in_run = 0;
    m_active_shop_identity.reset();
    m_shop_sold_in_cycle = false;
    m_shop_shelf_page = ShelfPage::Top;
    m_pending_eerie_store_snapshot.reset();
    m_scrap_shop_purchased.clear();
    m_scrap_shop_purchased_names.clear();
    m_scrap_shop_buy_attempted.clear();
    m_scrap_shop_buy_attempted_names.clear();
    m_scrap_shop_sell_attempted.clear();
    m_scrap_shop_entry_processing_items.clear();
    m_scrap_shop_refresh_count = 0;
    m_active_scrap_shop_identity.reset();
    m_scrap_shop_sold_in_cycle = false;
    m_scrap_shop_has_board_vine = false;
    m_scrap_shop_cultivation_requested = false;
    m_refresh_ledger.clear();
    reset_shop_resume_task();
}

AutomationStoreIdentity
    BlackFlowAutomationStoreTaskPlugin::current_store_identity(AutomationStoreKind kind) const noexcept
{
    NodeId node = m_session->run().current_node;
    if (const auto& context = m_session->page_context(); context.has_value() && context->node != InvalidNodeId) {
        node = context->node;
    }
    return AutomationStoreIdentity { m_session->map_generation(), node, kind };
}

std::vector<RunResources::MovementInstance>
    BlackFlowAutomationStoreTaskPlugin::projected_entry_processing_items() const
{
    RunState projected = m_session->run();
    const MoveTransaction* transaction = m_session->transaction();
    if (transaction == nullptr ||
        (transaction->stage() != MoveTransactionStage::Committed &&
         transaction->stage() != MoveTransactionStage::PageResolved)) {
        return projected.resources.movement_instances;
    }

    if (!project_consumed_entry_processing_item(projected, transaction->proposal().movement)) {
        Log.warn(
            "BlackFlow store entry inventory projection could not consume the committed movement",
            to_string(transaction->proposal().movement));
    }
    return projected.resources.movement_instances;
}

void BlackFlowAutomationStoreTaskPlugin::clear_pending_purchase() noexcept
{
    m_pending_purchase.reset();
    m_pending_purchase_name.reset();
    m_pending_purchase_ingots_before.reset();
    m_pending_purchase_confirmed = false;
}

void BlackFlowAutomationStoreTaskPlugin::capture_store_snapshot(
    AutomationStoreKind kind,
    std::string_view phase,
    int refresh_index) const
{
    if (m_port == nullptr) {
        return;
    }
    const std::string_view store_kind = kind == AutomationStoreKind::Eerie ? "eerie_merchant" : "secret_merchant";
    std::string error;
    if (!m_port->capture_store_page(store_kind, phase, refresh_index, nullptr, &error)) {
        Log.warn("BlackFlow store page capture failed", store_kind, phase, error);
    }
}

void BlackFlowAutomationStoreTaskPlugin::queue_eerie_store_snapshot(std::string phase, int refresh_index)
{
    m_pending_eerie_store_snapshot.emplace(std::move(phase), refresh_index);
}

void BlackFlowAutomationStoreTaskPlugin::capture_pending_eerie_store_snapshot(
    const cv::Mat& top_image,
    const cv::Mat& bottom_image,
    const std::vector<TextRect>& top_goods,
    const std::vector<TextRect>& bottom_goods)
{
    if (!m_pending_eerie_store_snapshot.has_value() || m_port == nullptr) {
        return;
    }
    const auto bottom_task = Task.get<OcrTaskInfo>(ShopGoodsBottomTask);
    if (bottom_task == nullptr || top_image.empty() || bottom_image.empty() ||
        top_image.size() != bottom_image.size() || top_image.type() != bottom_image.type()) {
        Log.warn("BlackFlow eerie merchant stitched capture has incompatible screenshots");
        return;
    }

    const auto anchors = [](const std::vector<TextRect>& goods) {
        std::vector<EerieStoreStitchAnchor> result;
        result.reserve(goods.size());
        for (const TextRect& good : goods) {
            result.emplace_back(EerieStoreStitchAnchor { good.text, good.rect.x, good.rect.y });
        }
        return result;
    };
    const std::optional<int> scroll_offset = eerie_store_scroll_offset(anchors(top_goods), anchors(bottom_goods));
    if (!scroll_offset.has_value()) {
        Log.warn("BlackFlow eerie merchant stitched capture cannot locate the overlapping second row");
        return;
    }
    const auto layout = eerie_store_stitch_layout(
        top_image.cols,
        top_image.rows,
        bottom_task->roi,
        *scroll_offset);
    if (!layout.has_value()) {
        Log.warn("BlackFlow eerie merchant stitched capture calculated an invalid layout", *scroll_offset);
        return;
    }

    const auto cv_rect = [](const Rect& rect) { return cv::Rect { rect.x, rect.y, rect.width, rect.height }; };
    cv::Mat stitched(layout->output_height, layout->output_width, top_image.type(), cv::Scalar(0));
    top_image(cv_rect(layout->base_source)).copyTo(stitched(cv_rect(layout->base_destination)));
    bottom_image(cv_rect(layout->continuation_source))
        .copyTo(stitched(cv_rect(layout->continuation_destination)));

    const auto& [phase, refresh_index] = *m_pending_eerie_store_snapshot;
    std::string error;
    if (!m_port->capture_store_page("eerie_merchant", phase, refresh_index, &stitched, &error)) {
        Log.warn("BlackFlow eerie merchant stitched page capture failed", phase, error);
        return;
    }
    Log.info(
        "BlackFlow eerie merchant page stitched",
        "scroll offset",
        *scroll_offset,
        "size",
        stitched.cols,
        "x",
        stitched.rows);
    m_pending_eerie_store_snapshot.reset();
}

void BlackFlowAutomationStoreTaskPlugin::finalize_pending_purchase(AutomationStoreKind kind)
{
    if (!m_pending_purchase_confirmed || !m_pending_purchase_name.has_value()) {
        return;
    }

    const cv::Mat image = ctrler()->get_image();
    const std::string wallet_task = std::string(
        kind == AutomationStoreKind::Eerie ? ShopWalletTask : ScrapShopWalletTask);
    const std::optional<int> ingots_after = read_optional_number(image, wallet_task);
    const bool succeeded = automation_store_purchase_succeeded(
        m_pending_purchase_ingots_before,
        ingots_after);
    const bool collectible =
        kind == AutomationStoreKind::Eerie && is_eerie_store_collectible(*m_pending_purchase_name);

    if (m_port != nullptr) {
        std::string error;
        if (!m_port->record_store_purchase(
                kind == AutomationStoreKind::Eerie ? "eerie_merchant" : "secret_merchant",
                *m_pending_purchase_name,
                m_pending_purchase_ingots_before,
                ingots_after,
                collectible,
                &error)) {
            Log.warn("BlackFlow store purchase record failed", *m_pending_purchase_name, error);
        }
    }

    if (kind == AutomationStoreKind::Eerie) {
        if (succeeded) {
            m_shop_purchased_names.emplace_back(*m_pending_purchase_name);
            if (collectible) {
                ++m_shop_collectibles_purchased_in_run;
            }
            if (m_pending_purchase.has_value()) {
                m_shop_purchased.emplace_back(*m_pending_purchase);
            }
        }
        else {
            m_shop_attempted_names.emplace_back(*m_pending_purchase_name);
            if (m_pending_purchase.has_value()) {
                m_shop_attempted.emplace_back(*m_pending_purchase);
            }
        }
    }
    else if (succeeded) {
        m_scrap_shop_cultivation_requested =
            scrap_shop_purchase_requests_cultivation(*m_pending_purchase_name);
        m_scrap_shop_purchased_names.emplace_back(*m_pending_purchase_name);
        if (m_pending_purchase.has_value()) {
            m_scrap_shop_purchased.emplace_back(*m_pending_purchase);
        }
    }
    else {
        m_scrap_shop_buy_attempted_names.emplace_back(*m_pending_purchase_name);
        if (m_pending_purchase.has_value()) {
            m_scrap_shop_buy_attempted.emplace_back(*m_pending_purchase);
        }
    }

    Log.info(
        "BlackFlow store purchase verified",
        *m_pending_purchase_name,
        "before",
        m_pending_purchase_ingots_before.has_value() ? std::to_string(*m_pending_purchase_ingots_before)
                                                     : std::string("unrecognized"),
        "after",
        ingots_after.has_value() ? std::to_string(*ingots_after) : std::string("unrecognized"),
        "success",
        succeeded);
    clear_pending_purchase();
}

bool BlackFlowAutomationStoreTaskPlugin::_run()
{
    LogTraceFunction;
    const PendingWork work = m_pending;
    m_pending = PendingWork::None;

    if (work == PendingWork::ShopEnter) {
        clear_pending_purchase();
        m_pending_sale.reset();
        m_pending_sale_name.reset();
        m_shop_purchased.clear();
        m_shop_purchased_names.clear();
        m_shop_attempted.clear();
        m_shop_attempted_names.clear();
        m_shop_sell_attempted.clear();
        m_shop_goods.clear();
        m_shop_entry_processing_items = projected_entry_processing_items();
        m_active_shop_identity = current_store_identity(AutomationStoreKind::Eerie);
        m_shop_refresh_count = m_refresh_ledger.refresh_count(*m_active_shop_identity);
        m_shop_sold_in_cycle = false;
        m_shop_shelf_page = ShelfPage::Top;
        Task.set_task_base(std::string(ShopResumeAction), std::string(automation_shop_resume_base_task()));
        queue_eerie_store_snapshot("initial", m_shop_refresh_count);
        return true;
    }
    if (work == PendingWork::ShopDecision) {
        finalize_pending_purchase(AutomationStoreKind::Eerie);
        std::optional<StoreSelection> selection;
        if (!select_shop_good(selection)) {
            return false;
        }
        if (selection.has_value()) {
            const std::string_view recognition_task = selection->page == ShelfPage::Top
                                                          ? ShopGoodsTask
                                                          : ShopGoodsBottomTask;
            if (!relocate_selection(*selection, recognition_task)) {
                // 刷新转场可能先露出旧的下半货架，随后才回到顶部。缓存名称仍然有效，
                // 但旧坐标可能已经落在投资入口上；找不到当前同名商品时只重扫，绝不盲点。
                Log.warn(
                    "BlackFlow automation 诡意行商商品当前坐标校验失败，重新扫描货架",
                    normalized_good_name(selection->good.text));
                m_shop_goods.clear();
                Task.set_task_base(std::string(ShopAction), std::string(ShopDecisionEntry));
                return true;
            }
            record_run_event(
                RunLogLevel::Info,
                "store.eerie.purchase.select",
                "started",
                "pending",
                json::object {
                    { "name", selection->good.text },
                    { "shelf", selection->page == ShelfPage::Top ? "top" : "bottom" },
                    { "rect", json::array { selection->good.rect.x, selection->good.rect.y,
                                             selection->good.rect.width, selection->good.rect.height } },
                },
                "BlackFlowAutomationStore",
                nullptr,
                true);
            ctrler()->click(selection->good.rect);
            m_pending_purchase = ShelfSlot { selection->good.rect, selection->page };
            m_pending_purchase_name = std::string(normalized_good_name(selection->good.text));
            m_pending_purchase_ingots_before = selection->ingots_before;
            m_pending_purchase_confirmed = false;
            Task.set_task_base(std::string(ShopAction), std::string(ShopBuyConfirmEntry));
            Log.info(
                "BlackFlow automation 诡意行商 purchase selected",
                selection->good.text,
                "货架",
                selection->page == ShelfPage::Top ? "上半" : "下半");
            return true;
        }

        Task.set_task_base(std::string(ShopAction), std::string(ShopToggleToSellEntry));
        return true;
    }
    if (work == PendingWork::ShopBuyConfirmed) {
        m_pending_purchase_confirmed = m_pending_purchase_name.has_value();
        return true;
    }
    if (work == PendingWork::ShopBuyMissed) {
        if (m_pending_purchase.has_value()) {
            m_shop_attempted.emplace_back(*m_pending_purchase);
            m_pending_purchase.reset();
        }
        if (m_pending_purchase_name.has_value()) {
            m_shop_attempted_names.emplace_back(std::move(*m_pending_purchase_name));
            m_pending_purchase_name.reset();
        }
        m_pending_purchase_ingots_before.reset();
        m_pending_purchase_confirmed = false;
        return true;
    }
    if (work == PendingWork::ShopSellDecision) {
        const cv::Mat image = ctrler()->get_image();
        const auto items = recognize(image, std::string(ShopSellItemsTask));
        const auto available = std::ranges::find_if(items, [&](const TextRect& item) {
            if (already_recorded(item.rect, m_shop_sell_attempted)) {
                return false;
            }
            const std::string_view name = normalized_good_name(item.text);
            if (!merchant_sale_allowed(name)) {
                return false;
            }
            const std::optional<int> minimum = minimum_natural_sale_price(name);
            if (!minimum.has_value()) {
                return true;
            }
            const std::optional<int> price = read_good_price(image, item.rect);
            Log.info(
                "BlackFlow automation 诡意行商自然物出售报价",
                name,
                "报价",
                price.has_value() ? std::to_string(*price) : std::string("未识别"),
                "最低价",
                *minimum,
                "出售",
                natural_sale_price_allowed(name, price) ? "是" : "否");
            return natural_sale_price_allowed(name, price);
        });
        if (available != items.end()) {
            record_run_event(
                RunLogLevel::Info,
                "store.eerie.sale.select",
                "started",
                "pending",
                json::object {
                    { "name", available->text },
                    { "rect", json::array { available->rect.x, available->rect.y,
                                             available->rect.width, available->rect.height } },
                },
                "BlackFlowAutomationStore",
                std::make_shared<cv::Mat>(image));
            ctrler()->click(available->rect);
            m_pending_sale = available->rect;
            m_pending_sale_name = std::string(normalized_good_name(available->text));
            Task.set_task_base(std::string(ShopSellAction), std::string(ShopSellConfirmEntry));
            return true;
        }

        m_pending_sale.reset();
        m_pending_sale_name.reset();
        if (m_shop_sold_in_cycle) {
            // 出售所得重新参与同一货架的购买判断；仍然买不起时，下一轮出售为空，才进入刷新判断。
            m_shop_sold_in_cycle = false;
            m_shop_attempted.clear();
            m_shop_attempted_names.clear();
            m_shop_sell_attempted.clear();
            // 出售会改变整张货架的可购买状态；商品位置虽然不变，但重新采一轮状态
            // 可以让此前买不起且价格 OCR 缺失的商品重新进入候选。
            m_shop_goods.clear();
            Task.set_task_base(std::string(ShopSellAction), std::string(ShopToggleToBuyEntry));
            return true;
        }

        const int wallet = read_number(image, std::string(ShopWalletTask));
        if (automation_store_can_refresh(m_shop_refresh_count, wallet, ShopRefreshReserve)) {
            Task.set_task_base(std::string(ShopSellAction), std::string(ShopRefreshPrepare));
        }
        else {
            Task.set_task_base(std::string(ShopSellAction), std::string(ShopLeaveEntry));
        }
        return true;
    }
    if (work == PendingWork::ShopSellConfirmed) {
        m_shop_sold_in_cycle = true;
        m_pending_sale.reset();
        m_pending_sale_name.reset();
        return true;
    }
    if (work == PendingWork::ShopSellMissed) {
        if (m_pending_sale.has_value()) {
            m_shop_sell_attempted.emplace_back(*m_pending_sale);
            m_pending_sale.reset();
        }
        m_pending_sale_name.reset();
        return true;
    }
    if (work == PendingWork::ShopRefreshCompleted) {
        m_shop_refresh_count = m_active_shop_identity.has_value()
                                   ? m_refresh_ledger.record_refresh(*m_active_shop_identity)
                                   : std::min(m_shop_refresh_count + 1, AutomationStoreMaxRefreshTimes);
        clear_pending_purchase();
        m_shop_purchased.clear();
        m_shop_attempted.clear();
        m_shop_attempted_names.clear();
        m_shop_sell_attempted.clear();
        m_shop_goods.clear();
        m_shop_sold_in_cycle = false;
        m_shop_shelf_page = ShelfPage::Top;
        queue_eerie_store_snapshot("after_refresh", m_shop_refresh_count);
        return true;
    }
    if (work == PendingWork::ShopLeave) {
        finalize_pending_purchase(AutomationStoreKind::Eerie);
        reset_shop_resume_task();
        m_active_shop_identity.reset();
        return true;
    }

    if (work == PendingWork::ScrapShopEnter) {
        clear_pending_purchase();
        m_pending_sale.reset();
        m_pending_sale_name.reset();
        m_scrap_shop_purchased.clear();
        m_scrap_shop_purchased_names.clear();
        m_scrap_shop_buy_attempted.clear();
        m_scrap_shop_buy_attempted_names.clear();
        m_scrap_shop_sell_attempted.clear();
        m_scrap_shop_entry_processing_items = projected_entry_processing_items();
        m_active_scrap_shop_identity = current_store_identity(AutomationStoreKind::Secret);
        m_scrap_shop_refresh_count = m_refresh_ledger.refresh_count(*m_active_scrap_shop_identity);
        m_scrap_shop_sold_in_cycle = false;
        m_scrap_shop_has_board_vine = false;
        m_scrap_shop_cultivation_requested = true;
        capture_store_snapshot(AutomationStoreKind::Secret, "initial", m_scrap_shop_refresh_count);
        return true;
    }
    if (work == PendingWork::ScrapShopDecision) {
        finalize_pending_purchase(AutomationStoreKind::Secret);
        const cv::Mat image = ctrler()->get_image();
        if (m_scrap_shop_cultivation_requested) {
            const int held_seeds = read_number(image, std::string(HeldSeedCountTask));
            const bool start_button_visible = !recognize(image, std::string(CultivateStartButtonTask)).empty();
            if (scrap_shop_should_start_cultivation(true, held_seeds, start_button_visible)) {
                // 一次培育会在 AtMost 回调中连续点击“最多”3 次，已经会选中当前全部种子；
                // 消费本次请求，收获后直接返回购买流程，不再重复开启另一轮培育。
                m_scrap_shop_cultivation_requested = false;
                Task.set_task_base(std::string(ScrapShopBuyAction), std::string(ScrapShopStartCultivationEntry));
                return true;
            }
            m_scrap_shop_cultivation_requested = false;
        }

        std::optional<StoreSelection> selection;
        if (!select_scrap_shop_good(selection)) {
            return false;
        }
        if (selection.has_value()) {
            record_run_event(
                RunLogLevel::Info,
                "store.secret.purchase.select",
                "started",
                "pending",
                json::object {
                    { "name", selection->good.text },
                    { "shelf", selection->page == ShelfPage::Top ? "top" : "bottom" },
                    { "rect", json::array { selection->good.rect.x, selection->good.rect.y,
                                             selection->good.rect.width, selection->good.rect.height } },
                },
                "BlackFlowAutomationStore",
                nullptr,
                true);
            ctrler()->click(selection->good.rect);
            m_pending_purchase = ShelfSlot { selection->good.rect, selection->page };
            m_pending_purchase_name = std::string(normalized_good_name(selection->good.text));
            m_pending_purchase_ingots_before = selection->ingots_before;
            m_pending_purchase_confirmed = false;
            Task.set_task_base(std::string(ScrapShopBuyAction), std::string(ScrapShopBuyConfirmEntry));
            Log.info(
                "BlackFlow automation 秘境行商 purchase selected",
                selection->good.text,
                "货架",
                selection->page == ShelfPage::Top ? "上半" : "下半");
            return true;
        }

        Task.set_task_base(std::string(ScrapShopBuyAction), std::string(ScrapShopToggleToSellEntry));
        return true;
    }
    if (work == PendingWork::ScrapShopBuyConfirmed) {
        m_pending_purchase_confirmed = m_pending_purchase_name.has_value();
        return true;
    }
    if (work == PendingWork::ScrapShopBuyMissed) {
        if (m_pending_purchase.has_value()) {
            m_scrap_shop_buy_attempted.emplace_back(*m_pending_purchase);
            m_pending_purchase.reset();
        }
        if (m_pending_purchase_name.has_value()) {
            m_scrap_shop_buy_attempted_names.emplace_back(std::move(*m_pending_purchase_name));
            m_pending_purchase_name.reset();
        }
        m_pending_purchase_ingots_before.reset();
        m_pending_purchase_confirmed = false;
        return true;
    }
    if (work == PendingWork::ScrapShopSellDecision) {
        const cv::Mat image = ctrler()->get_image();
        const auto items = recognize(image, std::string(ScrapShopSellItemsTask));
        const bool saw_board_vine = std::ranges::any_of(items, [](const TextRect& item) {
            return normalized_good_name(item.text) == "板藤";
        });
        const bool newly_confirmed_board_vine = saw_board_vine && !m_scrap_shop_has_board_vine;
        m_scrap_shop_has_board_vine = m_scrap_shop_has_board_vine || saw_board_vine;
        if (newly_confirmed_board_vine) {
            Log.info("BlackFlow automation 秘境行商确认持有板藤，重新检查条件购买商品");
            Task.set_task_base(std::string(ScrapShopSellAction), std::string(ScrapShopToggleToBuyEntry));
            return true;
        }
        const auto available = std::ranges::find_if(items, [&](const TextRect& item) {
            if (already_recorded(item.rect, m_scrap_shop_sell_attempted)) {
                return false;
            }
            const std::string_view name = normalized_good_name(item.text);
            if (!merchant_sale_allowed(name)) {
                return false;
            }
            const std::optional<int> minimum = minimum_natural_sale_price(name);
            if (!minimum.has_value()) {
                return true;
            }
            const std::optional<int> price = read_good_price(image, item.rect);
            Log.info(
                "BlackFlow automation 秘境行商自然物出售报价",
                name,
                "报价",
                price.has_value() ? std::to_string(*price) : std::string("未识别"),
                "最低价",
                *minimum,
                "出售",
                natural_sale_price_allowed(name, price) ? "是" : "否");
            return natural_sale_price_allowed(name, price);
        });
        if (available != items.end()) {
            record_run_event(
                RunLogLevel::Info,
                "store.secret.sale.select",
                "started",
                "pending",
                json::object {
                    { "name", available->text },
                    { "rect", json::array { available->rect.x, available->rect.y,
                                             available->rect.width, available->rect.height } },
                },
                "BlackFlowAutomationStore",
                std::make_shared<cv::Mat>(image));
            ctrler()->click(available->rect);
            m_pending_sale = available->rect;
            m_pending_sale_name = std::string(normalized_good_name(available->text));
            Task.set_task_base(std::string(ScrapShopSellAction), std::string(ScrapShopSellConfirmEntry));
            return true;
        }

        m_pending_sale.reset();
        m_pending_sale_name.reset();
        if (m_scrap_shop_sold_in_cycle) {
            // 出售所得可能让先前买不起的商品重新变得可买；回到购买判断。
            m_scrap_shop_sold_in_cycle = false;
            m_scrap_shop_buy_attempted.clear();
            m_scrap_shop_buy_attempted_names.clear();
            m_scrap_shop_sell_attempted.clear();
            Task.set_task_base(std::string(ScrapShopSellAction), std::string(ScrapShopToggleToBuyEntry));
            return true;
        }

        const int wallet = read_number(image, std::string(ScrapShopWalletTask));
        if (automation_store_can_refresh(m_scrap_shop_refresh_count, wallet, ScrapShopRefreshReserve)) {
            Task.set_task_base(std::string(ScrapShopSellAction), std::string(ScrapShopRefreshPrepare));
        }
        else {
            Task.set_task_base(std::string(ScrapShopSellAction), std::string(ScrapShopLeaveEntry));
        }
        return true;
    }
    if (work == PendingWork::ScrapShopSellConfirmed) {
        if (m_pending_sale_name == "板藤") {
            m_scrap_shop_has_board_vine = false;
        }
        m_scrap_shop_sold_in_cycle = true;
        m_pending_sale.reset();
        m_pending_sale_name.reset();
        return true;
    }
    if (work == PendingWork::ScrapShopSellMissed) {
        if (m_pending_sale.has_value()) {
            m_scrap_shop_sell_attempted.emplace_back(*m_pending_sale);
            m_pending_sale.reset();
        }
        m_pending_sale_name.reset();
        return true;
    }
    if (work == PendingWork::ScrapShopRefreshCompleted) {
        m_scrap_shop_refresh_count = m_active_scrap_shop_identity.has_value()
                                         ? m_refresh_ledger.record_refresh(*m_active_scrap_shop_identity)
                                         : std::min(m_scrap_shop_refresh_count + 1, AutomationStoreMaxRefreshTimes);
        clear_pending_purchase();
        m_pending_sale.reset();
        m_scrap_shop_purchased.clear();
        m_scrap_shop_buy_attempted.clear();
        m_scrap_shop_buy_attempted_names.clear();
        m_scrap_shop_sell_attempted.clear();
        m_scrap_shop_sold_in_cycle = false;
        capture_store_snapshot(AutomationStoreKind::Secret, "after_refresh", m_scrap_shop_refresh_count);
        return true;
    }
    if (work == PendingWork::ScrapShopClickAtMost) {
        if (const auto hit = get_hit_detail<Matcher::Result>(); hit != nullptr) {
            record_run_event(
                RunLogLevel::Info,
                "cultivation.select-all-seeds",
                "started",
                "pending",
                json::object { { "click_count", AtMostClickTimes },
                               { "rect", json::array { hit->rect.x, hit->rect.y, hit->rect.width, hit->rect.height } } },
                "BlackFlowAutomationStore",
                get_hit_image());
            int times = AtMostClickTimes;
            while (times-- > 0) {
                ctrler()->click(hit->rect);
                sleep(RepeatedClickInterval);
            }
            record_run_event(
                RunLogLevel::Info,
                "cultivation.select-all-seeds",
                "completed",
                "success",
                json::object { { "click_count", AtMostClickTimes } },
                "BlackFlowAutomationStore",
                nullptr,
                true);
        }
        return true;
    }
    if (work == PendingWork::ScrapShopReadHarvest) {
        // 此时结果页已稳定识别，但 HarvestConfirm 尚未点击；把确认前的完整结果页
        // 保存到当前秘境行商节点目录。
        capture_store_snapshot(
            AutomationStoreKind::Secret,
            "cultivation_result_before_confirm",
            m_scrap_shop_refresh_count);
        const auto items = recognize(ctrler()->get_image(), std::string(HarvestItemsTask));
        Log.info("BlackFlow automation cultivation harvest recognized", "count", items.size());
        return true;
    }
    if (work == PendingWork::ScrapShopLeave) {
        finalize_pending_purchase(AutomationStoreKind::Secret);
        m_active_scrap_shop_identity.reset();
        return true;
    }
    return true;
}

bool BlackFlowAutomationStoreTaskPlugin::automation_collection_enabled() const noexcept
{
    return m_session != nullptr && m_session->profile() == "automation_collection" &&
           m_config->get_theme() == RoguelikeTheme::BlackFlow &&
           m_config->get_mode() == RoguelikeMode::BlackFlowAutomationCollection;
}

std::vector<TextRect> BlackFlowAutomationStoreTaskPlugin::recognize(const cv::Mat& image, const std::string& task) const
{
    const auto task_info = Task.get<OcrTaskInfo>(task);
    if (task_info == nullptr) {
        return {};
    }
    OCRer analyzer(image);
    analyzer.set_task_info(task_info);
    analyzer.set_required(task_info->text);
    const auto result = analyzer.analyze();
    return result.has_value() ? std::move(*result) : std::vector<TextRect> {};
}

int BlackFlowAutomationStoreTaskPlugin::read_number(const cv::Mat& image, const std::string& task) const
{
    RoguelikeParameterAnalyzer analyzer(image);
    return analyzer.get_number(image, task);
}

std::optional<int>
    BlackFlowAutomationStoreTaskPlugin::read_optional_number(const cv::Mat& image, const std::string& task) const
{
    const auto task_info = Task.get<OcrTaskInfo>(task);
    if (task_info == nullptr) {
        return std::nullopt;
    }
    OCRer analyzer(image);
    analyzer.set_task_info(task_info);
    analyzer.set_replace(Task.get<OcrTaskInfo>("NumberOcrReplace")->replace_map);
    analyzer.set_use_char_model(true);
    const auto results = analyzer.analyze();
    if (!results.has_value()) {
        return std::nullopt;
    }
    for (const TextRect& result : *results) {
        int value = 0;
        if (utils::chars_to_number(result.text, value) && value >= 0) {
            return value;
        }
    }
    return std::nullopt;
}

bool BlackFlowAutomationStoreTaskPlugin::same_shelf_slot(const Rect& lhs, const Rect& rhs) const noexcept
{
    return std::abs(lhs.x + lhs.width / 2 - (rhs.x + rhs.width / 2)) <= SameShelfSlotTolerance &&
           std::abs(lhs.y + lhs.height / 2 - (rhs.y + rhs.height / 2)) <= SameShelfSlotTolerance;
}

bool BlackFlowAutomationStoreTaskPlugin::already_recorded(const Rect& rect, const std::vector<Rect>& recorded) const
{
    return std::ranges::any_of(recorded, [&](const Rect& other) { return same_shelf_slot(rect, other); });
}

bool BlackFlowAutomationStoreTaskPlugin::already_recorded(
    const ShelfSlot& slot,
    const std::vector<ShelfSlot>& recorded) const
{
    return std::ranges::any_of(recorded, [&](const ShelfSlot& other) {
        return slot.page == other.page && same_shelf_slot(slot.rect, other.rect);
    });
}

bool BlackFlowAutomationStoreTaskPlugin::already_recorded(
    std::string_view name,
    const std::vector<std::string>& recorded) const
{
    return std::ranges::find(recorded, name) != recorded.end();
}

std::optional<bool>
    BlackFlowAutomationStoreTaskPlugin::shop_good_is_buyable(const cv::Mat& image, const Rect& name_rect) const
{
    if (!image.empty()) {
        const cv::Rect image_bounds(0, 0, image.cols, image.rows);
        const cv::Rect color_roi = make_rect<cv::Rect>(merchant_buyability_color_roi(name_rect)) & image_bounds;
        if (color_roi.area() > 0) {
            cv::Mat hsv;
            cv::Mat red_low;
            cv::Mat red_high;
            cv::Mat red_mask;
            cv::Mat green_mask;
            cv::cvtColor(image(color_roi), hsv, cv::COLOR_BGR2HSV);
            // Price-band background samples from real BlackFlow merchant pages:
            // red is clustered around H=0..12 (with a small wraparound range),
            // while affordable teal/green is around H=35..105. Very dark pixels
            // and bright white text are excluded from both counts.
            cv::inRange(hsv, cv::Scalar(0, 46, 21), cv::Scalar(12, 255, 210), red_low);
            cv::inRange(hsv, cv::Scalar(168, 46, 21), cv::Scalar(179, 255, 210), red_high);
            cv::bitwise_or(red_low, red_high, red_mask);
            cv::inRange(hsv, cv::Scalar(35, 46, 21), cv::Scalar(105, 255, 210), green_mask);

            const std::size_t red_pixels = static_cast<std::size_t>(cv::countNonZero(red_mask));
            const std::size_t green_pixels = static_cast<std::size_t>(cv::countNonZero(green_mask));
            const std::size_t total_pixels = hsv.total();
            const std::optional<bool> color_evidence =
                merchant_buyability_color_evidence(red_pixels, green_pixels, total_pixels);
            Log.info(
                "BlackFlow automation 商品可购买底色",
                make_rect<Rect>(color_roi),
                "红",
                red_pixels,
                "绿",
                green_pixels,
                "总计",
                total_pixels,
                "判定",
                color_evidence.has_value() ? (*color_evidence ? "可购买" : "不可购买") : "不确定");
            if (color_evidence.has_value()) {
                return color_evidence;
            }
        }
    }

    Matcher matcher(image);
    matcher.set_task_info("RoguelikeTraderShopping");
    matcher.set_roi(merchant_buyability_color_roi(name_rect));
    return positive_shelf_buyability_evidence(matcher.analyze().has_value());
}

std::optional<int>
    BlackFlowAutomationStoreTaskPlugin::read_good_price(const cv::Mat& image, const Rect& name_rect) const
{
    if (image.empty()) {
        return std::nullopt;
    }

    // 售价数字是低饱和度的亮白字；先去掉青绿色货币图标、商品光效和暗色卡片纹理，
    // 再让数字 OCR 处理。这样左侧图标不会再生成 114 一类伪数字。
    cv::Mat hsv;
    cv::Mat price_mask;
    cv::Mat filtered;
    cv::cvtColor(image, hsv, cv::COLOR_BGR2HSV);
    cv::inRange(
        hsv,
        cv::Scalar(0, 0, PriceTextMinimumValue),
        cv::Scalar(179, PriceTextMaximumSaturation, 255),
        price_mask);
    cv::cvtColor(price_mask, filtered, cv::COLOR_GRAY2BGR);

    OCRer analyzer(filtered);
    analyzer.set_task_info("NumberOcrReplace");
    // 名称下方卡片的右侧才是售价。旧 ROI 从名称左侧开始，货币图标和卡片纹理
    // 曾被误识别为 114，导致实际报价 2 的雾滚草越过最低售价保护。
    analyzer.set_roi(merchant_price_roi(name_rect));
    analyzer.set_replace(Task.get<OcrTaskInfo>("NumberOcrReplace")->replace_map);
    analyzer.set_use_char_model(true);
    const auto results = analyzer.analyze();
    if (!results.has_value()) {
        return std::nullopt;
    }
    std::vector<std::pair<int, std::string_view>> candidates;
    candidates.reserve(results->size());
    for (const TextRect& result : *results) {
        candidates.emplace_back(result.rect.x, result.text);
    }
    return rightmost_numeric_price(candidates);
}

bool BlackFlowAutomationStoreTaskPlugin::scan_shop_goods()
{
    // 不能只相信内存里的 ShelfPage：商店刷新会异步把实际货架复位到顶部，
    // 现场曾因此把下半货架的商品坐标缓存成顶部坐标。每次全量扫描先物理回顶。
    if (!run_goods_swipe(ShopGoodsSwipeToTopTask)) {
        return false;
    }
    m_shop_shelf_page = ShelfPage::Top;

    const cv::Mat top_image = ctrler()->get_image();
    const auto top_goods = recognize(top_image, std::string(ShopGoodsTask));
    // Screenshot stitching must not depend on the purchase whitelist. The
    // overlapping row can consist entirely of goods we intentionally never
    // buy, while generic labels such as "价格" still provide stable anchors.
    const auto top_capture_anchors = m_pending_eerie_store_snapshot.has_value()
                                         ? recognize(top_image, std::string(ShopCaptureAnchorsTask))
                                         : std::vector<TextRect> {};

    if (!run_goods_swipe(ShopGoodsSwipeToBottomTask)) {
        return false;
    }
    m_shop_shelf_page = ShelfPage::Bottom;
    const cv::Mat bottom_image = ctrler()->get_image();
    const auto bottom_goods = recognize(bottom_image, std::string(ShopGoodsBottomTask));
    const auto bottom_capture_anchors = m_pending_eerie_store_snapshot.has_value()
                                            ? recognize(bottom_image, std::string(ShopCaptureAnchorsTask))
                                            : std::vector<TextRect> {};
    capture_pending_eerie_store_snapshot(top_image, bottom_image, top_capture_anchors, bottom_capture_anchors);

    m_shop_goods.clear();
    m_shop_goods.reserve(top_goods.size() + bottom_goods.size());
    for (const TextRect& good : top_goods) {
        m_shop_goods.emplace_back(
            CachedShopGood {
                good,
                ShelfPage::Top,
                read_good_price(top_image, good.rect),
                shop_good_is_buyable(top_image, good.rect),
            });
    }
    for (const TextRect& good : bottom_goods) {
        m_shop_goods.emplace_back(
            CachedShopGood {
                good,
                ShelfPage::Bottom,
                read_good_price(bottom_image, good.rect),
                shop_good_is_buyable(bottom_image, good.rect),
            });
    }
    Log.info(
        "BlackFlow automation 诡意行商货架位置已缓存",
        "上半商品",
        top_goods.size(),
        "下半商品",
        bottom_goods.size());
    return true;
}

bool BlackFlowAutomationStoreTaskPlugin::select_shop_good(std::optional<StoreSelection>& selected)
{
    selected.reset();
    if (m_shop_goods.empty() && !scan_shop_goods()) {
        return false;
    }

    const cv::Mat current_image = ctrler()->get_image();
    const std::optional<int> wallet = read_optional_number(current_image, std::string(ShopWalletTask));

    const auto& recruited_operators = m_config->status().opers;
    const auto operator_recruited = [&](std::string_view name) {
        return recruited_operators.contains(std::string(name));
    };
    const auto operator_elite_two = [&](std::string_view name) {
        const auto iter = recruited_operators.find(std::string(name));
        return iter != recruited_operators.end() && iter->second.elite >= 2;
    };
    const AutomationCollectionTeamProgress team_progress {
        .first_operator_elite_two = operator_elite_two(AutomationCollectionFirstOperator),
        .caster_operator_recruited = operator_recruited(AutomationCollectionCasterOperator),
        .core_operator_elite_two = operator_elite_two(AutomationCollectionCoreOperator),
        .defender_operator_recruited = operator_recruited(AutomationCollectionDefenderOperator),
        .specialist_operator_recruited = operator_recruited(AutomationCollectionSpecialistOperator),
    };
    std::vector<StoreGoodOffer> offers;
    offers.reserve(m_shop_goods.size());
    for (CachedShopGood& cached : m_shop_goods) {
        const std::string_view name = normalized_good_name(cached.good.text);
        const ShelfSlot slot { cached.good.rect, cached.page };
        const bool progress_purchase_allowed = automation_collection_shop_progress_purchase_allowed(
            name,
            team_progress);
        std::optional<bool> shelf_buyable;
        if (cached.page == m_shop_shelf_page) {
            shelf_buyable = shop_good_is_buyable(current_image, cached.good.rect);
            cached.buyable_when_scanned = shelf_buyable;
        }
        else if (cached.buyable_when_scanned == true) {
            // 货架亮起是比价格 OCR 更强的正证据。钱包下降后它可能暂时过期，但最多
            // 触发一次由确认弹窗兜底的失败点击；不能反过来让异常价格永久跳过商品。
            shelf_buyable = true;
        }
        else if (cached.price.has_value() && wallet.has_value()) {
            shelf_buyable = *cached.price <= *wallet;
        }
        else {
            shelf_buyable = cached.buyable_when_scanned;
        }
        const std::size_t entry_usable_count =
            usable_processing_item_count(name, m_shop_entry_processing_items);
        const std::size_t purchased_count = std::ranges::count(m_shop_purchased_names, name);
        offers.emplace_back(StoreGoodOffer {
            name,
            cached.price,
            automation_store_purchase_quota_allowed(name, entry_usable_count, purchased_count) &&
                progress_purchase_allowed &&
                eerie_store_collectible_purchase_allowed(name, m_shop_collectibles_purchased_in_run) &&
                !already_recorded(slot, m_shop_purchased) &&
                purchase_name_attempt_allowed(name, m_shop_attempted_names),
            shelf_buyable,
        });
        Log.info(
            "BlackFlow automation 诡意行商商品",
            name,
            "货架",
            cached.page == ShelfPage::Top ? "上半" : "下半",
            "价格",
            cached.price.has_value() ? std::to_string(*cached.price) : std::string("未识别"),
            "源石锭",
            wallet.has_value() ? std::to_string(*wallet) : std::string("未识别"),
            "进节点时可用同类加工品",
            entry_usable_count,
            "本节点已买",
            purchased_count,
            "卡片可购买",
            shelf_buyable.has_value() ? (*shelf_buyable ? "是" : "否") : "未确认");
    }

    const auto preferred = select_preferred_affordable_good(ShopBuyPriority, offers, wallet);
    if (!preferred.has_value()) {
        return true;
    }

    const CachedShopGood& target = m_shop_goods[*preferred];
    if (target.page != m_shop_shelf_page) {
        const std::string_view swipe =
            target.page == ShelfPage::Top ? ShopGoodsSwipeToTopTask : ShopGoodsSwipeToBottomTask;
        if (!run_goods_swipe(swipe)) {
            return false;
        }
        m_shop_shelf_page = target.page;
    }
    // 购买只会把原卡片置灰，不会移除或补位；在刷新前直接复用首次扫描坐标。
    selected = StoreSelection { target.good, target.page, wallet };
    return true;
}

bool BlackFlowAutomationStoreTaskPlugin::select_scrap_shop_good(std::optional<StoreSelection>& selected)
{
    selected.reset();
    const cv::Mat image = ctrler()->get_image();
    const auto goods = recognize(image, std::string(ScrapShopBuyGoodsTask));
    const std::optional<int> wallet = read_optional_number(image, std::string(ScrapShopWalletTask));

    std::vector<StoreGoodOffer> offers;
    offers.reserve(goods.size());
    for (const TextRect& good : goods) {
        const std::string_view name = normalized_good_name(good.text);
        const ShelfSlot slot { good.rect, ShelfPage::Top };
        const std::optional<int> price = read_good_price(image, good.rect);
        const std::optional<bool> shelf_buyable = shop_good_is_buyable(image, good.rect);
        const std::size_t entry_usable_count =
            usable_processing_item_count(name, m_scrap_shop_entry_processing_items);
        const std::size_t purchased_count = std::ranges::count(m_scrap_shop_purchased_names, name);
        offers.emplace_back(StoreGoodOffer {
            name,
            price,
            board_vine_purchase_allowed(AutomationStoreKind::Secret, name, m_scrap_shop_has_board_vine) &&
                automation_store_purchase_quota_allowed(name, entry_usable_count, purchased_count) &&
                !already_recorded(slot, m_scrap_shop_purchased) &&
                purchase_name_attempt_allowed(name, m_scrap_shop_buy_attempted_names),
            shelf_buyable,
        });
        Log.info(
            "BlackFlow automation 秘境行商商品",
            name,
            "价格",
            price.has_value() ? std::to_string(*price) : std::string("未识别"),
            "源石锭",
            wallet.has_value() ? std::to_string(*wallet) : std::string("未识别"),
            "进节点时可用同类加工品",
            entry_usable_count,
            "本节点已买",
            purchased_count,
            "卡片可购买",
            shelf_buyable.has_value() ? (*shelf_buyable ? "是" : "否") : "未确认");
    }

    const auto preferred = select_preferred_affordable_good(ScrapShopBuyPriority, offers, wallet);
    if (preferred.has_value()) {
        StoreSelection relocated { goods[*preferred], ShelfPage::Top, wallet };
        if (!relocate_selection(relocated, ScrapShopBuyGoodsTask)) {
            Log.warn(
                "BlackFlow automation 秘境行商商品重新定位失败，沿用扫描坐标",
                normalized_good_name(relocated.good.text));
        }
        selected = std::move(relocated);
    }
    return true;
}

bool BlackFlowAutomationStoreTaskPlugin::relocate_selection(
    StoreSelection& selection,
    std::string_view recognition_task)
{
    const std::string target_name(normalized_good_name(selection.good.text));
    constexpr int RetryTimes = 3;
    constexpr int RetryInterval = 250;

    for (int retry = 0; retry < RetryTimes; ++retry) {
        if (retry > 0) {
            sleep(RetryInterval);
        }
        const auto current_goods = recognize(ctrler()->get_image(), std::string(recognition_task));
        const auto closest = std::ranges::min_element(current_goods, {}, [&](const TextRect& candidate) {
            if (normalized_good_name(candidate.text) != target_name) {
                return std::numeric_limits<long long>::max();
            }
            const long long dx = static_cast<long long>(candidate.rect.x) - selection.good.rect.x;
            const long long dy = static_cast<long long>(candidate.rect.y) - selection.good.rect.y;
            return dx * dx + dy * dy;
        });
        if (closest != current_goods.end() && normalized_good_name(closest->text) == target_name) {
            if (!same_shelf_slot(selection.good.rect, closest->rect)) {
                Log.info(
                    "BlackFlow automation 商品点击坐标已按当前货架重定位",
                    target_name,
                    "旧坐标",
                    selection.good.rect,
                    "新坐标",
                    closest->rect);
            }
            selection.good = *closest;
            return true;
        }
    }
    return false;
}

bool BlackFlowAutomationStoreTaskPlugin::run_goods_swipe(std::string_view task)
{
    return ProcessTask(*this, { std::string(task) }).set_retry_times(0).run();
}

void BlackFlowAutomationStoreTaskPlugin::reset_shop_resume_task() const
{
    Task.set_task_base(std::string(ShopResumeAction), std::string(automation_shop_resume_fallback_base_task()));
}
} // namespace asst::blackflow
