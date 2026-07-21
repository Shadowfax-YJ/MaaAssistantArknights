#include "BlackFlowStoreImageAnalyzer.h"

#include "Config/Miscellaneous/BlackFlowStoreConfig.h"
#include "MaaUtils/ImageIo.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Utils/Sha256.hpp"
#include "Vision/RegionOCRer.h"

#include <algorithm>
#include <array>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <utility>

namespace
{
asst::BlackFlowStoreSlotOcr run_word_ocr(const cv::Mat& normalized_frame, const asst::BlackFlowStoreTitleRoi& roi)
{
    asst::BlackFlowStoreSlotOcr outcome;
    try {
        const asst::Rect rectangle(roi.x, roi.y, roi.width, roi.height);
        asst::RegionOCRer ocr(normalized_frame, rectangle);
        ocr.set_bin_threshold(171, 255);
        ocr.set_use_char_model(false);
        if (auto result = ocr.analyze()) {
            outcome.fragments.emplace_back(
                asst::BlackFlowStoreOcrFragment {
                    .x = result->rect.x - roi.x,
                    .text = std::move(result->text),
                    .score = result->score,
                });
        }
    }
    catch (const std::exception& exception) {
        outcome.succeeded = false;
        outcome.error_message = exception.what();
    }
    return outcome;
}

bool has_normalized_size(const cv::Mat& image)
{
    return !image.empty() && image.cols == 1280 && image.rows == 720;
}

bool cancellation_requested(const asst::BlackFlowStoreCancellationRequested& predicate) noexcept
{
    if (!predicate) {
        return false;
    }
    try {
        return predicate();
    }
    catch (...) {
        return true;
    }
}

#ifdef ASST_BUILD_SMOKE_TEST
using ExpectedSlots = std::array<std::string_view, 10>;

struct ExpectedSample
{
    std::string_view file_name;
    ExpectedSlots slots;
};

constexpr std::array<ExpectedSample, 8> ExpectedSamples { {
    {
        "e01-p01-runtime.png",
        { "支援起重机", "报废轮子", "近卫招募券", "沙盘β", "悲伤的红", "止痛片", "残弩-新典训", "窜天源石虫", "", "" },
    },
    {
        "e01-p02-runtime.png",
        { "支援地雷组", "浪花", "先锋招募券", "热辣可可", "沙盘β", "断杖-波纹", "医者-新典训", "凉拌海草", "", "" },
    },
    {
        "e01-p03-runtime.png",
        { "支援雾机",
          "试作外骨骼",
          "医疗招募券",
          "竞技场贵宾券",
          "残弩-神速",
          "沙盘β",
          "玻璃小鸟",
          "锈刃-新典训",
          "",
          "" },
    },
    {
        "e01-p04-runtime.png",
        { "支援轰隆隆",
          "浪花",
          "医疗招募券",
          "热辣可可",
          "叙拉古人的愤怒",
          "种植者保单",
          "三尺万象",
          "支柱-新典训",
          "",
          "" },
    },
    {
        "e01-p04-user.png",
        { "支援轰隆隆",
          "浪花",
          "医疗招募券",
          "热辣可可",
          "叙拉古人的愤怒",
          "种植者保单",
          "三尺万象",
          "支柱-新典训",
          "",
          "" },
    },
    {
        "e02-p01-runtime.png",
        { "支援轰隆隆",
          "白模狗",
          "术师招募券",
          "沙盘β",
          "迷藏",
          "止痛片",
          "静谧扩香石",
          "古怪的长笛",
          "种植者保单",
          "医者-新典训" },
    },
    {
        "e02-p02-runtime.png",
        { "支援轰隆隆",
          "种子",
          "辅助招募券",
          "万星园之辉",
          "钝爪-先机",
          "沙盘β",
          "活木甲",
          "“老妈的鼓励”",
          "医者-新典训",
          "锈刃-突破" },
    },
    {
        "e02-p03-user.png",
        { "支援起重机",
          "多生苔藓",
          "先锋招募券",
          "《归来》",
          "佣兵的饰物",
          "沙盘β",
          "迷藏",
          "城墙之子",
          "锈刃-新典训",
          "医者-地缘策略" },
    },
} };

constexpr std::array<std::pair<std::string_view, std::string_view>, 3> ExpectedWordOcrResources { {
    { "PaddleOCR/det/inference.onnx", "d572c1773fd00e72f2f2a4c6399513223c49d70f64bc8ccf52fc6cc500b2803c" },
    { "PaddleOCR/rec/inference.onnx", "ece6e0173b177a79358b7610524d768711c7c887895f1b2b767e2bed83ec88cf" },
    { "PaddleOCR/rec/keys.txt", "38055a5ea5937ac7ea96f114fb2deaccab8c32f89d94a05a31acbf1293f9f83c" },
} };

bool has_expected_word_ocr_resources(const std::filesystem::path& resource_directory)
{
    for (const auto& [relative_path, expected_sha256] : ExpectedWordOcrResources) {
        const auto path = resource_directory / relative_path;
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            std::cerr << "BlackFlow OCR resource is missing: " << path << std::endl;
            return false;
        }
        const std::string bytes { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
        const auto actual_sha256 = asst::utils::sha256(bytes);
        if (actual_sha256 != expected_sha256) {
            std::cerr << "BlackFlow OCR resource fingerprint changed: " << path << " expected=" << expected_sha256
                      << " actual=" << actual_sha256 << std::endl;
            return false;
        }
    }
    return true;
}
#endif
} // namespace

