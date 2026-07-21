#include "Config/Miscellaneous/BlackFlowStoreConfigContract.hpp"
#include "Task/Miscellaneous/BlackFlowStorePageRepository.hpp"
#include "Vision/Roguelike/BlackFlowProductNameMatcher.hpp"
#include "Vision/Roguelike/BlackFlowStorePageAnalyzer.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <meojson/json.hpp>

namespace
{
class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        m_path = std::filesystem::temp_directory_path() /
                 ("maa-black-flow-store-page-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        REQUIRE(std::filesystem::create_directory(m_path));
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(m_path); }

    const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

asst::BlackFlowStoreConfigContract release_config()
{
    const auto path = std::filesystem::path(MAA_TEST_RESOURCE_DIR) / "black_flow" / "standard_product_names.json";
    const auto document = json::open(path, true, true);
    REQUIRE(document.has_value());
    const auto parsed = asst::parse_black_flow_store_config(document.value());
    REQUIRE(parsed.has_value());
    return parsed.value();
}

std::string read_bytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("BlackFlow exact matching honors its score boundary and preserves sand-table variants")
{
    const std::array<std::string, 3> standard_product_names { "沙盘α", "沙盘β", "凉拌海草" };
    const asst::BlackFlowProductNameMatcher matcher(standard_product_names);

    const auto accepted = matcher.match("沙盘β", 0.75);
    CHECK(accepted.kind == asst::BlackFlowProductNameMatchKind::Exact);
    CHECK(accepted.standard_product_name == "沙盘β");

    const auto rejected = matcher.match("沙盘β", 0.749999);
    CHECK(rejected.kind == asst::BlackFlowProductNameMatchKind::Unmatched);
    CHECK(rejected.standard_product_name.empty());
}

TEST_CASE("BlackFlow matching applies only the approved OCR normalization rules")
{
    const std::array<std::string, 3> standard_product_names { "凉拌海草", "“老妈的鼓励”", "沙盘α" };
    const asst::BlackFlowProductNameMatcher matcher(standard_product_names);

    const auto suffixed = matcher.match("凉 拌　海草-α", 0.9);
    CHECK(suffixed.kind == asst::BlackFlowProductNameMatchKind::Exact);
    CHECK(suffixed.standard_product_name == "凉拌海草");

    const auto other_greek_letter = matcher.match("凉拌海草-δ", 0.9);
    CHECK(other_greek_letter.kind == asst::BlackFlowProductNameMatchKind::Exact);
    CHECK(other_greek_letter.standard_product_name == "凉拌海草");

    const auto greek_modifier_letter = matcher.match("凉拌海草-ͺ", 0.9);
    CHECK(greek_modifier_letter.kind == asst::BlackFlowProductNameMatchKind::Exact);
    CHECK(greek_modifier_letter.standard_product_name == "凉拌海草");

    const auto greek_symbol = matcher.match("凉拌海草-϶", 0.9);
    CHECK(greek_symbol.kind == asst::BlackFlowProductNameMatchKind::Unmatched);

    const auto quoted = matcher.match("\"老妈的鼓励\"", 0.9);
    CHECK(quoted.kind == asst::BlackFlowProductNameMatchKind::Exact);
    CHECK(quoted.standard_product_name == "“老妈的鼓励”");

    const auto unsuffixed = matcher.match("沙盘α", 0.9);
    CHECK(unsuffixed.kind == asst::BlackFlowProductNameMatchKind::Exact);
    CHECK(unsuffixed.standard_product_name == "沙盘α");
}

TEST_CASE("BlackFlow matching normalizes standard names and OCR text to Unicode NFC")
{
    const std::array<std::string, 1> decomposed_standard_product_names { "Café" };
    const asst::BlackFlowProductNameMatcher matcher(decomposed_standard_product_names);

    const auto matched = matcher.match("Café", 0.75);
    CHECK(matched.kind == asst::BlackFlowProductNameMatchKind::Exact);
    CHECK(matched.standard_product_name == "Café");
}

TEST_CASE("BlackFlow NFC handles canonical ordering Hangul and normalized Greek suffixes")
{
    const std::array<std::string, 3> standard_product_names { "ṩ", "가", "凉拌海草" };
    const asst::BlackFlowProductNameMatcher matcher(standard_product_names);

    CHECK(matcher.match("s\xCC\x87\xCC\xA3", 0.75).standard_product_name == "ṩ");
    CHECK(matcher.match("가", 0.75).standard_product_name == "가");
    CHECK(matcher.match("凉拌海草-ά", 0.75).standard_product_name == "凉拌海草");
}

TEST_CASE("BlackFlow NFC does not apply compatibility folding and rejects invalid UTF-8")
{
    const std::array<std::string, 2> standard_product_names { "A", "ff" };
    const asst::BlackFlowProductNameMatcher matcher(standard_product_names);

    CHECK(matcher.match("Ａ", 0.99).kind == asst::BlackFlowProductNameMatchKind::Unmatched);
    CHECK(matcher.match("ﬀ", 0.99).kind == asst::BlackFlowProductNameMatchKind::Unmatched);
    const std::string invalid_utf8("\xC3\x28", 2);
    CHECK(matcher.match(invalid_utf8, 0.99).kind == asst::BlackFlowProductNameMatchKind::Unmatched);
}

TEST_CASE("BlackFlow standard-name normalization deduplicates canonically equivalent entries")
{
    const std::array<std::string, 2> duplicate_names { "Café", "Café" };
    const asst::BlackFlowProductNameMatcher matcher(duplicate_names);

    const auto matched = matcher.match("Cafè", 0.99);
    CHECK(matched.kind == asst::BlackFlowProductNameMatchKind::Fuzzy);
    CHECK(matched.standard_product_name == "Café");
}

TEST_CASE("BlackFlow fuzzy matching enforces score length and candidate-lead boundaries")
{
    const std::array<std::string, 5> standard_product_names {
        "支援起重机", "凉拌海草", "沙盘α", "甲乙丙丁戊己庚辛", "甲乙丙戊",
    };
    const asst::BlackFlowProductNameMatcher matcher(standard_product_names);

    const auto accepted = matcher.match("支援起重几", 0.85);
    CHECK(accepted.kind == asst::BlackFlowProductNameMatchKind::Fuzzy);
    CHECK(accepted.standard_product_name == "支援起重机");

    CHECK(matcher.match("支援起重几", 0.849999).kind == asst::BlackFlowProductNameMatchKind::Unmatched);
    CHECK(matcher.match("沙盘β", 0.99).kind == asst::BlackFlowProductNameMatchKind::Unmatched);

    const auto two_edits = matcher.match("甲乙丙丁戊己壬癸", 0.99);
    CHECK(two_edits.kind == asst::BlackFlowProductNameMatchKind::Fuzzy);
    CHECK(two_edits.standard_product_name == "甲乙丙丁戊己庚辛");

    const std::array<std::string, 2> ambiguous_names { "甲乙丙丁", "甲乙丙戊" };
    const asst::BlackFlowProductNameMatcher ambiguous_matcher(ambiguous_names);
    CHECK(ambiguous_matcher.match("甲乙丙己", 0.99).kind == asst::BlackFlowProductNameMatchKind::Unmatched);

    const std::array<std::string, 1> ascii_quote_names { "\"甲乙丙丁X\"" };
    const asst::BlackFlowProductNameMatcher ascii_quote_matcher(ascii_quote_names);
    const auto quote_fuzzy = ascii_quote_matcher.match("\"甲乙丙丁\"", 0.99);
    CHECK(quote_fuzzy.kind == asst::BlackFlowProductNameMatchKind::Fuzzy);
    CHECK(quote_fuzzy.standard_product_name == "\"甲乙丙丁X\"");
}

TEST_CASE("BlackFlow page classification honors the stable and changed fingerprint boundaries")
{
    asst::BlackFlowStoreFrameObservation first;
    first.store_anchor_visible = true;
    first.refresh_control_visible = true;

    auto stable = first;
    for (size_t index = 0; index < 267U; ++index) {
        stable.title_fingerprint[index] = 1U;
    }
    CHECK(
        asst::classify_black_flow_store_page(first, stable, std::nullopt) ==
        asst::BlackFlowStorePageClassification::StableInitial);

    auto unstable = stable;
    unstable.title_fingerprint[267] = 1U;
    CHECK(
        asst::classify_black_flow_store_page(first, unstable, std::nullopt) ==
        asst::BlackFlowStorePageClassification::Unstable);

    asst::BlackFlowStoreTitleFingerprint last_committed;
    auto changed = stable;
    for (size_t index = 267U; index < 1068U; ++index) {
        changed.title_fingerprint[index] = 1U;
    }
    CHECK(
        asst::classify_black_flow_store_page(changed, changed, last_committed) ==
        asst::BlackFlowStorePageClassification::StableNew);

    changed.title_fingerprint[1067] = 0U;
    CHECK(
        asst::classify_black_flow_store_page(changed, changed, last_committed) ==
        asst::BlackFlowStorePageClassification::StableOld);
}

TEST_CASE("BlackFlow slot analysis preserves fixed order raw OCR and empty-unmatched semantics")
{
    const std::array<std::string, 1> standard_product_names { "支援起重机" };
    const asst::BlackFlowProductNameMatcher matcher(standard_product_names);
    std::array<asst::BlackFlowStoreSlotOcr, 10> ocr;
    std::array<bool, 10> foreground { };

    foreground[0] = true;
    ocr[0].fragments = {
        { .x = 90, .text = "重机", .score = 0.95 },
        { .x = 5, .text = "支援起", .score = 0.88 },
    };

    foreground[1] = true;
    ocr[1].fragments = { { .x = 1, .text = "不可匹配原文", .score = 0.4 } };

    foreground[2] = true;

    foreground[3] = true;
    ocr[3].succeeded = false;
    ocr[3].error_message = "Word OCR unavailable";

    const auto result = asst::analyze_black_flow_store_slots(foreground, ocr, matcher);

    CHECK(result.page_status == asst::BlackFlowAnalyzedPageStatus::Partial);
    CHECK(result.slots[0].status == asst::BlackFlowAnalyzedSlotStatus::Matched);
    CHECK(result.slots[0].ocr_text == "支援起重机");
    CHECK(result.slots[0].ocr_score == 0.88);
    CHECK(result.slots[0].standard_product_name == "支援起重机");
    CHECK(result.slots[1].status == asst::BlackFlowAnalyzedSlotStatus::Unmatched);
    CHECK(result.slots[1].ocr_text == "不可匹配原文");
    CHECK(result.slots[2].status == asst::BlackFlowAnalyzedSlotStatus::OcrError);
    CHECK(result.slots[3].status == asst::BlackFlowAnalyzedSlotStatus::OcrError);
    CHECK(result.slots[4].status == asst::BlackFlowAnalyzedSlotStatus::Empty);
}

TEST_CASE("BlackFlow page repository commits one immutable same-stem PNG and sidecar")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path).starts_with("valid-png")
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    const auto exploration = repository.begin_exploration(asst::BlackFlowClientType::Official);
    REQUIRE(exploration.has_value());

    asst::BlackFlowStoreSlotsAnalysis analysis;
    analysis.page_status = asst::BlackFlowAnalyzedPageStatus::Complete;
    for (auto& slot : analysis.slots) {
        slot.status = asst::BlackFlowAnalyzedSlotStatus::Empty;
    }
    const auto analyze = [&](const std::filesystem::path& committed_png) {
        CHECK(committed_png.filename() == "page-01.png");
        CHECK(read_bytes(committed_png) == "valid-png-first");
        return analysis;
    };

    const std::string first_png = "valid-png-first";
    const auto committed =
        repository.capture_page(exploration.value(), 1, 1, std::as_bytes(std::span(first_png)), analyze);

    CHECK(committed.disposition == asst::BlackFlowJsonDisposition::FirstCommit);
    CHECK(committed.advances_completed_pages);
    CHECK(committed.should_notify);
    CHECK(committed.png_relative_path.stem() == committed.json_relative_path.stem());
    CHECK(std::filesystem::exists(temporary.path() / committed.png_relative_path));
    CHECK(std::filesystem::exists(temporary.path() / committed.json_relative_path));

    const auto sidecar = json::open(temporary.path() / committed.json_relative_path, true, true);
    REQUIRE(sidecar.has_value());
    CHECK(sidecar->at("client_type").as_string() == "Official");
    CHECK(sidecar->at("slots").as_array().size() == 10U);
    const auto captured_at = sidecar->at("captured_at").as_string();
    CHECK(captured_at.size() == 24U);
    CHECK(captured_at.at(19) == '.');
    CHECK(captured_at.back() == 'Z');

    const std::string second_png = "valid-png-second";
    const auto conflict =
        repository.capture_page(exploration.value(), 1, 2, std::as_bytes(std::span(second_png)), analyze);
    CHECK(conflict.disposition == asst::BlackFlowJsonDisposition::Conflict);
    CHECK_FALSE(conflict.should_notify);
    CHECK(read_bytes(temporary.path() / committed.png_relative_path) == first_png);
}

