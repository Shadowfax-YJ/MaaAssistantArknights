#include "Config/Miscellaneous/BlackFlowStoreConfigContract.hpp"
#include "Task/Miscellaneous/BlackFlowClientGuard.hpp"
#include "Task/Miscellaneous/BlackFlowOcrSidecar.hpp"

#include <filesystem>
#include <ranges>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <meojson/json.hpp>

namespace
{
json::value release_config_document()
{
    const auto path = std::filesystem::path(MAA_TEST_RESOURCE_DIR) / "black_flow" / "standard_product_names.json";
    const auto document = json::open(path, true, true);
    REQUIRE(document.has_value());
    return document.value();
}

json::value minimal_sidecar_document()
{
    const auto config = asst::parse_black_flow_store_config(release_config_document());
    REQUIRE(config.has_value());
    return asst::black_flow_ocr_sidecar_to_json(
        asst::make_minimal_black_flow_ocr_sidecar(asst::BlackFlowClientType::Official, config.value()));
}

bool string_array_contains(const json::value& value, std::string_view expected)
{
    return value.is_array() && std::ranges::any_of(value.as_array(), [expected](const auto& item) {
               return item.is_string() && item.as_string() == expected;
           });
}
}

TEST_CASE("Release resource exposes the reviewed BlackFlow standard product names")
{
    const auto config = asst::parse_black_flow_store_config(release_config_document());
    REQUIRE(config.has_value());
    CHECK(config->standard_product_names.size() == 326);
    CHECK(config->standard_product_names_sha256 == "48972fc3a5b40a6586b2707f14c5e73ccf23dfaf0a25cea121b5e273684f7a50");
    CHECK(config->source_product_names_count == 365);
    CHECK(config->source_product_names_sha256 == "8981c39f89d6e074f02c395cdf7707c3c26796357527b78b42aff970c7cfb9fa");
    CHECK(config->client_data_commit == "6b6ac60fc30a16ff2c2aefc0889e94112dda8ed4");
    CHECK(config->prts_synthetic_substances_revision == 409595);
    CHECK(config->prts_engine_accessories_revision == 409409);
}

TEST_CASE("BlackFlow config rejects contract drift before task startup")
{
    SECTION("empty matching dictionary")
    {
        auto invalid = release_config_document();
        invalid["matching"]["product_names"] = json::array {};
        CHECK_FALSE(asst::parse_black_flow_store_config(invalid).has_value());
    }

    SECTION("declared matching count differs from the reviewed contract")
    {
        auto invalid = release_config_document();
        invalid["matching"]["product_names_count"] = 325;
        CHECK_FALSE(asst::parse_black_flow_store_config(invalid).has_value());
    }

    SECTION("matching names differ from their fingerprint")
    {
        auto invalid = release_config_document();
        invalid["matching"]["product_names"][0] = "篡改商品名";
        CHECK_FALSE(asst::parse_black_flow_store_config(invalid).has_value());
    }

    SECTION("declared matching fingerprint differs from the reviewed contract")
    {
        auto invalid = release_config_document();
        invalid["matching"]["product_names_sha256"] = std::string(64, '0');
        CHECK_FALSE(asst::parse_black_flow_store_config(invalid).has_value());
    }

    SECTION("source provenance conflicts with its reviewed revision")
    {
        auto invalid = release_config_document();
        invalid["source"]["prts"]["synthetic_substances"]["revision"] = 409596;
        CHECK_FALSE(asst::parse_black_flow_store_config(invalid).has_value());
    }

    SECTION("required field is missing")
    {
        auto invalid = release_config_document();
        invalid["source"].as_object().erase("product_names_sha256");
        CHECK_FALSE(asst::parse_black_flow_store_config(invalid).has_value());
    }

    SECTION("unknown field is present")
    {
        auto invalid = release_config_document();
        invalid["matching"]["future_override"] = true;
        CHECK_FALSE(asst::parse_black_flow_store_config(invalid).has_value());
    }
}

TEST_CASE("Official and Bilibili produce a minimal Schema v1 sidecar from release provenance")
{
    const auto config = asst::parse_black_flow_store_config(release_config_document());
    REQUIRE(config.has_value());

    const auto schema_path = std::filesystem::path(MAA_TEST_RESOURCE_DIR) / "black_flow" / "ocr_sidecar.schema.v1.json";
    const auto schema = json::open(schema_path, true, true);
    REQUIRE(schema.has_value());
    CHECK(schema->at("$id").as_string() == "https://maa.plus/schemas/black-flow/ocr-sidecar.v1.json");

    for (const auto client_type : { asst::BlackFlowClientType::Official, asst::BlackFlowClientType::Bilibili }) {
        const auto sidecar = asst::make_minimal_black_flow_ocr_sidecar(client_type, config.value());
        const auto document = asst::black_flow_ocr_sidecar_to_json(sidecar);
        const auto parsed = asst::parse_black_flow_ocr_sidecar(document);

        REQUIRE(parsed.has_value());
        CHECK(document.at("client_type").as_string() == asst::black_flow_client_type_name(client_type));
        CHECK(document.at("language").as_string() == "zh_cn");
        CHECK(document.at("provenance").at("standard_product_names_count").as_integer() == 326);
        CHECK(
            document.at("provenance").at("standard_product_names_sha256").as_string() ==
            "48972fc3a5b40a6586b2707f14c5e73ccf23dfaf0a25cea121b5e273684f7a50");
        CHECK(document.at("slots").as_array().size() == 10);
        CHECK(document.at("slots").at(0).contains("standard_product_name"));
        CHECK_FALSE(document.at("slots").at(0).contains("standard_name"));
    }
}

