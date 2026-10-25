#pragma once

#include "Task/Roguelike/BlackFlow/BlackFlowCollectionPopup.h"
#include "Task/Roguelike/BlackFlow/BlackFlowCollectionPopupTaskPlugin.h"
#include "Task/Interface/RoguelikeTask.h"
#include "Task/Roguelike/BlackFlow/BlackFlowAutomationStoreRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowAutomationStoreTaskPlugin.h"
#include "Task/Roguelike/BlackFlow/BlackFlowMovementTaskPlugin.h"

namespace asst::blackflow
{
struct BlackFlowStoreReplayAccess
{
    static void sand_table_policy(BlackFlowSession& session, bool disabled)
    {
        session.m_sand_table_beta_purchase_disabled = disabled;
    }
    static auto goods(BlackFlowAutomationStoreTaskPlugin& p, const cv::Mat& image, const std::string& task)
    {
        return p.recognize(image, task);
    }
    static auto name_task(BlackFlowAutomationStoreTaskPlugin& p, const std::string& task)
    {
        return p.recognition_task_info(task);
    }

    static void cache_shelf(BlackFlowAutomationStoreTaskPlugin& p, const cv::Mat& image)
    {
        p.invalidate_shop_goods();
        p.m_shop_shelf_page = BlackFlowAutomationStoreTaskPlugin::ShelfPage::Top;
        p.m_shop_goods_scanned = true;
        p.m_shop_goods_images[0] = std::make_shared<cv::Mat>(image.clone());
        for (const auto& good : p.recognize(image, "BlackFlow@Roguelike@AutomationShopGoods")) {
            p.m_shop_goods.push_back({good, BlackFlowAutomationStoreTaskPlugin::ShelfPage::Top, 12, true});
        }
        require(!p.m_shop_goods.empty(), "captured shelf has no cacheable name anchors");
    }
    static bool cache_matches(BlackFlowAutomationStoreTaskPlugin& p, const cv::Mat& image)
    { return p.shop_goods_cache_matches(image); }
    static bool cache_valid(const BlackFlowAutomationStoreTaskPlugin& p) { return p.m_shop_goods_scanned; }
    static Rect first_cached_name(const BlackFlowAutomationStoreTaskPlugin& p) { return p.m_shop_goods.front().good.rect; }

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

    static int paid_refresh_count(const BlackFlowAutomationStoreTaskPlugin& p) { return p.m_shop_paid_refresh_count; }