asst::BlackFlowStoreImageAnalyzer::BlackFlowStoreImageAnalyzer(
    const BlackFlowStoreConfigContract& config,
    BlackFlowWordOcr word_ocr) :
    m_matcher(config.standard_product_names),
    m_word_ocr(word_ocr ? std::move(word_ocr) : BlackFlowWordOcr(run_word_ocr))
{
}

std::optional<asst::BlackFlowStoreFrameObservation>
    asst::BlackFlowStoreImageAnalyzer::observe(const cv::Mat& normalized_frame, BlackFlowStoreReadiness readiness) const
{
    if (!has_normalized_size(normalized_frame)) {
        return std::nullopt;
    }

    BlackFlowStoreFrameObservation observation {
        .store_anchor_visible = readiness.store_anchor_visible,
        .refresh_control_visible = readiness.refresh_control_visible,
        .overlay_visible = readiness.overlay_visible,
    };
    size_t fingerprint_offset = 0;
    try {
        for (size_t slot_index = 0; slot_index < BlackFlowStoreTitleRois.size(); ++slot_index) {
            const auto& roi = BlackFlowStoreTitleRois[slot_index];
            const cv::Rect rectangle(roi.x, roi.y, roi.width, roi.height);
            const auto title = normalized_frame(rectangle);
            cv::Mat grayscale;
            if (title.channels() == 1) {
                grayscale = title;
            }
            else if (title.channels() == 3) {
                cv::cvtColor(title, grayscale, cv::COLOR_BGR2GRAY);
            }
            else if (title.channels() == 4) {
                cv::cvtColor(title, grayscale, cv::COLOR_BGRA2GRAY);
            }
            else {
                return std::nullopt;
            }

            cv::Mat binary;
            cv::threshold(grayscale, binary, 170, 1, cv::THRESH_BINARY);
            observation.title_foreground[slot_index] = cv::countNonZero(binary) != 0;
            for (int y = 0; y < binary.rows; ++y) {
                const auto* row = binary.ptr<std::uint8_t>(y);
                std::copy_n(row, binary.cols, observation.title_fingerprint.begin() + fingerprint_offset);
                fingerprint_offset += static_cast<size_t>(binary.cols);
            }
        }
    }
    catch (const cv::Exception&) {
        return std::nullopt;
    }
    return observation;
}

