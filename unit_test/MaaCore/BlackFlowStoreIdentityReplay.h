#pragma once

// Controlled two-node observations and prior paid counts, with production Session
// transactions, tree-hole entry/return and store-entry callbacks (no UI controller).
struct StoreIdentityScenario
{
    using Kind = asst::blackflow::AutomationStoreKind;
    std::shared_ptr<asst::RoguelikeConfig> config = std::make_shared<asst::RoguelikeConfig>();
    std::shared_ptr<asst::blackflow::BlackFlowSession> session = std::make_shared<asst::blackflow::BlackFlowSession>();
    asst::blackflow::BlackFlowAutomationStoreTaskPlugin store { { },     nullptr, "Roguelike", config,
                                                                nullptr, session, nullptr };
    int floor;
    int sequence = 0;

    explicit StoreIdentityScenario(int outer_floor = 5) :
        floor(outer_floor)
    {
        require(config->verify_and_load_params(json::object { { "theme", "BlackFlow" }, { "mode", 30002 } }), "config");
        require(session->initialize("automation_collection"), "session");
        require(session->set_current_floor(floor), "floor");
        observe(0);
    }

    void observe(int current)
    {
        auto snapshot = map_frame(floor, ++sequence, current, "portal");
        snapshot.observation.nodes[0].type = "shop";
        require(session->update(snapshot), "store identity map observation");
    }

    void enter_store(Kind kind = Kind::Eerie)
    {
        const std::string task = kind == Kind::Eerie ? "BlackFlow@Roguelike@AutomationShopEnter"
                                                     : "BlackFlow@Roguelike@AutomationCultivateEnter";
        require(
            store.verify(
                asst::AsstMsg::SubTaskStart,
                json::object { { "subtask", "ProcessTask" }, { "details", json::object { { "task", task } } } }),
            "store entry callback");
        require(store.run(), "store entry");
    }

    void prior_refreshes(Kind kind, int count)
    {
        asst::blackflow::BlackFlowStoreReplayAccess::prior_refreshes(store, kind, count);
        enter_store(kind);
    }

    void move_to(int target)
    {
        using namespace asst::blackflow;
        MoveCandidate move;
        move.action_id = "store-identity-" + std::to_string(sequence) + "-" + std::to_string(target);
        move.source = session->run().current_node;
        move.target = move.landing = *make_stable_node_id(floor, { 0, target });
        move.path = move.possible_landings = { move.target };
        move.landing_action_point_gains.emplace(move.target, 0);
        move.controllable = true;
        move.movement = MovementKind::Walk;
        move.predicted_action_point_cost = 1;
        require(session->begin_transaction(move), "store identity move");
        require(
            session->accept_preview({ PreviewReachability::Reachable, 1 }) == PreviewDisposition::ReadyToCommit,
            "store identity preview");
        require(session->commit(), "store identity commit");
    }

    void resolve_page()
    {
        require(session->mark_page_running(), "store identity page running");
        asst::blackflow::NodeTaskResult result;
        result.kind = asst::blackflow::NodeTaskResultKind::PageCompleted;
        result.succeeded = true;
        require(session->apply_node_task_result(result, json::object { }), "store identity page completed");
    }

    void enter_tree_hole()
    {
        move_to(1);
        require(session->mark_page_running(), "portal running");
        session->record_portal_choice("木屑");
        asst::blackflow::NodeTaskResult result;
        result.kind = asst::blackflow::NodeTaskResultKind::PageCompleted;
        result.succeeded = true;
        require(session->apply_node_task_result(result, json::object { }), "portal completed");
        require(session->set_current_floor(6), "tree-hole entry");
    }

    void return_to_shop(Kind kind = Kind::Eerie)
    {
        require(session->restore_outer_map(floor, nullptr), "outer map restoration");
        observe(1);
        move_to(0);
        enter_store(kind);
    }
};