    static bool confirm_fruit_sale(BlackFlowAutomationStoreTaskPlugin& p)
    {
        p.m_pending_sale_name = "无缘果";
        p.m_pending = BlackFlowAutomationStoreTaskPlugin::PendingWork::ShopSellConfirmed;
        return p.run();
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
    static std::pair<bool, std::size_t> empty_slot(BlackFlowMovementTaskPlugin& p, const cv::Mat& image)
    {
        BlackFlowMovementTaskPlugin::InventoryFrame frame;
        std::string error;
        require(
            p.analyze_inventory_frame(image, frame, 0, &error) ==
                BlackFlowMovementTaskPlugin::InventoryAnalysisOutcome::Recognized,
            "captured inventory frame not recognized");
        return { frame.has_empty_slot, frame.items.size() };
    }

    static std::vector<TextRect> names(BlackFlowMovementTaskPlugin& p, const cv::Mat& image)
    {
        BlackFlowMovementTaskPlugin::InventoryFrame frame;
        std::string error;
        require(
            p.analyze_inventory_frame(image, frame, 0, &error) ==
                BlackFlowMovementTaskPlugin::InventoryAnalysisOutcome::Recognized,
            error.empty() ? "inventory names not recognized" : error);
        return frame.visible_names;
    }

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
    for (const bool disabled : { false, true }) {
        test(("sand table beta policy is logged and survives merchant resets " + std::to_string(disabled)).c_str(), [&] {
            auto session = std::make_shared<blackflow::BlackFlowSession>();
            std::string error; require(session->initialize("automation_collection", &error), error);
            blackflow::BlackFlowStoreReplayAccess::sand_table_policy(*session, disabled);
            auto config = std::make_shared<RoguelikeConfig>();
            blackflow::BlackFlowAutomationStoreTaskPlugin store { {}, nullptr, "Roguelike", config, nullptr, session, nullptr };
            const auto revision = session->run_revision();
            for (int visit = 0; visit < 3; ++visit) {
                store.reset_in_run_variables();
                require(session->sand_table_beta_purchase_disabled() == disabled && session->run_revision() == revision,
                        "merchant reset redrew the run purchase policy");
                const auto state = session->run_log_state();
                const auto& policy = state.at("store_purchase_policy").at("sand_table_beta");
                require(policy.at("disabled_for_run").as_boolean() == disabled, "logged policy differs from enforced policy");
                require(policy.at("disable_probability").as_double() == 0.5, "wrong disable probability");
                require(disabled ? policy.at("quota_override").as_integer() == 0 : policy.at("quota_override").is_null(),
                        "default quota was replaced or zero override was lost");
            }
            const blackflow::BlackFlowSession copied(*session);
            require(copied.sand_table_beta_purchase_disabled() == disabled, "session copy lost the run policy");
            std::ofstream policy_output("sand-table-policy-" + std::to_string(disabled) + ".json");
            policy_output << session->run_log_state().at("store_purchase_policy").to_string();
            session->reset_run();
            require(session->run_revision() == revision + 1, "new run did not reinitialize its policy state");
            const auto state = session->run_log_state();
            require(state.at("store_purchase_policy").at("sand_table_beta").at("disabled_for_run").as_boolean() ==
                        session->sand_table_beta_purchase_disabled(), "new run policy was not recorded");
        });
    }
    test("all declared BlackFlow item close templates have a before-click capture handler", [&] {
        const auto source = json::open(repo / "resource/tasks/Roguelike/BlackFlow.json");
        require(source.has_value(), "BlackFlow task resource missing");
        int checked = 0;
        for (const auto& [name, value] : source->as_object()) {
            const auto task = Task.get<MatchTaskInfo>(name);
            if (task == nullptr || (task->action != ProcessTaskAction::ClickSelf && task->action != ProcessTaskAction::ClickRect)) continue;
            const bool item_close = std::ranges::any_of(task->templ_names, [](const std::string& path) {
                return path.ends_with("BlackFlow@Roguelike@CloseCollection.png") ||
                       path.ends_with("BlackFlow@Roguelike@CloseCollectionContinue.png");
            });
            if (!item_close) continue;
            ++checked;
            // These two aliases are closed only after the parent has persisted
            // report_sacrifice("civilization-popup"). Keep that specialized
            // evidence instead of inventing a second generic item reward.
            if (name == "BlackFlow@Roguelike@SacrificePopupClose" ||
                name == "BlackFlow@Roguelike@SacrificePopupContinue") continue;
            require(blackflow::collection_popup_button(name).has_value(), "unhandled item close: " + name);
        }
        require(checked >= 4, "item-close audit did not cover the declared aliases");
    });
    test("nested roguelike ProcessTasks inherit the popup evidence guard", [&] {
        struct Probe : RoguelikeTask {
            Probe() : RoguelikeTask({}, nullptr) {}
            std::shared_ptr<AbstractTask> root() const { return m_subtasks.at(0); }
        } task;
        const auto root = task.root();
        require(root->find_plugin<blackflow::BlackFlowCollectionPopupTaskPlugin>() != nullptr, "root popup observer missing");
        for (const auto& plugin : root->get_plugins()) {
            if (std::dynamic_pointer_cast<blackflow::BlackFlowCollectionPopupTaskPlugin>(plugin) != nullptr) continue;
            ProcessTask nested(*plugin, { "BlackFlow@Roguelike@CloseCollection" });
            require(nested.find_plugin<blackflow::BlackFlowCollectionPopupTaskPlugin>() != nullptr,
                    "nested ProcessTask bypasses pre-close capture");
        }
    });
    for (const auto store : { std::string("Shop"), std::string("Cultivate") }) {
        test(("merchant optional sale reward is dispatched before another shop action " + store).c_str(), [&] {
            const auto image = MAA_NS::imread(repo / "unit_test/MaaCore/fixtures/blackflow-encounter/20261024/cargo-reward.jpg");
            auto tasks = Task.get("BlackFlow@Roguelike@Automation" + store + "SellConfirm")->next;
            bool found = false;
            for (int step = 0; step < 20; ++step) {
                PipelineAnalyzer analyzer(image); analyzer.set_tasks(tasks);
                const auto hit = analyzer.analyze();
                require(hit.has_value(), "sale reward route lost the visible popup");
                if (blackflow::collection_popup_button(hit->task_ptr->name).has_value()) { found = true; break; }
                require(hit->task_ptr->name != "BlackFlow@Roguelike@Automation" + store + "SellDecision",
                        "sale decision bypassed the visible reward popup");
                require(hit->task_ptr->action == ProcessTaskAction::DoNothing, "sale route issued input before inspecting reward");
                tasks = hit->task_ptr->next;
            }
            require(found, "sale reward route never reached a captured close task");
        });
        test(("merchant sale without any popup resumes on the actual selling page " + store).c_str(), [&] {
            const auto image = MAA_NS::imread(repo / "unit_test/MaaCore/fixtures/blackflow-merchant/20261024/eerie-sale-after.jpg");
            auto tasks = Task.get("BlackFlow@Roguelike@Automation" + store + "SellConfirm")->next;
            bool ready = false;
            for (int step = 0; step < 8; ++step) {
                PipelineAnalyzer analyzer(image); analyzer.set_tasks(tasks);
                const auto hit = analyzer.analyze();
                require(hit.has_value(), "normal sale page cannot resume");
                require(!blackflow::collection_popup_button(hit->task_ptr->name).has_value(), "invented a popup on a selling page");
                if (hit->task_ptr->name == "BlackFlow@Roguelike@Automation" + store + "SellDecision") { ready = true; break; }
                require(hit->task_ptr->action == ProcessTaskAction::DoNothing, "sale observation clicked the shop");
                tasks = hit->task_ptr->next;
            }
            require(ready, "sale without a reward remained waiting for a nonexistent popup");
        });
        test(("merchant chained reward popups return to the same sale decision " + store).c_str(), [&] {
            const auto popup = MAA_NS::imread(repo / "unit_test/MaaCore/fixtures/blackflow-encounter/20261024/cargo-reward.jpg");
            const auto merchant = MAA_NS::imread(repo / "unit_test/MaaCore/fixtures/blackflow-merchant/20261024/eerie-sale-after.jpg");
            auto tasks = Task.get("BlackFlow@Roguelike@Automation" + store + "SellConfirm")->next;
            int closes = 0; bool ready = false;
            for (int step = 0; step < 16; ++step) {
                PipelineAnalyzer analyzer(closes < 2 ? popup : merchant); analyzer.set_tasks(tasks);
                const auto hit = analyzer.analyze(); require(hit.has_value(), "chained sale rewards lost their continuation");
                if (blackflow::collection_popup_button(hit->task_ptr->name).has_value()) ++closes;
                else if (hit->task_ptr->name == "BlackFlow@Roguelike@Automation" + store + "SellDecision") { ready = true; break; }
                else require(hit->task_ptr->action == ProcessTaskAction::DoNothing, "sale resumed input before draining rewards");
                tasks = hit->task_ptr->next;
            }
            require(ready && closes == 2, "chained popups did not resume the same sale flow");
        });
    }
    test("merchant cache survives a stationary rejected target and invalidates on shelf changes", [&] {
        const auto image = MAA_NS::imread(repo / "unit_test/MaaCore/fixtures/blackflow-recovery/20261024/eerie-shelf.jpg");
        auto config = std::make_shared<RoguelikeConfig>();
        blackflow::BlackFlowAutomationStoreTaskPlugin store { {}, nullptr, "Roguelike", config, nullptr, nullptr, nullptr };
        using A = blackflow::BlackFlowStoreReplayAccess;
        A::cache_shelf(store, image);
        require(A::cache_matches(store, image), "stationary shelf always forces another full scan");
        auto hud_changed = image.clone();
        hud_changed(cv::Rect(0, 0, 100, 70)).setTo(cv::Scalar::all(0));
        require(A::cache_matches(store, hud_changed), "unrelated HUD invalidates shelf");
        auto renamed = image.clone(); const auto name = A::first_cached_name(store);
        renamed(cv::Rect(name.x, name.y, name.width, name.height)).setTo(cv::Scalar::all(0));
        require(!A::cache_matches(store, renamed), "changed item retained old shelf");
        auto repriced = image.clone(); const auto price = blackflow::merchant_price_roi(name);
        repriced(cv::Rect(price.x, price.y, price.width, price.height)).setTo(cv::Scalar::all(0));
        require(!A::cache_matches(store, repriced), "changed price retained old shelf");
        auto new_card = image.clone();
        new_card(cv::Rect(1100, 170, 30, 20)).setTo(cv::Scalar(255, 0, 255));
        require(!A::cache_matches(store, new_card), "changed unrecognized region retained shelf");
        store.reset_in_run_variables();
        require(!A::cache_valid(store) && !A::cache_matches(store, image), "new run retained shelf");
    });
    test("inventory early reset accepts stationary duplicate items but rejects motion and missing observations", [&] {
        const auto image = MAA_NS::imread(repo / "unit_test/MaaCore/fixtures/blackflow-inventory/20261024/instance003-left-edge.jpg");
        auto config = std::make_shared<RoguelikeConfig>();
        blackflow::BlackFlowMovementTaskPlugin movement { {}, nullptr, "Roguelike", config, nullptr, nullptr, nullptr };
        const auto names = blackflow::BlackFlowMovementReplayAccess::names(movement, image);
        require(std::ranges::count_if(names, [](const TextRect& name) { return name.text == "标准引擎"; }) == 2,
                "captured duplicate engines were not recognized as separate items");
        require(blackflow::inventory_reset_can_confirm_early(image, image, names, names), "stationary duplicate inventory cannot confirm origin early");
        auto shifted = names; shifted.front().rect.x += 10;
        require(!blackflow::inventory_reset_can_confirm_early(image, image, names, shifted), "partial swipe mistaken for origin");
        auto changed = image.clone(); changed(cv::Rect(360, 140, 430, 150)).setTo(cv::Scalar::all(0));
        require(!blackflow::inventory_reset_can_confirm_early(image, changed, names, names), "unchanged names conceal changed cards");
        auto rebound = image.clone();
        image(cv::Rect(360,140,850,475)).copyTo(rebound(cv::Rect(370,140,850,475)));
        require(!blackflow::inventory_reset_can_confirm_early(image, rebound, names, names), "same labels concealed a rebounding duplicate column");
        require(!blackflow::inventory_reset_can_confirm_early({}, image, names, names), "missing image confirms origin");
        require(!blackflow::inventory_reset_can_confirm_early(image, image, {}, {}), "empty OCR confirms origin");
    });
    test("complete shelf anchors include goods outside the purchase vocabulary", [&] {
        const auto image = MAA_NS::imread(repo / "unit_test/MaaCore/fixtures/blackflow-recovery/shop-refresh-not-open.jpg");
        auto config = std::make_shared<RoguelikeConfig>();
        blackflow::BlackFlowAutomationStoreTaskPlugin store { {}, nullptr, "Roguelike", config, nullptr, nullptr, nullptr };
        const auto anchors = blackflow::BlackFlowStoreReplayAccess::goods(
            store, image, "BlackFlow@Roguelike@AutomationShopCaptureAnchors");
        require(!anchors.empty() && std::ranges::any_of(anchors, [](const TextRect& row) { return row.text.find("价格") != std::string::npos; }),
                "purchase identity filtering discarded the complete-shelf anchors");
    });
    test("captured secret shelf preserves unpurchased identities before filtering purchases", [&] {
        const auto image =
            MAA_NS::imread(repo / "unit_test/MaaCore/fixtures/blackflow-merchant/20261024/secret-shelf.jpg");
        require(!image.empty(), "secret shelf fixture missing");
        auto config = std::make_shared<RoguelikeConfig>();
        blackflow::BlackFlowAutomationStoreTaskPlugin store { { },     nullptr, "Roguelike", config,
                                                              nullptr, nullptr, nullptr };
        const auto goods = blackflow::BlackFlowStoreReplayAccess::goods(
            store,
            image,
            "BlackFlow@Roguelike@AutomationCultivateBuyItems");
        int seeds = 0, seed_pods = 0, blood_mushrooms = 0;
        std::vector<blackflow::StoreGoodOffer> declined_offers;
        for (const auto& item : goods) {
            require(item.text != "血橙", "blood mushroom became a purchasable blood orange");
            if (item.text == "种荚" || item.text == "血蕈") {
                seed_pods += item.text == "种荚";
                blood_mushrooms += item.text == "血蕈";
                declined_offers.push_back({item.text, 1, true, true});
            }
            if (item.text == "种子") {
                ++seeds;
                require(item.rect.x >= 1000 && item.rect.y < 300, "seed moved to pod card");
            }
        }
        require(seeds == 1, "actual seed missing or duplicated by fuzzy alias");
        require(seed_pods == 1 && blood_mushrooms == 1, "full recognition lost unpurchased item identities");
        require(!blackflow::select_preferred_affordable_good(blackflow::ScrapShopBuyPriority, declined_offers, 100),
                "recognized but unselected items became purchase candidates");
        OCRer legacy(image);
        legacy.set_task_info("BlackFlow@Roguelike@AutomationCultivateBuyItems");
        // Deliberately omit the two identities to retain the original alias regression,
        // independently of future changes to the full production vocabulary.
        auto incomplete = Task.get<OcrTaskInfo>("BlackFlow@Roguelike@AutomationCultivateBuyItems")->text;
        std::erase(incomplete, "种荚");
        std::erase(incomplete, "血蕈");
        legacy.set_required(std::move(incomplete));
        require(legacy.analyze().has_value(), "fixture no longer reproduces original recognition");
        const auto old = legacy.get_result();
        require(
            std::ranges::count_if(old, [](const auto& item) { return item.text == "种子"; }) == 2 &&
                std::ranges::any_of(old, [](const auto& item) { return item.text == "血橙"; }),
            "fixture no longer reproduces original alias");
    });
    using namespace asst::blackflow;
    const auto fixtures = repo / "unit_test/MaaCore/fixtures/blackflow-recovery/20260926";
    auto read = [&](const char* name) {
        auto image = MAA_NS::imread(fixtures / name);
        require(!image.empty(), name);
        return image;
    };
    auto config = std::make_shared<RoguelikeConfig>();
    auto session = std::make_shared<BlackFlowSession>();
    BlackFlowAutomationStoreTaskPlugin store({ }, nullptr, "Roguelike", config, nullptr, session, nullptr);
    BlackFlowMovementTaskPlugin movement({ }, nullptr, "Roguelike", config, nullptr, session, nullptr);
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
    std::vector<TextRect> previous_names;
    for (const char* file : { "instance001-first-page.jpg", "instance003-first-page.jpg" }) {
        test((std::string("captured reset first page retains the inventory guard ") + file).c_str(), [&] {
            const auto image = MAA_NS::imread(repo / "unit_test/MaaCore/fixtures/blackflow-inventory/20261022" / file);
            require(!image.empty(), "captured reset first page missing");
            require(movement_inventory_page_visible(image), "captured inventory page lost its contents guard");
            Matcher collapse(image);
            collapse.set_task_info("BlackFlow@Roguelike@MovementInventoryResetCollapse");
            require(collapse.analyze().has_value(), "captured inventory page cannot use the guarded collapse task");
            require(!BlackFlowMovementReplayAccess::names(movement, image).empty(), "captured inventory names missing");
        });
    }
    int stationary = 0;
    for (int index = 1; index <= 9; ++index) {
        test(("captured inventory scan with loaded highlight page " + std::to_string(index)).c_str(), [&] {
            const auto frame = MAA_NS::imread(
                repo / "unit_test/MaaCore/fixtures/blackflow-inventory/20261021" /
                ("page-" + std::to_string(index) + ".jpg"));
            require(!frame.empty(), "captured inventory frame missing");
            const auto current_names = BlackFlowMovementReplayAccess::names(movement, frame);
            const bool unchanged = inventory_name_layout_unchanged(previous_names, current_names);
            stationary = unchanged ? stationary + 1 : 0;
            if (index <= 4) {
                require(!unchanged, "moving inventory accepted as stationary");
            }
            if (index >= 6) {
                require(stationary >= 2, "loaded animation reset the completed inventory tail");
            }
            previous_names = current_names;
        });
    }
    const auto empty_fixtures = repo / "unit_test/MaaCore/fixtures/blackflow-inventory/20261021-instance002";
    test("captured 002 inventory first page recognizes empty slots beside two movements", [&] {
        const auto image = MAA_NS::imread(empty_fixtures / "first-page.jpg");
        require(!image.empty(), "captured first inventory page missing");
        const auto [empty, movements] = BlackFlowMovementReplayAccess::empty_slot(movement, image);
        require(empty && movements == 2, "empty terminal or same-page movements were not recognized");
    });
    test("captured 002 empty tail recognizes an explicit terminal", [&] {
        const auto image = MAA_NS::imread(empty_fixtures / "empty-tail.jpg");
        require(!image.empty(), "captured empty tail missing");
        const auto [empty, movements] = BlackFlowMovementReplayAccess::empty_slot(movement, image);
        require(empty && movements == 0, "empty tail not recognized");
    });
    test("removing the captured empty labels removes the explicit terminal", [&] {
        auto image = MAA_NS::imread(empty_fixtures / "first-page.jpg");
        require(!image.empty(), "captured first inventory page missing");
        image(cv::Rect(930, 365, 175, 215)).setTo(cv::Scalar(25, 25, 25));
        const auto [empty, movements] = BlackFlowMovementReplayAccess::empty_slot(movement, image);
        require(!empty && movements == 2, "missing empty labels became terminal evidence");
    });
}