TEST_CASE("BlackFlow page repository serializes concurrent same-page capture conflicts")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path) == "valid-png"
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };

    std::mutex coordination_mutex;
    std::condition_variable coordination;
    bool context_committed = false;
    bool competitor_finished = false;
    const auto pause_after_context = [&](asst::BlackFlowStoreCommitCheckpoint checkpoint) {
        if (checkpoint != asst::BlackFlowStoreCommitCheckpoint::ContextCommitted) {
            return;
        }
        std::unique_lock lock(coordination_mutex);
        context_committed = true;
        coordination.notify_all();
        coordination.wait_for(lock, std::chrono::milliseconds(200), [&] { return competitor_finished; });
    };

    asst::BlackFlowStorePageRepository first_repository(temporary.path(), config, png_verifier, pause_after_context);
    asst::BlackFlowStorePageRepository competing_repository(temporary.path(), config, png_verifier);
    const auto exploration = first_repository.begin_exploration(asst::BlackFlowClientType::Official);
    REQUIRE(exploration.has_value());

    asst::BlackFlowStoreSlotsAnalysis complete;
    complete.page_status = asst::BlackFlowAnalyzedPageStatus::Complete;
    for (auto& slot : complete.slots) {
        slot.status = asst::BlackFlowAnalyzedSlotStatus::Empty;
    }
    const std::string png = "valid-png";
    const auto analyze = [&](const std::filesystem::path&) {
        return complete;
    };

    auto first_capture = std::async(std::launch::async, [&] {
        return first_repository.capture_page(exploration.value(), 1, 1, std::as_bytes(std::span(png)), analyze);
    });
    {
        std::unique_lock lock(coordination_mutex);
        REQUIRE(coordination.wait_for(lock, std::chrono::seconds(1), [&] { return context_committed; }));
    }
    auto competing_capture = std::async(std::launch::async, [&] {
        auto result =
            competing_repository.capture_page(exploration.value(), 1, 2, std::as_bytes(std::span(png)), analyze);
        {
            std::scoped_lock lock(coordination_mutex);
            competitor_finished = true;
        }
        coordination.notify_all();
        return result;
    });

    const auto first = first_capture.get();
    const auto competing = competing_capture.get();
    CHECK(first.disposition == asst::BlackFlowJsonDisposition::FirstCommit);
    CHECK(first.should_notify);
    CHECK(competing.disposition == asst::BlackFlowJsonDisposition::Conflict);
    CHECK_FALSE(competing.should_notify);
}

