#pragma once
#include "Task/Roguelike/BlackFlow/BlackFlowMovementRecognition.h"
#include "Utils/WorkingDir.hpp"
#include <chrono>
#include <tuple>

template <class Test>
void overload_recognition_regressions(const std::filesystem::path& repo, Test&& test)
{
    using namespace asst;
    using namespace asst::blackflow;
    const auto root = repo / "unit_test/MaaCore/fixtures/blackflow-overload/20261025";
    for (const auto& [name, page, count, limit] : {
             std::tuple { "overloaded-inventory.jpg", InventoryOverloadPage::Inventory, 13, 12 },
             std::tuple { "cleared-map.jpg", InventoryOverloadPage::Map, 12, 12 },
             std::tuple { "normal-inventory.jpg", InventoryOverloadPage::Inventory, 11, 12 },
         }) {
        test((std::string("overload actual original state ") + name).c_str(), [&] {
            const auto image = MAA_NS::imread(root / name);
            require(!image.empty(), "overload original missing");
            const auto observed = recognize_inventory_overload(image);
            require(observed.known() && observed.page == page, "actual page or capacity was not recognized");
            require(
                observed.capacity->count == count && observed.capacity->limit == limit,
                "actual capacity differs from the original");
            require(observed.cleared() == (count <= limit), "overload status conflicts with the actual count");
        });
    }
    for (const auto* name : { "details-overlay.jpg", "reward-popup.jpg" }) {
        test((std::string("overload actual foreground blocks background state ") + name).c_str(), [&] {
            const auto image = MAA_NS::imread(root / name);
            require(!image.empty(), "foreground original missing");
            require(!recognize_inventory_overload(image).known(), "foreground was treated as a safe inventory/map");
        });
    }
    test("overload capacity cannot be fabricated from missing or conflicting OCR", [&] {
        const auto row = [](std::string text, double score = .99) {
            return TextRect { { 0, 0, 80, 25 }, score, std::move(text) };
        };
        for (const auto& rows : { std::vector<TextRect> { },
                                  std::vector { row("13/12"), row("12/12") },
                                  std::vector { row("12/0") },
                                  std::vector { row("100/12") },
                                  std::vector { row("-1/12") },
                                  std::vector { row("O/12") },
                                  std::vector { row("12/12", .3) } }) {
            require(!inventory_capacity_from_ocr(rows), "unknown or conflicting capacity became valid");
        }
        require(!recognize_inventory_overload({ }).known(), "empty capture became a cleared inventory");
    });
    test("overload actual logger preserves evidence without creating node or item receipts", [&] {
        const auto payloads = json::open(root / "lifecycle-payloads.json").value();
        const auto output = std::filesystem::current_path() / "overload-logger";
        std::filesystem::create_directories(output);
        const auto isolated = output / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        require(std::filesystem::create_directory(isolated), "fresh logger fixture directory");
        const auto previous = UserDir.empty() ? std::filesystem::current_path() : UserDir.get();
        struct RestoreUserDir
        {
            std::filesystem::path previous;
            ~RestoreUserDir() { UserDir.set(previous); }
        } restore { previous };
        require(UserDir.set(isolated), "UserDir");
        auto session = std::make_shared<BlackFlowSession>();
        require(session->initialize("automation_collection") && session->set_current_floor(4), "session");
        BlackFlowTaskPort port({ }, nullptr, "Roguelike", std::make_shared<BlackFlowMapObservationSource>());
        port.set_collection_popup_session(session);
        for (const auto& payload : payloads.as_array()) {
            RunLogEvent event { .level = RunLogLevel::Info,
                                .action = payload.get("action", std::string()),
                                .phase = payload.get("phase", std::string()),
                                .outcome = "observed",
                                .task = "BlackFlowInventoryOverload",
                                .state = session->run_log_state(),
                                .details = payload.at("details").as_object() };
            const bool cleared = event.details.get("overload_cleared", false);
            const auto image = MAA_NS::imread(root / (cleared ? "cleared-map.jpg" : "overloaded-inventory.jpg"));
            std::string error;
            require(
                port.record_run_event(session->run_revision(), event, std::make_shared<cv::Mat>(image), false, &error),
                error);
        }
        json::array events;
        for (const auto& entry : std::filesystem::directory_iterator(isolated / "debug/BlackFlow")) {
            if (entry.is_directory()) {
                std::ifstream file(entry.path() / "run-events.jsonl");
                std::string line;
                while (std::getline(file, line)) {
                    events.emplace_back(json::parse(line).value());
                }
            }
        }
        require(events.size() == payloads.as_array().size(), "overload evidence was dropped");
        for (const auto& event : events) {
            require(
                event.get("action", std::string()) == "inventory.overload.capture",
                "evidence became an acquisition");
        }
        std::ofstream(output / "producer-events.json") << json::value(events).to_string();
    });
}
