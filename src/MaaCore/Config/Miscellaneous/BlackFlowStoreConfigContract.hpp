#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <meojson/json.hpp>

#include "Utils/JsonContract.hpp"
#include "Utils/Sha256.hpp"

namespace asst
{
struct BlackFlowStoreConfigContract
{
    std::vector<std::string> standard_product_names;
    std::string standard_product_names_sha256;
    int source_product_names_count = 0;
    std::string source_product_names_sha256;
    std::string client_data_commit;
    int prts_synthetic_substances_revision = 0;
    int prts_engine_accessories_revision = 0;
};

namespace black_flow_release_contract
{
inline constexpr int StandardProductNamesCount = 326;
inline constexpr std::string_view StandardProductNamesSha256 =
    "48972fc3a5b40a6586b2707f14c5e73ccf23dfaf0a25cea121b5e273684f7a50";
inline constexpr int SourceProductNamesCount = 365;
inline constexpr std::string_view SourceProductNamesSha256 =
    "8981c39f89d6e074f02c395cdf7707c3c26796357527b78b42aff970c7cfb9fa";
inline constexpr std::string_view ClientDataCommit = "6b6ac60fc30a16ff2c2aefc0889e94112dda8ed4";
inline constexpr int PrtsSyntheticSubstancesRevision = 409595;
inline constexpr int PrtsEngineAccessoriesRevision = 409409;
} // namespace black_flow_release_contract

namespace black_flow_config_detail
{
inline std::optional<BlackFlowStoreConfigContract> parse(const json::value& root)
{
    using namespace black_flow_release_contract;
    if (!utils::has_exact_json_fields(
            root,
            std::array {
                std::string_view("schema"),
                std::string_view("version"),
                std::string_view("language"),
                std::string_view("source"),
                std::string_view("matching"),
            })) {
        return std::nullopt;
    }

    if (!root.at("schema").is_string() || root.at("schema").as_string() != "maa.black_flow.standard_product_names" ||
        !root.at("version").is<int>() || root.at("version").as_integer() != 1 || !root.at("language").is_string() ||
        root.at("language").as_string() != "zh_cn") {
        return std::nullopt;
    }

    const auto& source = root.at("source");
    if (!utils::has_exact_json_fields(
            source,
            std::array {
                std::string_view("product_names_count"),
                std::string_view("product_names_sha256"),
                std::string_view("client_data"),
                std::string_view("prts"),
            })) {
        return std::nullopt;
    }

    const auto& client_data = source.at("client_data");
    if (!utils::has_exact_json_fields(
            client_data,
            std::array {
                std::string_view("repository"),
                std::string_view("commit"),
                std::string_view("path"),
                std::string_view("file_sha256"),
                std::string_view("raw_record_count"),
            })) {
        return std::nullopt;
    }

    const auto& prts = source.at("prts");
    if (!utils::has_exact_json_fields(
            prts,
            std::array {
                std::string_view("synthetic_substances"),
                std::string_view("engine_accessories"),
            })) {
        return std::nullopt;
    }

    const auto& synthetic = prts.at("synthetic_substances");
    const auto& engine = prts.at("engine_accessories");
    constexpr std::array revision_fields {
        std::string_view("revision"),
        std::string_view("product_names_count"),
        std::string_view("product_names_sha256"),
    };
    if (!utils::has_exact_json_fields(synthetic, revision_fields) ||
        !utils::has_exact_json_fields(engine, revision_fields)) {
        return std::nullopt;
    }

    const auto& matching = root.at("matching");
    if (!utils::has_exact_json_fields(
            matching,
            std::array {
                std::string_view("normalization"),
                std::string_view("product_names_count"),
                std::string_view("product_names_sha256"),
                std::string_view("product_names"),
            }) ||
        !source.at("product_names_count").is<int>() || !source.at("product_names_sha256").is_string() ||
        !client_data.at("repository").is_string() || !client_data.at("commit").is_string() ||
        !client_data.at("path").is_string() || !client_data.at("file_sha256").is_string() ||
        !client_data.at("raw_record_count").is<int>() || !synthetic.at("revision").is<int>() ||
        !synthetic.at("product_names_count").is<int>() || !synthetic.at("product_names_sha256").is_string() ||
        !engine.at("revision").is<int>() || !engine.at("product_names_count").is<int>() ||
        !engine.at("product_names_sha256").is_string() || !matching.at("normalization").is_string() ||
        !matching.at("product_names_count").is<int>() || !matching.at("product_names_sha256").is_string() ||
        !matching.at("product_names").is_array()) {
        return std::nullopt;
    }

    if (source.at("product_names_count").as_integer() != SourceProductNamesCount ||
        source.at("product_names_sha256").as_string() != SourceProductNamesSha256 ||
        client_data.at("repository").as_string() != "Kengxxiao/ArknightsGameData" ||
        client_data.at("commit").as_string() != ClientDataCommit ||
        client_data.at("path").as_string() != "zh_CN/gamedata/excel/roguelike_topic_table.json" ||
        client_data.at("file_sha256").as_string() !=
            "00fc51c87ed1bf759298fbdc8c356681c43e85240633440a1993cb61d080bd9b" ||
        client_data.at("raw_record_count").as_integer() != 420 ||
        synthetic.at("revision").as_integer() != PrtsSyntheticSubstancesRevision ||
        synthetic.at("product_names_count").as_integer() != 239 ||
        synthetic.at("product_names_sha256").as_string() !=
            "9405849bb25a2a8a7d450ca6836368df2f8306e483f62ea483c5d48dbbb1c307" ||
        engine.at("revision").as_integer() != PrtsEngineAccessoriesRevision ||
        engine.at("product_names_count").as_integer() != 30 ||
        engine.at("product_names_sha256").as_string() !=
            "dd60d6c0064de3acb91fb3b103b0758e6d00b3ea14a4e297596ff67ea310a95d" ||
        matching.at("normalization").as_string() !=
            "unicode_nfc_then_strip_hyphen_greek_variant_suffix_then_deduplicate" ||
        matching.at("product_names_count").as_integer() != StandardProductNamesCount ||
        matching.at("product_names_sha256").as_string() != StandardProductNamesSha256) {
        return std::nullopt;
    }

    BlackFlowStoreConfigContract result;
    result.source_product_names_count = source.at("product_names_count").as_integer();
    result.source_product_names_sha256 = source.at("product_names_sha256").as_string();
    result.client_data_commit = client_data.at("commit").as_string();
    result.prts_synthetic_substances_revision = synthetic.at("revision").as_integer();
    result.prts_engine_accessories_revision = engine.at("revision").as_integer();
    result.standard_product_names_sha256 = matching.at("product_names_sha256").as_string();

    std::string fingerprint_input;
    const auto& names = matching.at("product_names").as_array();
    if (names.size() != static_cast<size_t>(StandardProductNamesCount)) {
        return std::nullopt;
    }
    for (size_t index = 0; index < names.size(); ++index) {
        const auto& name = names[index];
        if (!name.is_string() || name.as_string().empty()) {
            return std::nullopt;
        }
        if (index != 0U) {
            fingerprint_input.push_back('\n');
        }
        fingerprint_input += name.as_string();
        result.standard_product_names.emplace_back(name.as_string());
    }
    if (!std::ranges::is_sorted(result.standard_product_names) ||
        std::ranges::adjacent_find(result.standard_product_names) != result.standard_product_names.end() ||
        utils::sha256(fingerprint_input) != StandardProductNamesSha256) {
        return std::nullopt;
    }
    return result;
}
} // namespace black_flow_config_detail

inline std::optional<BlackFlowStoreConfigContract> parse_black_flow_store_config(const json::value& root)
{
    return black_flow_config_detail::parse(root);
}
} // namespace asst