TEST_CASE("BlackFlow page repository replaces only an improving sidecar during reprocessing")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path) == "valid-png"
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    const auto exploration = repository.begin_exploration(asst::BlackFlowClientType::Bilibili);
    REQUIRE(exploration.has_value());

    const auto partial_with_errors = [](size_t error_count) {
        asst::BlackFlowStoreSlotsAnalysis analysis;
        analysis.page_status = asst::BlackFlowAnalyzedPageStatus::Partial;
        for (auto& slot : analysis.slots) {
            slot.status = asst::BlackFlowAnalyzedSlotStatus::Empty;
        }
        for (size_t index = 0; index < error_count; ++index) {
            analysis.slots[index].status = asst::BlackFlowAnalyzedSlotStatus::OcrError;
            analysis.slots[index].error_message = "retryable OCR error";
        }
        return analysis;
    };

    const std::string png = "valid-png";
    const auto first = repository.capture_page(
        exploration.value(),
        1,
        1,
        std::as_bytes(std::span(png)),
        [&](const std::filesystem::path&) { return partial_with_errors(2); });
    REQUIRE(first.disposition == asst::BlackFlowJsonDisposition::FirstCommit);
    const auto png_path = temporary.path() / first.png_relative_path;
    const auto json_path = temporary.path() / first.json_relative_path;
    const auto original_png = read_bytes(png_path);
    const auto original_json = read_bytes(json_path);

    const auto improved =
        repository.reprocess_page(exploration.value(), 1, 2, [&](const std::filesystem::path& committed_png) {
            CHECK(committed_png == png_path);
            return partial_with_errors(1);
        });
    CHECK(improved.disposition == asst::BlackFlowJsonDisposition::Improved);
    CHECK(improved.should_notify);
    CHECK_FALSE(improved.advances_completed_pages);
    CHECK(read_bytes(png_path) == original_png);
    CHECK(read_bytes(json_path) != original_json);
    const auto improved_json = read_bytes(json_path);

    const auto not_improved = repository.reprocess_page(exploration.value(), 1, 3, [&](const std::filesystem::path&) {
        return partial_with_errors(2);
    });
    CHECK(not_improved.disposition == asst::BlackFlowJsonDisposition::Unchanged);
    CHECK_FALSE(not_improved.should_notify);
    CHECK(read_bytes(png_path) == original_png);
    CHECK(read_bytes(json_path) == improved_json);

    auto failed_analysis = partial_with_errors(10);
    failed_analysis.page_status = asst::BlackFlowAnalyzedPageStatus::Failed;
    const auto failed_page = repository.capture_page(
        exploration.value(),
        2,
        1,
        std::as_bytes(std::span(png)),
        [&](const std::filesystem::path&) { return failed_analysis; });
    REQUIRE(failed_page.disposition == asst::BlackFlowJsonDisposition::FirstCommit);
    CHECK_FALSE(failed_page.advances_completed_pages);

    const auto recovered_page = repository.reprocess_page(exploration.value(), 2, 2, [&](const std::filesystem::path&) {
        return partial_with_errors(1);
    });
    CHECK(recovered_page.disposition == asst::BlackFlowJsonDisposition::Improved);
    CHECK(recovered_page.advances_completed_pages);
    CHECK(recovered_page.should_notify);
}

