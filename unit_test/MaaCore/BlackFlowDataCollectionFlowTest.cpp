#include <filesystem>
#include <ranges>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <meojson/json.hpp>

namespace
{
json::value collection_flow()
{
    const auto path = std::filesystem::path(MAA_TEST_RESOURCE_DIR) / "tasks" / "MiniGame" / "BlackFlow.json";
    const auto document = json::open(path, true, true);
    REQUIRE(document.has_value());
    return document.value();
}

bool string_array_contains(const json::value& value, std::string_view expected)
{
    return value.is_array() && std::ranges::any_of(value.as_array(), [expected](const auto& item) {
               return item.is_string() && item.as_string() == expected;
           });
}

void check_next_contains(const json::value& flow, std::string_view task, std::string_view next)
{
    CAPTURE(task, next);
    const auto task_name = std::string(task);
    REQUIRE(flow.contains(task_name));
    REQUIRE(flow.at(task_name).contains("next"));
    CHECK(string_array_contains(flow.at(task_name).at("next"), next));
}
} // namespace

TEST_CASE("BlackFlow collection follows the recorded pioneer-squad opening")
{
    const auto flow = collection_flow();

    const auto& squad = flow.at("MiniGame@BlackFlow@ChoosePioneerSquad");
    CHECK(squad.at("algorithm").as_string() == "OcrDetect");
    CHECK(string_array_contains(squad.at("text"), "开拓者分队"));
    check_next_contains(flow, "MiniGame@BlackFlow@StartExplore", "MiniGame@BlackFlow@ChoosePioneerSquad");
    check_next_contains(flow, "MiniGame@BlackFlow@StartExplore", "MiniGame@BlackFlow@SwipeToPioneerSquad");
    const auto& squad_swipe = flow.at("MiniGame@BlackFlow@SwipeToPioneerSquad");
    CHECK(squad_swipe.at("baseTask").as_string() == "SwipeToTheRight");
    check_next_contains(flow, "MiniGame@BlackFlow@SwipeToPioneerSquad", "MiniGame@BlackFlow@ChoosePioneerSquad");
    check_next_contains(flow, "MiniGame@BlackFlow@ChoosePioneerSquad", "MiniGame@BlackFlow@ProcessingItemReady");

    const auto& processing_item = flow.at("MiniGame@BlackFlow@ProcessingItemReady");
    CHECK(processing_item.at("action").as_string() == "DoNothing");
    CHECK(string_array_contains(processing_item.at("text"), "加工品"));
    check_next_contains(flow, "MiniGame@BlackFlow@ProcessingItemReady", "MiniGame@BlackFlow@CloseProcessingItem");
    check_next_contains(flow, "MiniGame@BlackFlow@CloseProcessingItem", "MiniGame@BlackFlow@ChooseRecruitCombination");
}

TEST_CASE("BlackFlow collection gives up exactly three recruitments before entering the first floor")
{
    const auto flow = collection_flow();

    const auto& combination = flow.at("MiniGame@BlackFlow@ChooseRecruitCombination");
    CHECK(string_array_contains(combination.at("text"), "稳扎稳打"));
    check_next_contains(
        flow,
        "MiniGame@BlackFlow@ChooseRecruitCombination",
        "MiniGame@BlackFlow@ConfirmRecruitCombination");

    const auto& combination_confirm = flow.at("MiniGame@BlackFlow@ConfirmRecruitCombination");
    CHECK(combination_confirm.at("action").as_string() == "ClickSelf");
    CHECK(string_array_contains(combination_confirm.at("text"), "你确定要这么做"));
    check_next_contains(flow, "MiniGame@BlackFlow@ConfirmRecruitCombination", "MiniGame@BlackFlow@RecruitVoucher1");

    for (int index = 1; index <= 3; ++index) {
        const auto suffix = std::to_string(index);
        const auto voucher = "MiniGame@BlackFlow@RecruitVoucher" + suffix;
        const auto give_up = "MiniGame@BlackFlow@GiveUpRecruit" + suffix;
        const auto confirm = "MiniGame@BlackFlow@GiveUpRecruitConfirm" + suffix;

        REQUIRE(flow.contains(voucher));
        REQUIRE(flow.contains(give_up));
        REQUIRE(flow.contains(confirm));
        check_next_contains(flow, voucher, give_up);
        check_next_contains(flow, give_up, confirm);
        CHECK(flow.at(confirm).at("template").as_string() == "Roguelike@StageEncounterGiveUpRecruitConfirm.png");

        if (index < 3) {
            check_next_contains(flow, confirm, "MiniGame@BlackFlow@RecruitVoucher" + std::to_string(index + 1));
        }
        else {
            check_next_contains(flow, confirm, "MiniGame@BlackFlow@ChooseTreeSea");
        }
    }

    const auto& start = flow.at("MiniGame@BlackFlow@ChooseTreeSea");
    CHECK(start.at("action").as_string() == "ClickSelf");
    CHECK(string_array_contains(start.at("text"), "沉沦于树海"));
    check_next_contains(flow, "MiniGame@BlackFlow@ChooseTreeSea", "MiniGame@BlackFlow@WaitForFirstLayer");
}

TEST_CASE("BlackFlow collection captures the parts box then reuses the established abandon path")
{
    const auto flow = collection_flow();

    check_next_contains(flow, "MiniGame@BlackFlow@WaitForFirstLayer", "MiniGame@BlackFlow@OpenPartsBox");
    check_next_contains(flow, "MiniGame@BlackFlow@OpenPartsBox", "MiniGame@BlackFlow@PartsBoxReady");

    const auto& parts_box = flow.at("MiniGame@BlackFlow@PartsBoxReady");
    CHECK(parts_box.at("action").as_string() == "DoNothing");
    CHECK(string_array_contains(parts_box.at("text"), "零件箱容量"));

    check_next_contains(flow, "MiniGame@BlackFlow@PartsBoxReady", "MiniGame@BlackFlow@ClosePartsBox");
    check_next_contains(flow, "MiniGame@BlackFlow@ClosePartsBox", "MiniGame@BlackFlow@ExitExploration");
    check_next_contains(flow, "MiniGame@BlackFlow@ExitExploration", "MiniGame@BlackFlow@Abandon");
    check_next_contains(flow, "MiniGame@BlackFlow@Abandon", "MiniGame@BlackFlow@AbandonConfirm");
    check_next_contains(flow, "MiniGame@BlackFlow@AbandonConfirm", "MiniGame@BlackFlow@WaitForExploreRestart");
    check_next_contains(flow, "MiniGame@BlackFlow@WaitForExploreRestart", "MiniGame@BlackFlow@StartExplore");
}
