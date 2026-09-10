#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <meojson/json.hpp>

#include "Vision/Roguelike/RoguelikeRecruitRole.h"

TEST_CASE("Roguelike recruitment identifies Amiya forms from the visible operator list")
{
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto data = json::open(root / "resource/battle_data.json");
    REQUIRE(data.has_value());
    std::unordered_map<std::string, std::unordered_set<std::string>> roles_by_name;
    for (const auto& [id, oper] : data->at("chars").as_object()) {
        roles_by_name[oper.at("name").as_string()].emplace(oper.at("profession").as_string());
    }
    const auto lookup_roles = [&](const std::string& name) {
        const auto it = roles_by_name.find(name);
        return it == roles_by_name.end() ? std::unordered_set<std::string> {} : it->second;
    };
    const auto resolve = [&](const std::unordered_set<std::string>& names) {
        return asst::resolve_recruitment_role(names, lookup_roles, std::string("UNKNOWN"));
    };

    SECTION("captured medical page passes voucher validation")
    {
        // asst (4).log: all 48 observations contained these eight names, including
        // the unsuffixed medical Amiya, before voucher validation stopped the task.
        CHECK(
            resolve({ "凯尔希·思衡托", "Mon3tr", "焰影苇草", "闪灵", "流明", "阿米娅", "赫默", "白面鸮" }) == "MEDIC");
    }

    SECTION("guard and caster pages retain their own roles")
    {
        CHECK(resolve({ "阿米娅", "玫兰莎" }) == "WARRIOR");
        CHECK(resolve({ "阿米娅", "卡达" }) == "CASTER");
    }

    SECTION("ambiguous names and conflicting pages remain unconfirmed")
    {
        CHECK_FALSE(resolve({ "阿米娅" }).has_value());
        CHECK_FALSE(resolve({ "阿米娅", "未识别的干员" }).has_value());
        CHECK_FALSE(resolve({ "阿米娅", "闪灵", "卡达" }).has_value());
    }
}
