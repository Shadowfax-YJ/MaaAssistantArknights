// Included after the native movement replay and controller definitions.
// Only the input/timing effects are controlled; recognition and scan methods are production code.
std::pair<int, int> run_inventory_tail_replay(
    const std::filesystem::path& repo,
    const std::filesystem::path& batch,
    std::string_view filter)
{
    using namespace ui_replay;
    const auto fixture = repo / "unit_test/MaaCore/fixtures/blackflow-inventory/20261029-instance002";
    const auto provenance = json::open(fixture / "provenance.json").value();
    std::vector<cv::Mat> originals;
    for (int index = 1; index <= 9; ++index) {
        originals.emplace_back(load(fixture / ("page-" + std::to_string(index) + ".jpg")));
    }
    int passed = 0;
    int failed = 0;
    for (const std::string scenario : {
             "inventory tail rebound settles before the next swipe",
             "inventory tail never settles",
             "inventory tail page disappears during settling",
             "inventory tail user stops during settling",
             "inventory tail forward input fails",
         }) {
        if (!filter.empty() && filter != "inventory-tail" && filter != scenario) {
            continue;
        }
        try {
            const auto dir = batch / scenario;
            std::filesystem::create_directories(dir);
            check(UserDir.set(dir), "UserDir");
            IO io;
            io.page = IO::Page::Inventory;
            io.inventory = originals;
            io.map = load(repo / "unit_test/MaaCore/fixtures/blackflow-inventory-finish/20261028/clear-map.jpg");
            int forward_swipes = 0;
            int premature_swipes = 0;
            long long last_swipe = -1;
            io.inventory_input = [&](std::string_view task) {
                if (task != InventorySwipeTask) {
                    return;
                }
                if (forward_swipes >= 2 && io.clock - last_swipe < 1250) {
                    ++premature_swipes;
                }
                ++forward_swipes;
                last_swipe = io.clock;
                if (forward_swipes == 2 && scenario == "inventory tail user stops during settling") {
                    io.cancel_at = io.clock + 250;
                }
                if (forward_swipes == 2 && scenario == "inventory tail forward input fails") {
                    io.click_ok = false;
                }
            };
            io.inventory_image = [&]() -> cv::Mat {
                if (forward_swipes < 2) {
                    return originals.at(forward_swipes);
                }
                const auto elapsed = io.clock - last_swipe;
                if (scenario == "inventory tail page disappears during settling" && elapsed >= 250) {
                    return io.map;
                }
                if (scenario == "inventory tail never settles") {
                    return originals.at(elapsed / 250 % 2 ? 6 : 8);
                }
                // Without passive observations, each new gesture sees the next original
                // failure frame, reproducing all nine captured pages and the old timeout.
                if (elapsed == 0) {
                    return originals.at(std::min(forward_swipes, 8));
                }
                // Settling after the gesture is controlled, not additional field footage.
                // Keep every image byte original, then hold frame 7 after 750 ms.
                return originals.at(elapsed < 750 ? 2 + int(elapsed / 250) : 6);
            };
            auto session = std::make_shared<BlackFlowSession>();
            auto port = std::make_shared<Port>(io);
            check(session->initialize("automation_collection") && session->set_current_floor(5), "session");
            check(session->configure_diagnostics({ DiagnosticLevel::Detailed, 3 }), "diagnostics");
            port->writer.set_collection_popup_session(session);
            check(
                session->apply_movement_inventory_observation({ { MovementKind::M04, 1, 0 } }, nullptr, false),
                "previous inventory");
            session->invalidate_movement_inventory();
            Movement observer(io, session, port);
            Movement::InventoryFrame frame;
            std::string error;
            if (scenario == "inventory tail rebound settles before the next swipe") {
                check(observer.observe_inventory(), "inventory callback");
                check(!session->terminated(), session->result() ? session->result()->termination_reason : "stopped");
                check(!session->movement_inventory_refresh_required(), "complete inventory was not committed");
                check(session->run().resources.lorentz_eye_available, "tail lost the observed Lorentz eye");
                const auto& actual = session->run().resources.movement_instances;
                const auto& expected = provenance.at("expected_movement_instances").as_array();
                check(actual.size() == expected.size(), "lost or duplicated movement instances");
                // Re-OCR of the original JPEG can move a same-row name by one pixel,
                // changing its enumeration order. Keep multiplicity and charges exact.
                std::vector<std::pair<std::string, int>> actual_items;
                std::vector<std::pair<std::string, int>> expected_items;
                for (std::size_t index = 0; index < actual.size(); ++index) {
                    actual_items.emplace_back(to_string(actual[index].movement), actual[index].remaining_charges);
                    expected_items.emplace_back(
                        expected[index].at("movement").as_string(),
                        expected[index].at("remaining_charges").as_integer());
                    check(actual[index].inventory_index == static_cast<int>(index), "duplicate inventory instance index");
                }
                std::ranges::sort(actual_items);
                std::ranges::sort(expected_items);
                check(actual_items == expected_items, "movement identity, multiplicity or remaining charges changed");
                check(premature_swipes == 0 && forward_swipes == 4, "swiped during rebound or did not confirm the end");
                std::cout << "EVIDENCE inventory tail producer " << dir.string() << '\n';
            }
            else {
                check(
                    !observer.scan_inventory_frame(frame, &error) && !frame.scan_complete,
                    "incomplete observation was accepted");
                check(forward_swipes == 2 && premature_swipes == 0, "failed observation sent another swipe");
                if (scenario == "inventory tail page disappears during settling") {
                    check(frame.page_lost && !frame.images.empty(), "lost-page evidence was not retained");
                }
                if (scenario == "inventory tail never settles") {
                    check(
                        !frame.images.empty() && error.find("settle") != std::string::npos,
                        "unstable tail was not preserved as a bounded failure");
                }
                if (scenario == "inventory tail user stops during settling") {
                    check(io.cancelled, "cancellation was not exercised");
                }
                check(
                    session->movement_inventory_refresh_required() &&
                        session->run().resources.movement_instances.size() == 1 &&
                        session->run().resources.movement_instances.front().movement == MovementKind::M04,
                    "partial scan replaced the previous inventory");
            }
            ++passed;
            std::cout << "PASS " << scenario << '\n';
        }
        catch (const std::exception& error) {
            ++failed;
            std::cout << "FAIL " << scenario << ": " << error.what() << '\n';
        }
    }
    return { passed, failed };
}