TEST_CASE("BlackFlow page repository serializes concurrent improve-only reprocessing")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path) == "valid-png"
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    const auto partial_with_errors = [](size_t error_count) {
        asst::BlackFlowStoreSlotsAnalysis analysis;
        analysis.page_status = asst::BlackFlowAnalyzedPageStatus::Partial;
        for (auto& slot : analysis.slots) {
            slot.status = asst::BlackFlowAnalyzedSlotStatus::Empty;
        }
        for (size_t index = 0; index < error_count; ++index) {
            analysis.slots[index].status = asst::BlackFlowAnalyzedSlotStatus::OcrError;
            analysis.slots[index].error_message = "retryable OCR error";
        }
        return analysis;
    };

    asst::BlackFlowStorePageRepository initial_repository(temporary.path(), config, png_verifier);
    const auto exploration = initial_repository.begin_exploration(asst::BlackFlowClientType::Official);
    REQUIRE(exploration.has_value());
    const std::string png = "valid-png";
    const auto initial = initial_repository.capture_page(
        exploration.value(),
        1,
        1,
        std::as_bytes(std::span(png)),
        [&](const std::filesystem::path&) { return partial_with_errors(3); });
    REQUIRE(initial.disposition == asst::BlackFlowJsonDisposition::FirstCommit);

    std::mutex coordination_mutex;
    std::condition_variable coordination;
    bool complete_analyzer_entered = false;
    bool partial_analyzer_entered = false;
    bool complete_replaced = false;
    const auto signal_complete_replaced = [&](asst::BlackFlowStoreCommitCheckpoint checkpoint) {
        if (checkpoint == asst::BlackFlowStoreCommitCheckpoint::JsonReplaced) {
            {
                std::scoped_lock lock(coordination_mutex);
                complete_replaced = true;
            }
            coordination.notify_all();
        }
    };

    asst::BlackFlowStorePageRepository complete_repository(
        temporary.path(),
        config,
        png_verifier,
        signal_complete_replaced);
    asst::BlackFlowStorePageRepository partial_repository(temporary.path(), config, png_verifier);
    asst::BlackFlowStoreSlotsAnalysis complete;
    complete.page_status = asst::BlackFlowAnalyzedPageStatus::Complete;
    for (auto& slot : complete.slots) {
        slot.status = asst::BlackFlowAnalyzedSlotStatus::Empty;
    }

    auto complete_reprocess = std::async(std::launch::async, [&] {
        return complete_repository.reprocess_page(exploration.value(), 1, 2, [&](const std::filesystem::path&) {
            std::unique_lock lock(coordination_mutex);
            complete_analyzer_entered = true;
            coordination.notify_all();
            coordination.wait_for(lock, std::chrono::milliseconds(200), [&] { return partial_analyzer_entered; });
            return complete;
        });
    });
    {
        std::unique_lock lock(coordination_mutex);
        REQUIRE(coordination.wait_for(lock, std::chrono::seconds(1), [&] { return complete_analyzer_entered; }));
    }
    auto partial_reprocess = std::async(std::launch::async, [&] {
        return partial_repository.reprocess_page(exploration.value(), 1, 3, [&](const std::filesystem::path&) {
            std::unique_lock lock(coordination_mutex);
            partial_analyzer_entered = true;
            coordination.notify_all();
            coordination.wait_for(lock, std::chrono::seconds(1), [&] { return complete_replaced; });
            return partial_with_errors(2);
        });
    });

    const auto complete_result = complete_reprocess.get();
    const auto partial_result = partial_reprocess.get();
    CHECK(complete_result.disposition == asst::BlackFlowJsonDisposition::Improved);
    CHECK(partial_result.disposition == asst::BlackFlowJsonDisposition::Unchanged);
    const auto final_sidecar = json::open(temporary.path() / initial.json_relative_path, true, true);
    REQUIRE(final_sidecar.has_value());
    CHECK(final_sidecar->at("page_status").as_string() == "complete");
}

