#pragma once
#include "Vision/Roguelike/BlackFlow/SacrificeRecognition.h"

template <class Test>
void sacrifice_priority_regressions(const std::filesystem::path& repo, Test&& test)
{
    using namespace asst::blackflow::perception;
    const auto root = repo / "unit_test/MaaCore/fixtures/blackflow-sacrifice/20261026";
    constexpr std::array first_grass_columns { 1, 3, 2, 1, 0 };
    for (int index = 0; index < static_cast<int>(first_grass_columns.size()); ++index) {
        test(("sacrifice actual thin grass picker " + std::to_string(index)).c_str(), [&] {
            const auto image = MAA_NS::imread(root / ("thin-grass-" + std::to_string(index) + ".jpg"));
            require(!image.empty(), "original thin grass picker missing");
            const auto list = sacrifice_item_list(image);
            require(list && !list->cards.empty(), "original cards not recognized");
            const auto preferred = sacrifice_preferred_part(image, list->cards);
            require(preferred.has_value(), "thin grass not preferred");
            require(preferred->x == 686 + 113 * first_grass_columns[index] && preferred->y == 175,
                    "preferred another part or failed to choose the first thin grass duplicate");
            require(sacrifice_selected_name(image) == "薄葬草", "actual selected-name OCR differs from thin grass");
        });
    }
    test("sacrifice actual parts picker without thin grass keeps random eligibility", [&] {
        const auto image = MAA_NS::imread(repo / "unit_test/MaaCore/fixtures/blackflow-sacrifice/20261025/picker-with-items.jpg");
        const auto list = sacrifice_item_list(image);
        require(list && !list->cards.empty(), "original parts picker missing");
        require(!sacrifice_preferred_part(image, list->cards), "another part became thin grass");
    });
    test("sacrifice actual relic picker never applies the part preference", [&] {
        const auto image = MAA_NS::imread(root / "relic-picker.jpg");
        const auto list = sacrifice_item_list(image);
        require(list && !list->cards.empty(), "original relic picker missing");
        require(!sacrifice_preferred_part(image, list->cards), "part preference applied to relics");
        require(!sacrifice_preferred_part({}, list->cards) && !sacrifice_preferred_part(image, {}),
                "empty observation produced a preferred card");
    });
}
