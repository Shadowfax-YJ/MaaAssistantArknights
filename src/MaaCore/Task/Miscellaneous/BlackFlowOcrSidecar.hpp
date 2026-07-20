#pragma once

#include "Config/Miscellaneous/BlackFlowStoreConfigContract.hpp"
#include "Task/Miscellaneous/BlackFlowClientGuard.hpp"

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <meojson/json.hpp>

#include "Utils/EnumMapping.hpp"
#include "Utils/JsonContract.hpp"

namespace asst
{
enum class BlackFlowPageStatus
{
    Complete,
    Partial,
    Failed,
};

enum class BlackFlowSlotStatus
{
    Matched,
    Empty,
    Unmatched,
    OcrError,
    MatchError,
    NotProcessed,
};

enum class BlackFlowMatchKind
{
    None,
    Exact,
    Fuzzy,
};

enum class BlackFlowErrorStage
{
    Capture,
    Ocr,
    Match,
    Persist,
};

struct BlackFlowSidecarError
{
    BlackFlowErrorStage stage = BlackFlowErrorStage::Capture;
    std::string code;
    std::string message;
    bool retryable = false;
    int slot_index = 0;
};

struct BlackFlowSidecarImage
{
    std::string file_name;
    int width = 1280;
    int height = 720;
    std::string sha256;
};

struct BlackFlowSidecarProvenance
{
    std::string config_schema;
    int config_version = 1;
    int standard_product_names_count = 0;
    std::string standard_product_names_sha256;
    int source_product_names_count = 0;
    std::string source_product_names_sha256;
    std::string client_data_commit;
    int prts_synthetic_substances_revision = 0;
    int prts_engine_accessories_revision = 0;
};

struct BlackFlowSidecarSlot
{
    int index = 0;
    BlackFlowSlotStatus status = BlackFlowSlotStatus::NotProcessed;
    std::string ocr_text;
    double ocr_score = 0.0;
    std::string standard_product_name;
    BlackFlowMatchKind match_kind = BlackFlowMatchKind::None;
    std::vector<BlackFlowSidecarError> errors;
};

struct BlackFlowOcrSidecar
{
    std::string schema;
    int version = 1;
    std::string exploration_id;
    int page_index = 1;
    std::string captured_at;
    BlackFlowClientType client_type = BlackFlowClientType::Official;
    std::string language;
    BlackFlowSidecarImage image;
    BlackFlowPageStatus page_status = BlackFlowPageStatus::Complete;
    BlackFlowSidecarProvenance provenance;
    std::array<BlackFlowSidecarSlot, 10> slots;
    std::vector<BlackFlowSidecarError> errors;
};

namespace black_flow_sidecar_detail
{
inline constexpr std::array<std::pair<BlackFlowPageStatus, std::string_view>, 3> PageStatusNames { {
    { BlackFlowPageStatus::Complete, "complete" },
    { BlackFlowPageStatus::Partial, "partial" },
    { BlackFlowPageStatus::Failed, "failed" },
} };

inline constexpr std::array<std::pair<BlackFlowSlotStatus, std::string_view>, 6> SlotStatusNames { {
    { BlackFlowSlotStatus::Matched, "matched" },
    { BlackFlowSlotStatus::Empty, "empty" },
    { BlackFlowSlotStatus::Unmatched, "unmatched" },
    { BlackFlowSlotStatus::OcrError, "ocr_error" },
    { BlackFlowSlotStatus::MatchError, "match_error" },
    { BlackFlowSlotStatus::NotProcessed, "not_processed" },
} };

inline constexpr std::array<std::pair<BlackFlowMatchKind, std::string_view>, 3> MatchKindNames { {
    { BlackFlowMatchKind::None, "none" },
    { BlackFlowMatchKind::Exact, "exact" },
    { BlackFlowMatchKind::Fuzzy, "fuzzy" },
} };

inline constexpr std::array<std::pair<BlackFlowErrorStage, std::string_view>, 4> ErrorStageNames { {
    { BlackFlowErrorStage::Capture, "capture" },
    { BlackFlowErrorStage::Ocr, "ocr" },
    { BlackFlowErrorStage::Match, "match" },
    { BlackFlowErrorStage::Persist, "persist" },
} };

inline std::string_view page_status_name(BlackFlowPageStatus status)
{
    return utils::enum_name(status, PageStatusNames);
}

inline std::optional<BlackFlowPageStatus> parse_page_status(std::string_view value)
{
    return utils::parse_enum(value, PageStatusNames);
}

inline std::string_view slot_status_name(BlackFlowSlotStatus status)
{
    return utils::enum_name(status, SlotStatusNames);
}

inline std::optional<BlackFlowSlotStatus> parse_slot_status(std::string_view value)
{
    return utils::parse_enum(value, SlotStatusNames);
}

inline std::string_view match_kind_name(BlackFlowMatchKind kind)
{
    return utils::enum_name(kind, MatchKindNames);
}

inline std::optional<BlackFlowMatchKind> parse_match_kind(std::string_view value)
{
    return utils::parse_enum(value, MatchKindNames);
}

inline std::string_view error_stage_name(BlackFlowErrorStage stage)
{
    return utils::enum_name(stage, ErrorStageNames);
}

inline std::optional<BlackFlowErrorStage> parse_error_stage(std::string_view value)
{
    return utils::parse_enum(value, ErrorStageNames);
}

inline json::object error_to_json(const BlackFlowSidecarError& error)
{
    return {
        { "stage", std::string(error_stage_name(error.stage)) },
        { "code", error.code },
        { "message", error.message },
        { "retryable", error.retryable },
        { "slot_index", error.slot_index },
    };
}

inline std::optional<BlackFlowSidecarError> parse_error(const json::value& value)
{
    if (!utils::has_exact_json_fields(
            value,
            std::array {
                std::string_view("stage"),
                std::string_view("code"),
                std::string_view("message"),
                std::string_view("retryable"),
                std::string_view("slot_index"),
            }) ||
        !value.at("stage").is_string() || !value.at("code").is_string() || !value.at("message").is_string() ||
        !value.at("retryable").is_boolean() || !value.at("slot_index").is<int>()) {
        return std::nullopt;
    }

    auto stage = parse_error_stage(value.at("stage").as_string());
    const auto slot_index = value.at("slot_index").as_integer();
    if (!stage || value.at("code").as_string().empty() || value.at("message").as_string().empty() || slot_index < 0 ||
        slot_index > 10) {
        return std::nullopt;
    }

    return BlackFlowSidecarError {
        .stage = stage.value(),
        .code = value.at("code").as_string(),
        .message = value.at("message").as_string(),
        .retryable = value.at("retryable").as_boolean(),
        .slot_index = slot_index,
    };
}

inline bool is_lowercase_sha256(std::string_view value)
{
    if (value.size() != 64U) {
        return false;
    }
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

inline std::optional<std::vector<BlackFlowSidecarError>> parse_errors(const json::value& value)
{
    if (!value.is_array()) {
        return std::nullopt;
    }
    std::vector<BlackFlowSidecarError> result;
    result.reserve(value.as_array().size());
    for (const auto& error_value : value.as_array()) {
        auto error = parse_error(error_value);
        if (!error) {
            return std::nullopt;
        }
        result.emplace_back(std::move(error.value()));
    }
    return result;
}
} // namespace black_flow_sidecar_detail

inline BlackFlowOcrSidecar
    make_minimal_black_flow_ocr_sidecar(BlackFlowClientType client_type, const BlackFlowStoreConfigContract& config)
{
    BlackFlowOcrSidecar sidecar {
        .schema = "maa.black_flow.ocr_sidecar",
        .version = 1,
        .exploration_id = "minimal-contract-sample",
        .page_index = 1,
        .captured_at = "1970-01-01T00:00:00Z",
        .client_type = client_type,
        .language = "zh_cn",
        .image = {
            .file_name = "page-01.png",
            .width = 1280,
            .height = 720,
            .sha256 = std::string(64, '0'),
        },
        .page_status = BlackFlowPageStatus::Complete,
        .provenance = {
            .config_schema = "maa.black_flow.standard_product_names",
            .config_version = 1,
            .standard_product_names_count = static_cast<int>(config.standard_product_names.size()),
            .standard_product_names_sha256 = config.standard_product_names_sha256,
            .source_product_names_count = config.source_product_names_count,
            .source_product_names_sha256 = config.source_product_names_sha256,
            .client_data_commit = config.client_data_commit,
            .prts_synthetic_substances_revision = config.prts_synthetic_substances_revision,
            .prts_engine_accessories_revision = config.prts_engine_accessories_revision,
        },
    };
    for (size_t index = 0; index < sidecar.slots.size(); ++index) {
        sidecar.slots[index] = BlackFlowSidecarSlot {
            .index = static_cast<int>(index + 1U),
            .status = BlackFlowSlotStatus::Empty,
            .ocr_text = "",
            .ocr_score = 0.0,
            .standard_product_name = "",
            .match_kind = BlackFlowMatchKind::None,
            .errors = {},
        };
    }
    return sidecar;
}

inline json::value black_flow_ocr_sidecar_to_json(const BlackFlowOcrSidecar& sidecar)
{
    json::array slots;
    for (const auto& slot : sidecar.slots) {
        json::array errors;
        for (const auto& error : slot.errors) {
            errors.emplace_back(black_flow_sidecar_detail::error_to_json(error));
        }
        slots.emplace_back(
            json::object {
                { "index", slot.index },
                { "status", std::string(black_flow_sidecar_detail::slot_status_name(slot.status)) },
                { "ocr_text", slot.ocr_text },
                { "ocr_score", slot.ocr_score },
                { "standard_product_name", slot.standard_product_name },
                { "match_kind", std::string(black_flow_sidecar_detail::match_kind_name(slot.match_kind)) },
                { "errors", std::move(errors) },
            });
    }

    json::array errors;
    for (const auto& error : sidecar.errors) {
        errors.emplace_back(black_flow_sidecar_detail::error_to_json(error));
    }

    return json::object {
        { "schema", sidecar.schema },
        { "version", sidecar.version },
        { "exploration_id", sidecar.exploration_id },
        { "page_index", sidecar.page_index },
        { "captured_at", sidecar.captured_at },
        { "client_type", std::string(black_flow_client_type_name(sidecar.client_type)) },
        { "language", sidecar.language },
        { "image",
          json::object {
              { "file_name", sidecar.image.file_name },
              { "width", sidecar.image.width },
              { "height", sidecar.image.height },
              { "sha256", sidecar.image.sha256 },
          } },
        { "page_status", std::string(black_flow_sidecar_detail::page_status_name(sidecar.page_status)) },
        { "provenance",
          json::object {
              { "config_schema", sidecar.provenance.config_schema },
              { "config_version", sidecar.provenance.config_version },
              { "standard_product_names_count", sidecar.provenance.standard_product_names_count },
              { "standard_product_names_sha256", sidecar.provenance.standard_product_names_sha256 },
              { "source_product_names_count", sidecar.provenance.source_product_names_count },
              { "source_product_names_sha256", sidecar.provenance.source_product_names_sha256 },
              { "client_data_commit", sidecar.provenance.client_data_commit },
              { "prts_synthetic_substances_revision", sidecar.provenance.prts_synthetic_substances_revision },
              { "prts_engine_accessories_revision", sidecar.provenance.prts_engine_accessories_revision },
          } },
        { "slots", std::move(slots) },
        { "errors", std::move(errors) },
    };
}

inline std::optional<BlackFlowOcrSidecar> parse_black_flow_ocr_sidecar(const json::value& root)
{
    using namespace black_flow_sidecar_detail;
    using namespace black_flow_release_contract;

    if (!utils::has_exact_json_fields(
            root,
            std::array {
                std::string_view("schema"),
                std::string_view("version"),
                std::string_view("exploration_id"),
                std::string_view("page_index"),
                std::string_view("captured_at"),
                std::string_view("client_type"),
                std::string_view("language"),
                std::string_view("image"),
                std::string_view("page_status"),
                std::string_view("provenance"),
                std::string_view("slots"),
                std::string_view("errors"),
            }) ||
        !root.at("schema").is_string() || root.at("schema").as_string() != "maa.black_flow.ocr_sidecar" ||
        !root.at("version").is<int>() || root.at("version").as_integer() != 1 ||
        !root.at("exploration_id").is_string() || root.at("exploration_id").as_string().empty() ||
        !root.at("page_index").is<int>() || root.at("page_index").as_integer() < 1 ||
        root.at("page_index").as_integer() > 3 || !root.at("captured_at").is_string() ||
        root.at("captured_at").as_string().empty() || !root.at("client_type").is_string() ||
        !root.at("language").is_string() || root.at("language").as_string() != "zh_cn" ||
        !root.at("page_status").is_string()) {
        return std::nullopt;
    }

    const auto client_type = parse_black_flow_client_type(root.at("client_type").as_string());
    const auto page_status = parse_page_status(root.at("page_status").as_string());
    if (!client_type || !page_status) {
        return std::nullopt;
    }

    const auto& image = root.at("image");
    if (!utils::has_exact_json_fields(
            image,
            std::array {
                std::string_view("file_name"),
                std::string_view("width"),
                std::string_view("height"),
                std::string_view("sha256"),
            }) ||
        !image.at("file_name").is_string() || image.at("file_name").as_string().empty() ||
        !image.at("width").is<int>() || image.at("width").as_integer() != 1280 || !image.at("height").is<int>() ||
        image.at("height").as_integer() != 720 || !image.at("sha256").is_string() ||
        !is_lowercase_sha256(image.at("sha256").as_string())) {
        return std::nullopt;
    }

    const auto& provenance = root.at("provenance");
    if (!utils::has_exact_json_fields(
            provenance,
            std::array {
                std::string_view("config_schema"),
                std::string_view("config_version"),
                std::string_view("standard_product_names_count"),
                std::string_view("standard_product_names_sha256"),
                std::string_view("source_product_names_count"),
                std::string_view("source_product_names_sha256"),
                std::string_view("client_data_commit"),
                std::string_view("prts_synthetic_substances_revision"),
                std::string_view("prts_engine_accessories_revision"),
            }) ||
        !provenance.at("config_schema").is_string() ||
        provenance.at("config_schema").as_string() != "maa.black_flow.standard_product_names" ||
        !provenance.at("config_version").is<int>() || provenance.at("config_version").as_integer() != 1 ||
        !provenance.at("standard_product_names_count").is<int>() ||
        provenance.at("standard_product_names_count").as_integer() != StandardProductNamesCount ||
        !provenance.at("standard_product_names_sha256").is_string() ||
        provenance.at("standard_product_names_sha256").as_string() != StandardProductNamesSha256 ||
        !provenance.at("source_product_names_count").is<int>() ||
        provenance.at("source_product_names_count").as_integer() != SourceProductNamesCount ||
        !provenance.at("source_product_names_sha256").is_string() ||
        provenance.at("source_product_names_sha256").as_string() != SourceProductNamesSha256 ||
        !provenance.at("client_data_commit").is_string() ||
        provenance.at("client_data_commit").as_string() != ClientDataCommit ||
        !provenance.at("prts_synthetic_substances_revision").is<int>() ||
        provenance.at("prts_synthetic_substances_revision").as_integer() != PrtsSyntheticSubstancesRevision ||
        !provenance.at("prts_engine_accessories_revision").is<int>() ||
        provenance.at("prts_engine_accessories_revision").as_integer() != PrtsEngineAccessoriesRevision) {
        return std::nullopt;
    }

    const auto& slots = root.at("slots");
    if (!slots.is_array() || slots.as_array().size() != 10U) {
        return std::nullopt;
    }

    BlackFlowOcrSidecar result {
        .schema = root.at("schema").as_string(),
        .version = root.at("version").as_integer(),
        .exploration_id = root.at("exploration_id").as_string(),
        .page_index = root.at("page_index").as_integer(),
        .captured_at = root.at("captured_at").as_string(),
        .client_type = client_type.value(),
        .language = root.at("language").as_string(),
        .image = {
            .file_name = image.at("file_name").as_string(),
            .width = image.at("width").as_integer(),
            .height = image.at("height").as_integer(),
            .sha256 = image.at("sha256").as_string(),
        },
        .page_status = page_status.value(),
        .provenance = {
            .config_schema = provenance.at("config_schema").as_string(),
            .config_version = provenance.at("config_version").as_integer(),
            .standard_product_names_count = provenance.at("standard_product_names_count").as_integer(),
            .standard_product_names_sha256 = provenance.at("standard_product_names_sha256").as_string(),
            .source_product_names_count = provenance.at("source_product_names_count").as_integer(),
            .source_product_names_sha256 = provenance.at("source_product_names_sha256").as_string(),
            .client_data_commit = provenance.at("client_data_commit").as_string(),
            .prts_synthetic_substances_revision =
                provenance.at("prts_synthetic_substances_revision").as_integer(),
            .prts_engine_accessories_revision = provenance.at("prts_engine_accessories_revision").as_integer(),
        },
    };

    for (size_t index = 0; index < result.slots.size(); ++index) {
        const auto& slot = slots.at(index);
        if (!utils::has_exact_json_fields(
                slot,
                std::array {
                    std::string_view("index"),
                    std::string_view("status"),
                    std::string_view("ocr_text"),
                    std::string_view("ocr_score"),
                    std::string_view("standard_product_name"),
                    std::string_view("match_kind"),
                    std::string_view("errors"),
                }) ||
            !slot.at("index").is<int>() || slot.at("index").as_integer() != static_cast<int>(index + 1U) ||
            !slot.at("status").is_string() || !slot.at("ocr_text").is_string() || !slot.at("ocr_score").is_number() ||
            !std::isfinite(slot.at("ocr_score").as_double()) || slot.at("ocr_score").as_double() < 0.0 ||
            slot.at("ocr_score").as_double() > 1.0 || !slot.at("standard_product_name").is_string() ||
            !slot.at("match_kind").is_string()) {
            return std::nullopt;
        }

        const auto status = parse_slot_status(slot.at("status").as_string());
        const auto match_kind = parse_match_kind(slot.at("match_kind").as_string());
        auto errors = parse_errors(slot.at("errors"));
        if (!status || !match_kind || !errors) {
            return std::nullopt;
        }

        result.slots[index] = BlackFlowSidecarSlot {
            .index = slot.at("index").as_integer(),
            .status = status.value(),
            .ocr_text = slot.at("ocr_text").as_string(),
            .ocr_score = slot.at("ocr_score").as_double(),
            .standard_product_name = slot.at("standard_product_name").as_string(),
            .match_kind = match_kind.value(),
            .errors = std::move(errors.value()),
        };
    }

    auto errors = parse_errors(root.at("errors"));
    if (!errors) {
        return std::nullopt;
    }
    result.errors = std::move(errors.value());
    return result;
}
} // namespace asst