TEST_CASE("BlackFlow page repository completes an orphan PNG from its durable capture context")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path) == "valid-png"
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    const auto fail_json = [](asst::BlackFlowStoreCommitCheckpoint checkpoint) {
        if (checkpoint == asst::BlackFlowStoreCommitCheckpoint::JsonStaged) {
            throw std::runtime_error("injected JSON failure");
        }
    };
    asst::BlackFlowStorePageRepository interrupted(temporary.path(), config, png_verifier, fail_json);
    const auto exploration = interrupted.begin_exploration(asst::BlackFlowClientType::Official);
    REQUIRE(exploration.has_value());

    asst::BlackFlowStoreSlotsAnalysis complete;
    complete.page_status = asst::BlackFlowAnalyzedPageStatus::Complete;
    for (auto& slot : complete.slots) {
        slot.status = asst::BlackFlowAnalyzedSlotStatus::Empty;
    }
    const std::string png = "valid-png";
    const auto failed = interrupted.capture_page(
        exploration.value(),
        1,
        1,
        std::as_bytes(std::span(png)),
        [&](const std::filesystem::path&) { return complete; });
    CHECK(failed.disposition == asst::BlackFlowJsonDisposition::None);
    CHECK(std::filesystem::exists(temporary.path() / failed.png_relative_path));
    CHECK_FALSE(std::filesystem::exists(temporary.path() / failed.json_relative_path));

    const auto context_path = temporary.path() / exploration->id() / ".page-01.capture-context.json";
    CHECK(std::filesystem::exists(context_path));

    asst::BlackFlowStorePageRepository recovered(temporary.path(), config, png_verifier);
    const auto committed =
        recovered.reprocess_page(exploration.value(), 1, 2, [&](const std::filesystem::path&) { return complete; });
    CHECK(committed.disposition == asst::BlackFlowJsonDisposition::FirstCommit);
    CHECK(committed.advances_completed_pages);
    CHECK(committed.should_notify);
    CHECK(std::filesystem::exists(temporary.path() / committed.json_relative_path));
    CHECK_FALSE(std::filesystem::exists(context_path));
}

