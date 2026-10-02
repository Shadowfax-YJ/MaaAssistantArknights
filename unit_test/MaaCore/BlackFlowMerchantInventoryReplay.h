#pragma once
#include "Task/Roguelike/BlackFlow/BlackFlowAutomationStoreRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowAutomationStoreTaskPlugin.h"
#include "Task/Roguelike/BlackFlow/BlackFlowMovementTaskPlugin.h"

namespace asst::blackflow
{
struct BlackFlowStoreReplayAccess
{
    static auto identity(BlackFlowAutomationStoreTaskPlugin& p, AutomationStoreKind kind)
    {
        return p.current_store_identity(kind);
    }

    // Seed previously verified payments; entering/re-entering still runs the actual plugin callback.
    static void prior_refreshes(BlackFlowAutomationStoreTaskPlugin& p, AutomationStoreKind kind, int count)
    {
        const auto identity = p.current_store_identity(kind);
        for (int i = 0; i < count; ++i) {
            p.m_refresh_ledger.record_refresh(identity);
        }
    }

    static int refresh_count(const BlackFlowAutomationStoreTaskPlugin& p, AutomationStoreKind kind)
    {
        return kind == AutomationStoreKind::Eerie ? p.m_shop_refresh_count : p.m_scrap_shop_refresh_count;
    }

    static int queued_refresh_index(const BlackFlowAutomationStoreTaskPlugin& p)
    {
        return p.m_pending_eerie_store_snapshot.has_value() ? p.m_pending_eerie_store_snapshot->second : -1;
    }

    static auto wallet(BlackFlowAutomationStoreTaskPlugin& p, const cv::Mat& image)
    {
        return p.read_optional_number(image, "BlackFlow@Roguelike@StageTraderInvest-Wallet");
    }

    static auto price(BlackFlowAutomationStoreTaskPlugin& p, const cv::Mat& image, Rect name)
    {
        return p.read_good_price(image, name);
    }

    static bool wallet_page(BlackFlowAutomationStoreTaskPlugin& p, const cv::Mat& image)
    {
        return p.purchase_wallet_page(image, AutomationStoreKind::Eerie);
    }

    static bool sold(BlackFlowAutomationStoreTaskPlugin& p, const cv::Mat& before, const cv::Mat& after)
    {
        p.m_pending_purchase_name = "小八界";
        p.m_pending_purchase_image = std::make_shared<cv::Mat>(before.clone());
        p.m_pending_purchase =
            BlackFlowAutomationStoreTaskPlugin::ShelfSlot { Rect { 418, 389, 67, 26 },
                                                            BlackFlowAutomationStoreTaskPlugin::ShelfPage::Top };
        return p.purchased_good_sold_out(after, AutomationStoreKind::Secret);
    }
};

struct BlackFlowMovementReplayAccess
{
    static bool rejects_map(BlackFlowMovementTaskPlugin& p, const cv::Mat& image)
    {
        BlackFlowMovementTaskPlugin::InventoryFrame frame;
        std::string error;
        return p.analyze_inventory_frame(image, frame, 0, &error) ==
               BlackFlowMovementTaskPlugin::InventoryAnalysisOutcome::Failed;
    }
};
}