TEST_CASE("Schema v1 and the typed model enforce the same strict sidecar boundary")
{
    const auto schema_path = std::filesystem::path(MAA_TEST_RESOURCE_DIR) / "black_flow" / "ocr_sidecar.schema.v1.json";
    const auto schema = json::open(schema_path, true, true);
    REQUIRE(schema.has_value());

    CHECK_FALSE(schema->at("additionalProperties").as_boolean());
    CHECK(string_array_contains(schema->at("required"), "version"));
    CHECK(string_array_contains(schema->at("required"), "slots"));
    CHECK(schema->at("properties").at("version").at("const").as_integer() == 1);
    CHECK(string_array_contains(schema->at("properties").at("client_type").at("enum"), "Official"));
    CHECK(string_array_contains(schema->at("properties").at("client_type").at("enum"), "Bilibili"));
    CHECK(schema->at("properties").at("slots").at("minItems").as_integer() == 10);
    CHECK(schema->at("properties").at("slots").at("maxItems").as_integer() == 10);
    const auto& prefix_items = schema->at("properties").at("slots").at("prefixItems").as_array();
    REQUIRE(prefix_items.size() == 10);
    for (size_t index = 0; index < prefix_items.size(); ++index) {
        CHECK(
            prefix_items.at(index).at("allOf").at(1).at("properties").at("index").at("const").as_integer() ==
            static_cast<int>(index + 1U));
    }

    SECTION("missing field")
    {
        auto invalid = minimal_sidecar_document();
        invalid.as_object().erase("captured_at");
        CHECK_FALSE(asst::parse_black_flow_ocr_sidecar(invalid).has_value());
    }

    SECTION("unknown field")
    {
        auto invalid = minimal_sidecar_document();
        invalid["future_version_hint"] = 2;
        CHECK_FALSE(asst::parse_black_flow_ocr_sidecar(invalid).has_value());
    }

    SECTION("wrong integer version")
    {
        auto invalid = minimal_sidecar_document();
        invalid["version"] = 2;
        CHECK_FALSE(asst::parse_black_flow_ocr_sidecar(invalid).has_value());
    }

    SECTION("wrong enum")
    {
        auto invalid = minimal_sidecar_document();
        invalid["slots"][0]["status"] = "future_status";
        CHECK_FALSE(asst::parse_black_flow_ocr_sidecar(invalid).has_value());
    }

    SECTION("not ten slots")
    {
        auto invalid = minimal_sidecar_document();
        invalid["slots"].as_array().erase(9);
        CHECK_FALSE(asst::parse_black_flow_ocr_sidecar(invalid).has_value());
    }

    SECTION("slot index differs from its array position")
    {
        auto invalid = minimal_sidecar_document();
        invalid["slots"][4]["index"] = 4;
        CHECK_FALSE(asst::parse_black_flow_ocr_sidecar(invalid).has_value());
    }
}

TEST_CASE("Custom task admission rejects unsupported BlackFlow clients before UI takeover")
{
    for (const std::string task_name : { "MiniGame@BlackFlow@Begin", "BlackFlowTemporary@InvestSystem" }) {
        const std::vector black_flow_tasks { task_name };

        for (const std::string_view supported : { "Official", "Bilibili" }) {
            CHECK(
                asst::check_black_flow_task_admission(
                    black_flow_tasks,
                    asst::parse_black_flow_client_type(supported)) == asst::BlackFlowTaskAdmission::Accepted);
        }

        for (const std::string_view unsupported : { "txwy", "YoStarEN", "YoStarJP", "YoStarKR", "" }) {
            CHECK(
                asst::check_black_flow_task_admission(
                    black_flow_tasks,
                    asst::parse_black_flow_client_type(unsupported)) == asst::BlackFlowTaskAdmission::Rejected);
        }
    }

    CHECK(
        asst::check_black_flow_task_admission(std::vector<std::string> { "GachaOnce" }, std::nullopt) ==
        asst::BlackFlowTaskAdmission::NotBlackFlow);
}