TEST_CASE("BlackFlow page repository reconciles a fault after an improving JSON replacement")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path) == "valid-png"
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    const auto partial_with_errors = [](size_t error_count) {
        asst::BlackFlowStoreSlotsAnalysis analysis;
        analysis.page_status = asst::BlackFlowAnalyzedPageStatus::Partial;
        for (auto& slot : analysis.slots) {
            slot.status = asst::BlackFlowAnalyzedSlotStatus::Empty;
        }
        for (size_t index = 0; index < error_count; ++index) {
            analysis.slots[index].status = asst::BlackFlowAnalyzedSlotStatus::OcrError;
            analysis.slots[index].error_message = "retryable OCR error";
        }
        return analysis;
    };

    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    const auto exploration = repository.begin_exploration(asst::BlackFlowClientType::Official);
    REQUIRE(exploration.has_value());
    const std::string png = "valid-png";
    const auto first = repository.capture_page(
        exploration.value(),
        1,
        1,
        std::as_bytes(std::span(png)),
        [&](const std::filesystem::path&) { return partial_with_errors(2); });
    REQUIRE(first.disposition == asst::BlackFlowJsonDisposition::FirstCommit);

    const auto fail_after_replace = [](asst::BlackFlowStoreCommitCheckpoint checkpoint) {
        if (checkpoint == asst::BlackFlowStoreCommitCheckpoint::JsonReplaced) {
            throw std::runtime_error("injected post-replacement failure");
        }
    };
    asst::BlackFlowStorePageRepository interrupted(temporary.path(), config, png_verifier, fail_after_replace);
    const auto reconciled = interrupted.reprocess_page(exploration.value(), 1, 2, [&](const std::filesystem::path&) {
        return partial_with_errors(1);
    });

    CHECK(reconciled.disposition == asst::BlackFlowJsonDisposition::Improved);
    CHECK(reconciled.should_notify);
    CHECK_FALSE(reconciled.advances_completed_pages);

    auto failed_analysis = partial_with_errors(10);
    failed_analysis.page_status = asst::BlackFlowAnalyzedPageStatus::Failed;
    const auto failed_page = repository.capture_page(
        exploration.value(),
        2,
        1,
        std::as_bytes(std::span(png)),
        [&](const std::filesystem::path&) { return failed_analysis; });
    REQUIRE(failed_page.disposition == asst::BlackFlowJsonDisposition::FirstCommit);

    const auto recovered = interrupted.reprocess_page(exploration.value(), 2, 2, [&](const std::filesystem::path&) {
        return partial_with_errors(1);
    });
    CHECK(recovered.disposition == asst::BlackFlowJsonDisposition::Improved);
    CHECK(recovered.advances_completed_pages);
    CHECK(recovered.should_notify);
}

TEST_CASE("BlackFlow page repository cleans or reconciles every initial commit checkpoint")
{
    struct CheckpointExpectation
    {
        asst::BlackFlowStoreCommitCheckpoint checkpoint;
        bool png_committed;
        bool json_committed;
        bool context_retained;
    };

    constexpr std::array expectations {
        CheckpointExpectation { asst::BlackFlowStoreCommitCheckpoint::ContextCommitted, false, false, false },
        CheckpointExpectation { asst::BlackFlowStoreCommitCheckpoint::PngStaged, false, false, false },
        CheckpointExpectation { asst::BlackFlowStoreCommitCheckpoint::PngValidated, false, false, false },
        CheckpointExpectation { asst::BlackFlowStoreCommitCheckpoint::PngCommitted, true, false, true },
        CheckpointExpectation { asst::BlackFlowStoreCommitCheckpoint::PageAnalyzed, true, false, true },
        CheckpointExpectation { asst::BlackFlowStoreCommitCheckpoint::JsonStaged, true, false, true },
        CheckpointExpectation { asst::BlackFlowStoreCommitCheckpoint::JsonValidated, true, false, true },
        CheckpointExpectation { asst::BlackFlowStoreCommitCheckpoint::JsonCommitted, true, true, false },
    };

    for (const auto& expectation : expectations) {
        CAPTURE(static_cast<int>(expectation.checkpoint));
        TemporaryDirectory temporary;
        const auto config = release_config();
        const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
            return read_bytes(path) == "valid-png"
                       ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                       : std::nullopt;
        };
        const auto fail_at_checkpoint = [&](asst::BlackFlowStoreCommitCheckpoint checkpoint) {
            if (checkpoint == expectation.checkpoint) {
                throw std::runtime_error("injected commit checkpoint failure");
            }
        };
        asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier, fail_at_checkpoint);
        const auto exploration = repository.begin_exploration(asst::BlackFlowClientType::Official);
        REQUIRE(exploration.has_value());

        asst::BlackFlowStoreSlotsAnalysis complete;
        complete.page_status = asst::BlackFlowAnalyzedPageStatus::Complete;
        for (auto& slot : complete.slots) {
            slot.status = asst::BlackFlowAnalyzedSlotStatus::Empty;
        }
        const std::string png = "valid-png";
        const auto result = repository.capture_page(
            exploration.value(),
            1,
            1,
            std::as_bytes(std::span(png)),
            [&](const std::filesystem::path&) { return complete; });

        const auto exploration_directory = temporary.path() / exploration->id();
        const auto png_path = exploration_directory / "page-01.png";
        const auto json_path = exploration_directory / "page-01.json";
        const auto context_path = exploration_directory / ".page-01.capture-context.json";
        CHECK(std::filesystem::exists(png_path) == expectation.png_committed);
        CHECK(std::filesystem::exists(json_path) == expectation.json_committed);
        CHECK(std::filesystem::exists(context_path) == expectation.context_retained);

        size_t temporary_file_count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(exploration_directory)) {
            temporary_file_count += entry.path().filename().string().find(".tmp.") != std::string::npos ? 1U : 0U;
        }
        CHECK(temporary_file_count == 0U);

        if (expectation.json_committed) {
            CHECK(result.disposition == asst::BlackFlowJsonDisposition::FirstCommit);
            CHECK(result.advances_completed_pages);
            CHECK(result.should_notify);
        }
        else {
            CHECK(result.disposition == asst::BlackFlowJsonDisposition::None);
            CHECK_FALSE(result.advances_completed_pages);
            CHECK_FALSE(result.should_notify);
        }
    }
}

