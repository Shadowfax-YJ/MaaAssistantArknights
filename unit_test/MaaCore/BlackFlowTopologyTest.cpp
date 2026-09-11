#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <meojson/json.hpp>

#include "Vision/Roguelike/BlackFlow/BlackFlowFloor.h"

TEST_CASE("BlackFlow tree-hole title OCR keeps the observed green effect constrained to green templates")
{
    using namespace asst::blackflow::perception;
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto tasks = json::open(root / "resource/tasks/Roguelike/BlackFlow.json");
    REQUIRE(tasks.has_value());
    const auto& task = tasks->at("BlackFlow@Roguelike@TreeHoleEffectTitle");
    const auto replacements = task.get("ocrReplace", std::vector<std::vector<std::string>> {});
    const auto normalize = [&](std::string title) {
        for (const auto& replacement : replacements) {
            REQUIRE(replacement.size() == 2);
            const std::regex pattern(replacement[0]);
            if (task.get("replaceFull", false)) {
                if (std::regex_search(title, pattern)) {
                    title = replacement[1];
                }
            }
            else {
                title = std::regex_replace(title, pattern, replacement[1]);
            }
        }
        return title;
    };

    // Actual title from run 1058; the OCR typo previously disabled the color constraint.
    const auto title = normalize("“朱亡者遗怨” ");
    CHECK(title == "“未亡者遗怨” ");
    const auto color = tree_hole_mist_color(title);
    REQUIRE(color == "green");
    CHECK(topology_matches_tree_hole_color(TreeHoleFloor, "green", color));
    CHECK_FALSE(topology_matches_tree_hole_color(TreeHoleFloor, "red", color));
    CHECK(normalize("“未亡者遗怨” ") == "“未亡者遗怨” ");
    CHECK(normalize("“巨人摇篮” ") == "“巨人摇篮” ");
    CHECK(normalize("未知的朱色标题") == "未知的朱色标题");
    CHECK(tree_hole_mist_color(normalize("未知的朱色标题")).empty());
}

TEST_CASE("BlackFlow tree-hole templates preserve every screenshot corridor")
{
    // Independently transcribed from the nine screenshot groups reviewed on 2026-08-12.
    // S = initial position, o = occupied slot; only explicit '-' / '|' are corridors.
    // The source archive's treehole-NN numbering differs from the runtime TH-TNN IDs.
    struct Fixture
    {
        std::string id;
        std::string source;
        std::vector<std::string> diagram;
    };

    const std::vector<Fixture> fixtures {
        { "TH-T01",
          "treehole-05 / 0953D5DF-D4C1-4005-8EC9-229D93429D20_hd.jpg",
          { "o-o-o-o-o", "| | | | |", "o-o-o-o-o", "| | | | |", "o-o-o-o-o", "    |    ", "    S    " } },
        { "TH-T02", "treehole-03 / 1.png", { "o-o-o", "| | |", "o-S-o", "| | |", "o-o-o" } },
        { "TH-T03", "treehole-04 / 4-洞_001.png", { "  o-o    ", "  | |    ", "S-o-o-o-o", "  | |    ", "  o-o    " } },
        { "TH-T04", "treehole-01 / 3d_001.png", { "o-o-o-S-o-o-o" } },
        { "TH-T05",
          "treehole-02 / 2_003.png",
          { "      o", "      |", "    o-o", "    | |", "  o-o-o", "  | | |", "o-o-o-S" } },
        { "TH-T06",
          "treehole-07 / 004C5998-DEA0-4B15-8F95-593E49D95B29_big.jpg",
          { "o-o-o-o-o", "| | | | |", "S-o-o o-o" } },
        { "TH-T07", "treehole-09 / 3层洞.png", { "o-o-o", "|   |", "S-o-o", "|   |", "o-o-o" } },
        { "TH-T08", "treehole-08 / 147622F4-6F29-4F21-9BB2-15B9F110203E_hd.jpg", { "S-o-o-o-o-o" } },
        { "TH-T09",
          "treehole-06 / 2_001.png",
          { "S-o-o-o  ",
            "| | | |  ",
            "o-o-o-o-o",
            "| | | | |",
            "o-o-o-o-o",
            "| | | | |",
            "o-o-o-o-o",
            "  | | | |",
            "  o-o-o-o" } },
    };

    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto library = json::open(root / "resource/roguelike/BlackFlow/map_perception/topology.json");
    REQUIRE(library.has_value());
    const auto& templates = library->at("templates").as_array();
    REQUIRE(std::count_if(templates.begin(), templates.end(), [](const auto& entry) {
                return entry.at("floor").as_integer() == 6;
            }) == fixtures.size());

    using Slot = std::pair<int, int>; // column, row
    using Edge = std::pair<Slot, Slot>;
    const auto slot = [](const json::value& value) -> Slot {
        return { value.at(0).as_integer(), value.at(1).as_integer() };
    };
    const auto edge = [](Slot first, Slot second) -> Edge {
        if (second < first) {
            std::swap(first, second);
        }
        return { first, second };
    };
    for (const auto& fixture : fixtures) {
        DYNAMIC_SECTION(fixture.id << " from " << fixture.source)
        {
            const auto found = std::find_if(templates.begin(), templates.end(), [&](const auto& entry) {
                return entry.at("id").as_string() == fixture.id;
            });
            REQUIRE(found != templates.end());
            REQUIRE(found->at("floor").as_integer() == 6);
            REQUIRE(found->at("terminal_slots").as_array().empty());
            const auto& diagram = fixture.diagram;
            REQUIRE(
                slot(found->at("grid_shape")) == Slot { static_cast<int>((diagram.front().size() + 1) / 2),
                                                        static_cast<int>((diagram.size() + 1) / 2) });

            std::set<Slot> expected_slots;
            std::set<Edge> expected_edges;
            for (int y = 0; y < static_cast<int>(diagram.size()); ++y) {
                REQUIRE(diagram[y].size() == diagram.front().size());
                for (int x = 0; x < static_cast<int>(diagram[y].size()); ++x) {
                    const char mark = diagram[y][x];
                    if (mark == 'o' || mark == 'S') {
                        REQUIRE(x % 2 == 0);
                        REQUIRE(y % 2 == 0);
                        expected_slots.emplace(x / 2, y / 2);
                        if (mark == 'S') {
                            REQUIRE(slot(found->at("start_slot")) == Slot { x / 2, y / 2 });
                        }
                    }
                    else if (mark == '-') {
                        expected_edges.emplace(edge({ (x - 1) / 2, y / 2 }, { (x + 1) / 2, y / 2 }));
                    }
                    else if (mark == '|') {
                        expected_edges.emplace(edge({ x / 2, (y - 1) / 2 }, { x / 2, (y + 1) / 2 }));
                    }
                }
            }
            std::set<Slot> actual_slots;
            for (const auto& value : found->at("occupied_slots").as_array()) {
                REQUIRE(actual_slots.emplace(slot(value)).second);
            }
            CHECK(actual_slots == expected_slots);
            std::set<Edge> actual_edges;
            for (const auto& value : found->at("edges").as_array()) {
                const Edge corridor = edge(slot(value.at(0)), slot(value.at(1)));
                REQUIRE(actual_edges.emplace(corridor).second);
                INFO(
                    "Unexpected corridor: (" << corridor.first.first << "," << corridor.first.second << ")-("
                                             << corridor.second.first << "," << corridor.second.second << ")");
                CHECK(expected_edges.contains(corridor));
            }
            CHECK(actual_edges == expected_edges);
        }
    }
}