template <class Test>
void store_identity_regressions(const std::filesystem::path& repo, Test&& test)
{
    using namespace asst;
    using namespace asst::blackflow;
    for (const int prior_count : { 1, 2 }) {
        const auto name = "outer shop retains " + std::to_string(prior_count) + " refreshes after a tree-hole return";
        test(name.c_str(), [&] {
            StoreIdentityScenario s;
            s.prior_refreshes(AutomationStoreKind::Eerie, prior_count);
            const auto before = BlackFlowStoreReplayAccess::identity(s.store, AutomationStoreKind::Eerie);
            const auto perception_generation = s.session->map_generation();
            s.enter_tree_hole();
            s.return_to_shop();
            const auto after = BlackFlowStoreReplayAccess::identity(s.store, AutomationStoreKind::Eerie);
            require(s.session->map_generation() > perception_generation, "perception generation must still advance");
            require(before == after, "same outer shop was assigned a new identity");
            const int count = BlackFlowStoreReplayAccess::refresh_count(s.store, AutomationStoreKind::Eerie);
            require(count == prior_count, "store entry lost paid refreshes");
            require(
                BlackFlowStoreReplayAccess::queued_refresh_index(s.store) == prior_count,
                "capture lost refresh index");
            require(
                automation_store_can_refresh(count, 46, 12) == (prior_count == 1),
                "wrong refresh budget after return");
            require(automation_store_refresh_price(count) == 4 * (prior_count + 1), "wrong payment expectation");
        });
    }
    test("secret merchant retains refreshes after a tree-hole return", [&] {
        StoreIdentityScenario s;
        s.prior_refreshes(AutomationStoreKind::Secret, 2);
        const auto before = BlackFlowStoreReplayAccess::identity(s.store, AutomationStoreKind::Secret);
        s.enter_tree_hole();
        s.return_to_shop(AutomationStoreKind::Secret);
        require(
            before == BlackFlowStoreReplayAccess::identity(s.store, AutomationStoreKind::Secret),
            "secret shop identity changed");
        require(
            BlackFlowStoreReplayAccess::refresh_count(s.store, AutomationStoreKind::Secret) == 2,
            "secret count lost");
    });
    test("another tree hole has a separate shop identity and preserves the outer shop", [&] {
        StoreIdentityScenario s;
        s.prior_refreshes(AutomationStoreKind::Eerie, 2);
        s.enter_tree_hole();
        s.floor = 6;
        s.observe(0);
        s.prior_refreshes(AutomationStoreKind::Eerie, 1);
        const auto first_child = BlackFlowStoreReplayAccess::identity(s.store, AutomationStoreKind::Eerie);
        s.floor = 5;
        s.return_to_shop();
        s.resolve_page();
        s.observe(0);
        s.enter_tree_hole();
        s.floor = 6;
        s.observe(0);
        s.enter_store();
        require(
            first_child != BlackFlowStoreReplayAccess::identity(s.store, AutomationStoreKind::Eerie),
            "distinct tree holes share an identity");
        require(
            BlackFlowStoreReplayAccess::refresh_count(s.store, AutomationStoreKind::Eerie) == 0,
            "child count leaked");
        s.floor = 5;
        s.return_to_shop();
        require(
            BlackFlowStoreReplayAccess::refresh_count(s.store, AutomationStoreKind::Eerie) == 2,
            "second return lost outer count");
    });
    test("floor-four remembrance does not inherit the same-coordinate shop count", [&] {
        StoreIdentityScenario s(4);
        s.prior_refreshes(AutomationStoreKind::Eerie, 2);
        const auto before = BlackFlowStoreReplayAccess::identity(s.store, AutomationStoreKind::Eerie);
        s.session->clear_current_floor();
        require(s.session->set_current_floor(4), "remembrance floor");
        s.observe(0);
        s.enter_store();
        const auto after = BlackFlowStoreReplayAccess::identity(s.store, AutomationStoreKind::Eerie);
        require(before.node == after.node && before != after, "new physical map reused old shop identity");
        require(
            BlackFlowStoreReplayAccess::refresh_count(s.store, AutomationStoreKind::Eerie) == 0,
            "remembrance count leaked");
    });
    test("a new run does not inherit paid shop refreshes", [&] {
        StoreIdentityScenario s;
        s.prior_refreshes(AutomationStoreKind::Eerie, 2);
        s.session->reset_run();
        s.store.reset_in_run_variables();
        require(s.session->set_current_floor(5), "new-run floor");
        s.observe(0);
        s.enter_store();
        require(
            BlackFlowStoreReplayAccess::refresh_count(s.store, AutomationStoreKind::Eerie) == 0,
            "prior-run count leaked");
        require(BlackFlowStoreReplayAccess::queued_refresh_index(s.store) == 0, "prior-run capture index leaked");
    });
    test("October 2 paid shelf has a readable wallet of 34", [&] {
        StoreIdentityScenario s;
        const auto image = MAA_NS::imread(
            repo / "unit_test/MaaCore/fixtures/blackflow-recovery/20261002/return-refresh-wallet-34.jpg");
        require(!image.empty(), "October 2 shelf fixture");
        require(BlackFlowStoreReplayAccess::wallet_page(s.store, image), "paid shelf not recognized");
        require(BlackFlowStoreReplayAccess::wallet(s.store, image) == 34, "paid wallet read incorrectly");
    });
}