TEST_CASE("BlackFlow startup recovery processes retryable pages from oldest to newest")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path) == "valid-png"
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);

    const auto retryable_analysis = [](size_t error_count) {
        asst::BlackFlowStoreSlotsAnalysis analysis;
        analysis.page_status = asst::BlackFlowAnalyzedPageStatus::Partial;
        for (auto& slot : analysis.slots) {
            slot.status = asst::BlackFlowAnalyzedSlotStatus::Empty;
        }
        for (size_t index = 0; index < error_count; ++index) {
            analysis.slots[index].status = asst::BlackFlowAnalyzedSlotStatus::OcrError;
            analysis.slots[index].error_message = "retryable OCR error";
        }
        return analysis;
    };

    const auto newer = repository.begin_exploration(asst::BlackFlowClientType::Official);
    const auto older = repository.begin_exploration(asst::BlackFlowClientType::Bilibili);
    REQUIRE(newer.has_value());
    REQUIRE(older.has_value());
    const std::string png = "valid-png";
    const auto newer_page =
        repository.capture_page(newer.value(), 1, 1, std::as_bytes(std::span(png)), [&](const std::filesystem::path&) {
            return retryable_analysis(2);
        });
    const auto older_page =
        repository.capture_page(older.value(), 1, 1, std::as_bytes(std::span(png)), [&](const std::filesystem::path&) {
            return retryable_analysis(3);
        });
    REQUIRE(newer_page.disposition == asst::BlackFlowJsonDisposition::FirstCommit);
    REQUIRE(older_page.disposition == asst::BlackFlowJsonDisposition::FirstCommit);

    const auto set_captured_at = [&](const std::filesystem::path& relative_path, std::string_view captured_at) {
        const auto path = temporary.path() / relative_path;
        auto document = json::open(path, true, true);
        REQUIRE(document.has_value());
        document.value()["captured_at"] = std::string(captured_at);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << document->format() << '\n';
    };
    set_captured_at(newer_page.json_relative_path, "2026-07-21T00:00:02.000Z");
    set_captured_at(older_page.json_relative_path, "2026-07-21T00:00:01.000Z");

    std::vector<std::string> analyzed_explorations;
    std::vector<asst::BlackFlowStoreRecoveryCommit> commits;
    const auto summary = repository.recover_pending_pages(
        [&](const std::filesystem::path& path, const asst::BlackFlowStoreStopRequested&) {
            analyzed_explorations.push_back(path.parent_path().filename().string());
            return retryable_analysis(1);
        },
        [] { return std::chrono::steady_clock::time_point { }; },
        [] { return false; },
        [&](const asst::BlackFlowStoreRecoveryCommit& commit) { commits.push_back(commit); });

    CHECK(analyzed_explorations == std::vector<std::string> { older->id(), newer->id() });
    CHECK(summary.candidates_found == 2);
    CHECK(summary.candidates_processed == 2);
    CHECK(summary.commits_published == 2);
    CHECK(summary.candidates_failed == 0);
    REQUIRE(commits.size() == 2);
    CHECK(commits[0].exploration_id == older->id());
    CHECK(commits[0].result.disposition == asst::BlackFlowJsonDisposition::Improved);
    CHECK(commits[1].exploration_id == newer->id());
    CHECK(commits[1].result.disposition == asst::BlackFlowJsonDisposition::Improved);
}

