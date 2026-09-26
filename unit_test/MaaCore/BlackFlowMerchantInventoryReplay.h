#pragma once
#include "Task/Roguelike/BlackFlow/BlackFlowAutomationStoreRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowAutomationStoreTaskPlugin.h"
#include "Task/Roguelike/BlackFlow/BlackFlowMovementTaskPlugin.h"

namespace asst::blackflow
{
struct BlackFlowStoreReplayAccess
{
    static auto wallet(BlackFlowAutomationStoreTaskPlugin& p, const cv::Mat& image)
    {
        return p.read_optional_number(image, "BlackFlow@Roguelike@StageTraderInvest-Wallet");
    }

    static auto price(BlackFlowAutomationStoreTaskPlugin& p, const cv::Mat& image, Rect name)
    {
        return p.read_good_price(image, name);
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
