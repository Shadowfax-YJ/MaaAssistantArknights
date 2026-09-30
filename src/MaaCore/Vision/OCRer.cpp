#include "OCRer.h"

#include <algorithm>
#include <cmath>
#include <shared_mutex>
#include <unordered_map>

#include <boost/regex.hpp>

#include "Config/Miscellaneous/OcrConfig.h"
#include "Config/Miscellaneous/OcrPack.h"
#include "Config/TaskData.h"
#include "MaaUtils/Encoding.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Utils/FuzzyTextMatcher.h"
#include "Utils/Logger.hpp"

using namespace asst;

OCRer::ResultsVecOpt OCRer::analyze() const
{
    if (m_roi.empty()) {
        return std::nullopt;
    }
    OcrPack* ocr_ptr = nullptr;
    if (m_params.use_char_model) {
        ocr_ptr = &CharOcr::get_instance();
    }
    else {
        ocr_ptr = &WordOcr::get_instance();
    }
    ResultsVec raw_results = ocr_ptr->recognize(make_roi(m_image, m_roi), m_params.without_det, m_roi);
    ocr_ptr = nullptr;

    /* post process */
    ResultsVec results_vec;
    for (Result& res : raw_results) {
        if (res.text.empty() || std::isnan(res.score) || std::isinf(res.score)) {
            continue;
        }

        postproc_rect_(res);
        postproc_trim_(res);
        postproc_replace_(res);

        if (!filter_and_replace_by_required_(res)) {
            continue;
        }
#ifdef ASST_DEBUG
        cv::rectangle(m_image_draw, make_rect<cv::Rect>(res.rect), cv::Scalar(0, 255, 0), 2);
        cv::putText(
            m_image_draw,
            std::to_string(res.score) + " | " + res.text,
            cv::Point(res.rect.x, res.rect.y - 5),
            cv::FONT_HERSHEY_SIMPLEX,
            0.7,
            cv::Scalar(0, 255, 0),
            2);
#endif // !ASST_DEBUG

        results_vec.emplace_back(std::move(res));
    }

    if (results_vec.empty() && m_params.join_adjacent && !m_params.without_det && !m_params.required.empty()) {
        results_vec = join_adjacent_(raw_results);
    }
    if (results_vec.empty()) {
        return std::nullopt;
    }

    Log.trace("Proceed", results_vec);

    m_result = std::move(results_vec);
    return m_result;
}

OCRer::ResultsVec OCRer::join_adjacent_(const ResultsVec& fragments) const
{
    auto sorted = fragments;
    std::erase_if(sorted, [](const auto& row) {
        return row.text.empty() || !std::isfinite(row.score) || row.rect.width <= 0 || row.rect.height <= 0;
    });
    std::ranges::sort(sorted, [](const auto& a, const auto& b) {
        return a.rect.x != b.rect.x ? a.rect.x < b.rect.x : a.rect.y < b.rect.y;
    });
    ResultsVec results;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        const auto& anchor = sorted[i].rect;
        Result joined = sorted[i];
        Rect previous = anchor;
        for (std::size_t j = i + 1; j < sorted.size(); ++j) {
            const auto& row = sorted[j];
            const auto& rect = row.rect;
            const int height = std::max(anchor.height, rect.height);
            // Compare to the first fragment so a chain cannot drift into another line.
            if (std::min(anchor.height, rect.height) * 2 < height ||
                std::abs((anchor.y * 2 + anchor.height) - (rect.y * 2 + rect.height)) * 2 > height) {
                continue;
            }
            const int gap = rect.x - (previous.x + previous.width);
            if (gap > std::min(previous.height, rect.height) / 2) {
                break;
            }
            if (rect.x <= previous.x || gap < -std::min(previous.width, rect.width) / 3) {
                continue;
            }
            joined.text += row.text;
            joined.rect = Rect::bounding_box(joined.rect, rect);
            joined.score = std::min(joined.score, row.score);
            previous = rect;
            auto candidate = joined;
            // Joining must produce a complete configured title; do not fuzzy-match fragments.
            if (filter_and_replace_by_required_(candidate, false)) {
                results.emplace_back(std::move(candidate));
                break;
            }
        }
    }
    if (!results.empty() &&
        std::ranges::any_of(results, [&](const auto& row) { return row.text != results.front().text; })) {
        return { }; // Conflicting complete titles do not establish one page identity.
    }
    return results;
}