TEST_CASE("BlackFlow startup recovery processes at most twenty candidates")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path) == "valid-png"
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);

    asst::BlackFlowStoreSlotsAnalysis retryable;
    retryable.page_status = asst::BlackFlowAnalyzedPageStatus::Partial;
    for (auto& slot : retryable.slots) {
        slot.status = asst::BlackFlowAnalyzedSlotStatus::Empty;
    }
    retryable.slots.front().status = asst::BlackFlowAnalyzedSlotStatus::OcrError;
    retryable.slots.front().error_message = "retryable OCR error";
    const std::string png = "valid-png";

    for (size_t exploration_index = 0; exploration_index < 7; ++exploration_index) {
        const auto exploration = repository.begin_exploration(asst::BlackFlowClientType::Official);
        REQUIRE(exploration.has_value());
        for (int page_index = 1; page_index <= 3; ++page_index) {
            const auto committed = repository.capture_page(
                exploration.value(),
                page_index,
                1,
                std::as_bytes(std::span(png)),
                [&](const std::filesystem::path&) { return retryable; });
            REQUIRE(committed.disposition == asst::BlackFlowJsonDisposition::FirstCommit);
        }
    }

    asst::BlackFlowStoreSlotsAnalysis complete;
    complete.page_status = asst::BlackFlowAnalyzedPageStatus::Complete;
    for (auto& slot : complete.slots) {
        slot.status = asst::BlackFlowAnalyzedSlotStatus::Empty;
    }
    size_t analyzed = 0;
    const auto summary = repository.recover_pending_pages(
        [&](const std::filesystem::path&, const asst::BlackFlowStoreStopRequested&) {
            ++analyzed;
            return complete;
        },
        [] { return std::chrono::steady_clock::time_point { }; },
        [] { return false; },
        { });

    CHECK(summary.candidates_found == 21);
    CHECK(summary.candidates_processed == 20);
    CHECK(summary.commits_published == 20);
    CHECK(summary.candidates_failed == 0);
    CHECK(summary.candidate_limit_reached);
    CHECK_FALSE(summary.time_limit_reached);
    CHECK(analyzed == 20);
}

TEST_CASE("BlackFlow startup recovery isolates a bad candidate and cancels analysis at thirty seconds")
{
    TemporaryDirectory temporary;
    const auto config = release_config();
    const auto png_verifier = [](const std::filesystem::path& path) -> std::optional<asst::BlackFlowPngDimensions> {
        return read_bytes(path) == "valid-png"
                   ? std::optional(asst::BlackFlowPngDimensions { .width = 1280, .height = 720 })
                   : std::nullopt;
    };
    asst::BlackFlowStorePageRepository repository(temporary.path(), config, png_verifier);
    const auto exploration = repository.begin_exploration(asst::BlackFlowClientType::Official);
    REQUIRE(exploration.has_value());

    asst::BlackFlowStoreSlotsAnalysis retryable;
    retryable.page_status = asst::BlackFlowAnalyzedPageStatus::Partial;
    for (auto& slot : retryable.slots) {
        slot.status = asst::BlackFlowAnalyzedSlotStatus::Empty;
    }
    retryable.slots.front().status = asst::BlackFlowAnalyzedSlotStatus::OcrError;
    retryable.slots.front().error_message = "retryable OCR error";
    const std::string png = "valid-png";
    for (int page_index = 1; page_index <= 3; ++page_index) {
        const auto committed = repository.capture_page(
            exploration.value(),
            page_index,
            1,
            std::as_bytes(std::span(png)),
            [&](const std::filesystem::path&) { return retryable; });
        REQUIRE(committed.disposition == asst::BlackFlowJsonDisposition::FirstCommit);
    }

    asst::BlackFlowStoreSlotsAnalysis complete;
    complete.page_status = asst::BlackFlowAnalyzedPageStatus::Complete;
    for (auto& slot : complete.slots) {
        slot.status = asst::BlackFlowAnalyzedSlotStatus::Empty;
    }
    auto current_time = std::chrono::steady_clock::time_point { };
    size_t analyzed = 0;
    bool cancellation_observed = false;
    std::vector<int> committed_pages;
    const auto page_two_path = temporary.path() / exploration->id() / "page-02.json";
    const auto original_page_two_sidecar = read_bytes(page_two_path);
    const auto summary = repository.recover_pending_pages(
        [&](const std::filesystem::path&, const asst::BlackFlowStoreStopRequested& cancel_requested) {
            ++analyzed;
            if (analyzed == 1U) {
                current_time += std::chrono::seconds(1);
                throw std::runtime_error("isolated analyzer failure");
            }
            for (size_t elapsed_seconds = 0; elapsed_seconds < 60U; ++elapsed_seconds) {
                if (cancel_requested()) {
                    cancellation_observed = true;
                    break;
                }
                current_time += std::chrono::seconds(1);
            }
            return complete;
        },
        [&] { return current_time; },
        [] { return false; },
        [&](const asst::BlackFlowStoreRecoveryCommit& commit) { committed_pages.push_back(commit.page_index); });

    CHECK(summary.candidates_found == 3);
    CHECK(summary.candidates_processed == 2);
    CHECK(summary.commits_published == 0);
    CHECK(summary.candidates_failed == 2);
    CHECK_FALSE(summary.candidate_limit_reached);
    CHECK(summary.time_limit_reached);
    CHECK(analyzed == 2);
    CHECK(cancellation_observed);
    CHECK(committed_pages.empty());
    CHECK(current_time == std::chrono::steady_clock::time_point { } + std::chrono::seconds(30));
    CHECK(read_bytes(page_two_path) == original_page_two_sidecar);
    const auto page_two = json::open(page_two_path, true, true);
    REQUIRE(page_two.has_value());
    CHECK(page_two->at("page_status").as_string() == "partial");
}