asst::BlackFlowStoreSlotsAnalysis asst::BlackFlowStoreImageAnalyzer::analyze_slots(const cv::Mat& stable_frame) const
{
    return analyze_slots(stable_frame, { });
}

asst::BlackFlowStoreSlotsAnalysis asst::BlackFlowStoreImageAnalyzer::analyze_slots(
    const cv::Mat& stable_frame,
    const BlackFlowStoreCancellationRequested& cancel_requested) const
{
    if (cancellation_requested(cancel_requested)) {
        return { };
    }
    const auto observation = observe(stable_frame, { });
    if (!observation || cancellation_requested(cancel_requested)) {
        return { };
    }

    std::array<BlackFlowStoreSlotOcr, 10> ocr;
    for (size_t index = 0; index < ocr.size(); ++index) {
        if (cancellation_requested(cancel_requested)) {
            return { };
        }
        try {
            ocr[index] = m_word_ocr(stable_frame, BlackFlowStoreTitleRois[index]);
        }
        catch (const std::exception& exception) {
            ocr[index].succeeded = false;
            ocr[index].error_message = exception.what();
        }
    }
    if (cancellation_requested(cancel_requested)) {
        return { };
    }

    auto result = analyze_black_flow_store_slots(observation->title_foreground, ocr, m_matcher);
    const bool has_foreground = std::ranges::any_of(observation->title_foreground, std::identity { });
    const bool has_ocr_fragment = std::ranges::any_of(ocr, [](const auto& slot) { return !slot.fragments.empty(); });
    if (has_foreground && !has_ocr_fragment) {
        result.page_status = BlackFlowAnalyzedPageStatus::Failed;
    }
    return result;
}

#ifdef ASST_BUILD_SMOKE_TEST
bool asst::run_black_flow_store_fixture_smoke_test(const std::filesystem::path& fixture_directory)
{
    if (!has_expected_word_ocr_resources(fixture_directory.parent_path() / "resource")) {
        return false;
    }
    const BlackFlowStoreImageAnalyzer analyzer(BlackFlowStoreConfig::get_instance().get_data());
    size_t correct_nonempty = 0;
    size_t correct_empty = 0;
    size_t wrong_fuzzy = 0;

    for (const auto& sample : ExpectedSamples) {
        const auto image = MAA_NS::imread(fixture_directory / sample.file_name);
        if (!has_normalized_size(image)) {
            std::cerr << "BlackFlow fixture is missing or not 1280x720: " << sample.file_name << std::endl;
            return false;
        }

        const auto analysis = analyzer.analyze_slots(image);
        for (size_t index = 0; index < sample.slots.size(); ++index) {
            const auto& expected = sample.slots[index];
            const auto& actual = analysis.slots[index];
            if (expected.empty()) {
                if (actual.status == BlackFlowAnalyzedSlotStatus::Empty) {
                    ++correct_empty;
                }
                else {
                    std::cerr << "BlackFlow empty-slot mismatch: " << sample.file_name << " slot " << index + 1U
                              << " OCR='" << actual.ocr_text << "'" << std::endl;
                }
                continue;
            }

            if (actual.status == BlackFlowAnalyzedSlotStatus::Matched && actual.standard_product_name == expected) {
                ++correct_nonempty;
            }
            else {
                std::cerr << "BlackFlow product mismatch: " << sample.file_name << " slot " << index + 1U
                          << " expected='" << expected << "' OCR='" << actual.ocr_text << "' actual='"
                          << actual.standard_product_name << "'" << std::endl;
            }
            if (actual.match_kind == BlackFlowProductNameMatchKind::Fuzzy && actual.standard_product_name != expected) {
                ++wrong_fuzzy;
            }
        }
    }

    std::cout << "BlackFlow fixture gate: correct_nonempty=" << correct_nonempty << ", correct_empty=" << correct_empty
              << ", wrong_fuzzy=" << wrong_fuzzy << std::endl;
    return correct_nonempty == 70U && correct_empty == 10U && wrong_fuzzy == 0U;
}
#endif