template <class Test>
void merchant_inventory_regressions(const std::filesystem::path& repo, Test&& test)
{
    using namespace asst;
    using namespace asst::blackflow;
    const auto fixtures = repo / "unit_test/MaaCore/fixtures/blackflow-recovery/20260926";
    auto read = [&](const char* name) {
        auto image = MAA_NS::imread(fixtures / name);
        require(!image.empty(), name);
        return image;
    };
    auto config = std::make_shared<RoguelikeConfig>();
    auto session = std::make_shared<BlackFlowSession>();
    BlackFlowAutomationStoreTaskPlugin store({}, nullptr, "Roguelike", config, nullptr, session, nullptr);
    BlackFlowMovementTaskPlugin movement({}, nullptr, "Roguelike", config, nullptr, session, nullptr);
    const auto refresh_fixtures = repo / "unit_test/MaaCore/fixtures/blackflow-recovery/20260929";
    auto refresh_frame = [&](const char* name) {
        auto image = MAA_NS::imread(refresh_fixtures / name);
        require(!image.empty(), name);
        return image;
    };
    const auto refresh_dialog =
        MAA_NS::imread(repo / "unit_test/MaaCore/fixtures/blackflow-recovery/20260930/refresh-confirm-wallet-16.jpg");
    require(!refresh_dialog.empty(), "captured refresh dialog missing");
    const std::string refresh_confirm = "BlackFlow@Roguelike@AutomationShopRefreshConfirm";
    auto confirm_visible = [&](const cv::Mat& image) {
        Matcher matcher(image);
        matcher.set_task_info(refresh_confirm);
        return matcher.analyze().has_value();
    };
    for (const auto* name : { "refresh-paid-wallet-18.jpg", "refresh-paid-final.jpg" }) {
        test(name, [&] {
            const auto image = refresh_frame(name);
            require(!confirm_visible(image), "shelf matched a refresh confirmation dialog");
            require(BlackFlowStoreReplayAccess::wallet_page(store, image), "paid shop rejected");
            require(BlackFlowStoreReplayAccess::wallet(store, image) == 18, "26 - 8 receipt unreadable");
        });
    }
    test("paid shelf follows the refresh button rather than falsely confirming payment", [&] {
        PipelineAnalyzer analyzer(refresh_frame("refresh-paid-wallet-18.jpg"));
        const auto refresh = Task.get("BlackFlow@Roguelike@AutomationShopRefresh");
        analyzer.set_tasks(refresh->next);
        const auto result = analyzer.analyze();
        require(result && result->task_ptr->name == refresh->name, "shelf routed to payment confirmation");
    });
    test("captured refresh confirmation is recognized before payment", [&] {
        require(confirm_visible(refresh_dialog), "captured refresh confirmation rejected");
        require(!BlackFlowStoreReplayAccess::wallet_page(store, refresh_dialog), "unpaid dialog accepted as a shelf");
    });
    test("opened refresh dialog routes to confirmation through the production pipeline", [&] {
        PipelineAnalyzer analyzer(refresh_dialog);
        analyzer.set_tasks(Task.get("BlackFlow@Roguelike@AutomationShopRefresh")->next);
        const auto result = analyzer.analyze();
        require(result && result->task_ptr->name == refresh_confirm, "opened dialog did not route to confirmation");
    });
    test("inventory-full confirmation is not a refresh confirmation", [&] {
        require(!confirm_visible(refresh_frame("shared-confirm-dialog.jpg")), "inventory prompt accepted for refresh");
    });
    test("compressed confirmation dialog remains recognizable", [&] {
        std::vector<uchar> jpeg;
        require(cv::imencode(".jpg", refresh_dialog, jpeg, { cv::IMWRITE_JPEG_QUALITY, 45 }), "JPEG encoding failed");
        require(confirm_visible(cv::imdecode(jpeg, cv::IMREAD_COLOR)), "compressed confirmation rejected");
    });
    test("refresh confirmation tolerates moderate brightness variation", [&] {
        for (const double gain : { 0.8, 1.15 }) {
            cv::Mat image;
            refresh_dialog.convertTo(image, -1, gain);
            require(confirm_visible(image), "brightness variation rejected refresh confirmation");
        }
    });
    test("confirmation row without its cancel button is rejected", [&] {
        auto image = refresh_dialog.clone();
        image(cv::Rect(285, 455, 130, 61)).setTo(cv::Scalar(60, 60, 60));
        require(!confirm_visible(image), "missing cancel button accepted");
    });
    test("confirmation row without its confirm button is rejected", [&] {
        auto image = refresh_dialog.clone();
        image(cv::Rect(928, 455, 133, 61)).setTo(cv::Scalar(95, 158, 112));
        require(!confirm_visible(image), "missing confirm button accepted");
    });
    test("a green check button alone cannot establish a confirmation dialog", [&] {
        auto image = refresh_frame("refresh-paid-wallet-18.jpg");
        const auto button =
            MAA_NS::imread(repo / "resource/template/Roguelike/base/Roguelike@StageTraderRefreshConfirmNew.png");
        require(!button.empty(), "button fixture missing");
        button.copyTo(image(cv::Rect(800, 458, button.cols, button.rows)));
        require(!confirm_visible(image), "isolated check button accepted without the dialog row");
    });
    test("confirmation row outside its expected position is rejected", [&] {
        auto image = refresh_frame("refresh-paid-wallet-18.jpg");
        refresh_dialog(cv::Rect(278, 454, 792, 66)).copyTo(image(cv::Rect(338, 454, 792, 66)));
        require(!confirm_visible(image), "shifted confirmation row accepted");
    });
    test("legacy red confirmation button remains recognizable", [&] {
        auto image = refresh_frame("refresh-paid-wallet-18.jpg");
        const auto button =
            MAA_NS::imread(repo / "resource/template/Roguelike/base/Roguelike@StageTraderRefreshConfirm.png");
        require(!button.empty(), "legacy button missing");
        button.copyTo(image(cv::Rect(640, 458, button.cols, button.rows)));
        require(confirm_visible(image), "legacy confirmation rejected");
    });
    test("confirmation click is restricted to the right-hand button", [&] {
        const auto confirm = Task.get(refresh_confirm);
        require(confirm->action == ProcessTaskAction::ClickRect, "dialog row may click cancellation");
        const auto& click = confirm->specific_rect;
        require(
            click.x >= 640 && click.y >= 451 && click.x + click.width <= 1280 && click.y + click.height <= 522 &&
                click.width > 0 && click.height > 0,
            "confirmation click is outside the right-hand button");
    });
    for (const auto& [name, expected] :
         std::vector<std::pair<const char*, int>> { { "refresh-wallet-9.jpg", 9 },
                                                    { "refresh-wallet-17.jpg", 17 },
                                                    { "purchase-wallet-0.jpg", 0 },
                                                    { "purchase-wallet-7.jpg", 7 },
                                                    { "purchase-before-wallet-9.jpg", 9 },
                                                    { "purchase-after-wallet-5.jpg", 5 },
                                                    { "purchase-eerie-wallet-0.jpg", 0 } }) {
        test(name, [&] {
            require(BlackFlowStoreReplayAccess::wallet(store, read(name)) == expected, "incorrect wallet");
        });
    }
    test("xiaoba purchase debits 9 to 5 rather than reporting unchanged wallet", [&] {
        auto before = BlackFlowStoreReplayAccess::wallet(store, read("purchase-before-wallet-9.jpg"));
        auto after = BlackFlowStoreReplayAccess::wallet(store, read("purchase-after-wallet-5.jpg"));
        require(
            verify_store_purchase_evidence(true, before, after, 4, true).succeeded(),
            "real debit lost by wallet OCR");
    });
    test("wheel price 2 must not become 1", [&] {
        require(
            BlackFlowStoreReplayAccess::price(store, read("price-2.jpg"), { 831, 177, 86, 28 }) == 2,
            "incorrect price");
    });
    test("medic voucher price 4 must not become 1", [&] {
        require(
            BlackFlowStoreReplayAccess::price(store, read("price-4.jpg"), { 420, 182, 103, 24 }) == 4,
            "incorrect price");
    });
    test("two-digit prices retain their leading digit", [&] {
        auto image = read("price-4.jpg");
        require(BlackFlowStoreReplayAccess::price(store, image, { 625, 182, 86, 24 }) == 16, "16 truncated");
        require(BlackFlowStoreReplayAccess::price(store, image, { 425, 396, 106, 24 }) == 12, "12 truncated");
        require(
            BlackFlowStoreReplayAccess::price(store, image, { 1047, 182, 100, 24 }) == 16,
            "shifted name anchor truncated price");
    });
    test("sold-out selected slot survives obscured item name", [&] {
        require(
            BlackFlowStoreReplayAccess::sold(store, read("xiaoba-before.jpg"), read("xiaoba-after.jpg")),
            "sold item was lost");
    });
    test("neighbor sold-out slot cannot verify selected item", [&] {
        require(
            !BlackFlowStoreReplayAccess::sold(store, read("xiaoba-before.jpg"), read("xiaoba-before.jpg")),
            "unsold item accepted");
    });
    test("selected name obscured after purchase retains slot evidence", [&] {
        auto after = read("xiaoba-after.jpg");
        after(cv::Rect(408, 382, 200, 45)).setTo(cv::Scalar(0, 0, 0));
        require(
            BlackFlowStoreReplayAccess::sold(store, read("xiaoba-before.jpg"), after),
            "obscured title lost confirmed slot");
    });
    test("an already sold slot cannot establish another purchase when its title disappears", [&] {
        auto before = read("xiaoba-after.jpg");
        auto after = before.clone();
        after(cv::Rect(408, 382, 200, 45)).setTo(cv::Scalar(0, 0, 0));
        require(!BlackFlowStoreReplayAccess::sold(store, before, after), "old sold marker counted as a new purchase");
    });
    test("obscured title without unchanged shelf anchors is not accepted", [&] {
        auto after = read("xiaoba-after.jpg");
        after(cv::Rect(400, 159, 860, 70)).setTo(cv::Scalar(0, 0, 0));
        after(cv::Rect(400, 380, 860, 45)).setTo(cv::Scalar(0, 0, 0));
        require(
            !BlackFlowStoreReplayAccess::sold(store, read("xiaoba-before.jpg"), after),
            "unanchored shelf accepted");
    });
    test("inventory OCR rejects a closed map", [&] {
        require(
            BlackFlowMovementReplayAccess::rejects_map(movement, read("inventory-closed.jpg")),
            "map accepted as empty inventory");
    });
    test("inventory OCR rejects a closing transition", [&] {
        require(
            BlackFlowMovementReplayAccess::rejects_map(movement, read("inventory-closing.jpg")),
            "closing transition accepted");
    });
    test("open inventory remains recognizable", [&] {
        require(
            !BlackFlowMovementReplayAccess::rejects_map(movement, read("inventory-open.jpg")),
            "open inventory rejected");
    });
    test("close entry handles already-closed inventory", [&] {
        PipelineAnalyzer p(read("inventory-closed.jpg"));
        p.set_tasks(Task.get("BlackFlow@Roguelike@MovementInventoryClose-Enter")->next);
        auto result = p.analyze();
        require(
            result.has_value() && result->task_ptr->name == "BlackFlow@Roguelike@MapPrepare-Ready",
            "closed map has no successor");
    });
}