void OCRer::postproc_rect_(Result& res) const
{
    if (m_params.without_det) {
        res.rect = m_roi;
    }
    else {
        res.rect.x += m_roi.x;
        res.rect.y += m_roi.y;
    }
}

void OCRer::postproc_trim_(Result& res) const
{
    utils::string_trim(res.text);
}

static const boost::wregex& gen_regex(const std::wstring& pattern)
{
    static std::shared_mutex mtx;
    static std::unordered_map<std::wstring, boost::wregex> s_cache;

    {
        std::shared_lock slock(mtx);
        if (auto it = s_cache.find(pattern); it != s_cache.end()) {
            return it->second;
        }
    }

    std::unique_lock ulock(mtx);
    return s_cache.emplace(pattern, boost::wregex(pattern)).first->second;
}

void OCRer::postproc_replace_(Result& res) const
{
    if (m_params.replace.empty()) {
        return;
    }

    std::wstring text_u16 = MAA_NS::to_u16(res.text);
    for (const auto& [regex, new_str] : m_params.replace) {
        std::wstring regex_u16 = MAA_NS::to_u16(regex);
        std::wstring new_str_u16 = MAA_NS::to_u16(new_str);
        if (m_params.replace_full) {
            if (boost::regex_search(text_u16, gen_regex(regex_u16))) {
                text_u16 = new_str_u16;
            }
        }
        else {
            text_u16 = boost::regex_replace(text_u16, gen_regex(regex_u16), new_str_u16);
        }
    }
    res.text = MAA_NS::from_u16(text_u16);
}

bool OCRer::filter_and_replace_by_required_(Result& res, bool allow_fuzzy_match) const
{
    if (m_params.required.empty()) {
        return true;
    }
    auto& ocr_config = OcrConfig::get_instance();
    auto equ_text = ocr_config.process_equivalence_class(res.text);

    if (m_params.fuzzy_match && allow_fuzzy_match) {
        std::vector<std::string> candidates;
        candidates.reserve(m_params.required.size());
        for (const auto& candidate : m_params.required) {
            candidates.emplace_back(candidate.second);
        }
        utils::FuzzyTextMatchSettings settings;
        settings.minimum_fuzzy_candidate_length = m_params.fuzzy_match_min_length;
        const utils::FuzzyTextMatch match = utils::fuzzy_match_ocr_text(equ_text, candidates, settings);
        if (!match.accepted) {
            Log.debug(
                "OCR fuzzy match rejected",
                "captured",
                res.text,
                "best",
                match.canonical,
                "similarity",
                match.similarity,
                "runner up",
                match.runner_up,
                "margin",
                match.similarity - match.runner_up_similarity);
            return false;
        }
        const auto canonical = std::ranges::find_if(m_params.required, [&](const auto& candidate) {
            return candidate.second == match.canonical;
        });
        if (canonical == m_params.required.end()) {
            return false;
        }
        const std::string captured = res.text;
        res.text = canonical->first;
        if (!match.exact) {
            Log.info(
                "OCR fuzzy matched",
                "captured",
                captured,
                "canonical",
                res.text,
                "distance",
                match.edit_distance,
                "similarity",
                match.similarity);
        }
        return true;
    }

    if (!allow_fuzzy_match) {
        const auto exact = std::ranges::find_if(m_params.required, [&](const auto& candidate) {
            return candidate.second == equ_text;
        });
        if (exact == m_params.required.end()) {
            return false;
        }
        res.text = exact->first;
        return true;
    }
    if (m_params.full_match) {
        auto required = m_params.required | std::views::transform([&](const auto& str) { return str.second; });
        return std::ranges::find(required, equ_text) != required.end();
    }
    else {
        auto is_sub = [&](const auto& p) -> bool {
            if (equ_text.find(p.second) == std::string::npos) {
                return false;
            }
            res.text = p.first;
            return true;
        };
        return std::ranges::find_if(m_params.required, is_sub) != m_params.required.cend();
    };
}
