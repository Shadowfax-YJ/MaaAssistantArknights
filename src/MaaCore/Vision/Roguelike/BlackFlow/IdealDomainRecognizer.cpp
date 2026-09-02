#include "Vision/Roguelike/BlackFlow/IdealDomainRecognizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <numeric>
#include <set>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>

namespace asst::blackflow::perception
{
namespace
{
constexpr std::size_t RawFeatureCount = 116;
constexpr std::size_t ModelFeatureCount = 232;
constexpr double OrdinaryCombinedMargin = 0.10;
constexpr double HopefulSoilCombinedMargin = 0.03;
constexpr double HeadDisagreementMargin = 0.02;
constexpr double DomainObservationThreshold = 0.50;

using RawFeature = std::array<float, RawFeatureCount>;
using ModelFeature = std::array<float, ModelFeatureCount>;

struct PositionedNode
{
    IdealCoordinate position;
    cv::Point2f center;
};

double percentile(std::vector<float> values, double quantile)
{
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    const double position = static_cast<double>(values.size() - 1) * quantile;
    const std::size_t lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
    const double fraction = position - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

double median(std::vector<float> values)
{
    return percentile(std::move(values), 0.5);
}

double node_pitch(const std::vector<PositionedNode>& nodes)
{
    std::vector<float> differences;
    for (int axis = 0; axis < 2; ++axis) {
        std::vector<float> levels;
        levels.reserve(nodes.size());
        for (const auto& node : nodes) {
            const float raw = axis == 0 ? node.center.x : node.center.y;
            levels.emplace_back(std::round(raw * 1000.0F) / 1000.0F);
        }
        std::ranges::sort(levels);
        levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
        for (std::size_t index = 1; index < levels.size(); ++index) {
            const float difference = levels[index] - levels[index - 1];
            if (difference > 40.0F) {
                differences.emplace_back(difference);
            }
        }
    }
    if (differences.empty()) {
        throw std::runtime_error("ideal-domain node pitch is unavailable");
    }
    return median(std::move(differences));
}

struct SpatialMasks
{
    std::array<std::vector<int>, 4> regions;
    std::array<std::vector<int>, 16> sectors;
};

const SpatialMasks& spatial_masks()
{
    static const SpatialMasks masks = []() {
        SpatialMasks result;
        constexpr double normalized_pitch = 96.0 / 1.36;
        for (int y = 0; y < 96; ++y) {
            for (int x = 0; x < 96; ++x) {
                const int index = y * 96 + x;
                const double dx = x - 47.5;
                const double dy = y - 47.5;
                const double distance = std::hypot(dx, dy);
                if (distance <= normalized_pitch * 0.18) {
                    result.regions[0].emplace_back(index);
                }
                if (distance > normalized_pitch * 0.18 && distance <= normalized_pitch * 0.32) {
                    result.regions[1].emplace_back(index);
                }
                if (distance > normalized_pitch * 0.32 && distance <= normalized_pitch * 0.52) {
                    result.regions[2].emplace_back(index);
                }
                if (distance > normalized_pitch * 0.30 && distance <= normalized_pitch * 0.62 &&
                    std::abs(dx) > normalized_pitch * 0.12 && std::abs(dy) > normalized_pitch * 0.12) {
                    result.regions[3].emplace_back(index);
                }
                if (distance > normalized_pitch * 0.28 && distance < normalized_pitch * 0.60) {
                    const double angle = (std::atan2(dy, dx) + std::numbers::pi) / (2.0 * std::numbers::pi);
                    result.sectors[std::min(15, static_cast<int>(std::floor(angle * 16.0)))].emplace_back(index);
                }
            }
        }
        return result;
    }();
    return masks;
}

std::array<float, 12> frequency_features(const cv::Mat& gray)
{
    cv::Mat base = gray(cv::Rect(16, 16, 64, 64)).clone();
    cv::Scalar mean;
    cv::Scalar deviation;
    cv::meanStdDev(base, mean, deviation);
    std::array<float, 64> window {};
    for (int index = 0; index < 64; ++index) {
        window[index] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * index / 63.0));
    }

    std::array<float, 12> output {};
    for (int normalized = 0; normalized < 2; ++normalized) {
        cv::Mat windowed(64, 64, CV_32F);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                float value = base.at<float>(y, x);
                if (normalized != 0) {
                    value = static_cast<float>((value - mean[0]) / (deviation[0] + 1e-4));
                }
                windowed.at<float>(y, x) = value * window[y] * window[x];
            }
        }
        cv::Mat complex;
        cv::dft(windowed, complex, cv::DFT_COMPLEX_OUTPUT);
        std::array<double, 6> bands {};
        double total = 1e-6;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const int shifted_x = (x + 32) % 64;
                const int shifted_y = (y + 32) % 64;
                const cv::Vec2f frequency = complex.at<cv::Vec2f>(shifted_y, shifted_x);
                const double magnitude = std::log1p(std::hypot(frequency[0], frequency[1]));
                const double radius = std::hypot(x - 31.5, y - 31.5) / 32.0;
                if (radius > 0.03) {
                    total += magnitude;
                }
                const int band = radius < 0.08   ? 0
                                 : radius < 0.16 ? 1
                                 : radius < 0.28 ? 2
                                 : radius < 0.42 ? 3
                                 : radius < 0.65 ? 4
                                 : radius < 1.5  ? 5
                                                 : -1;
                if (band >= 0) {
                    bands[band] += magnitude;
                }
            }
        }
        for (int band = 0; band < 6; ++band) {
            output[normalized * 6 + band] = static_cast<float>(bands[band] / total);
        }
    }
    return output;
}

RawFeature node_feature(const cv::Mat& image, const cv::Point2f center, double pitch)
{
    const int side = std::max(48, static_cast<int>(std::round(pitch * 1.36)));
    cv::Mat patch;
    const cv::Mat transform = (cv::Mat_<double>(2, 3) << 1.0,
                               0.0,
                               (side - 1) / 2.0 - center.x,
                               0.0,
                               1.0,
                               (side - 1) / 2.0 - center.y);
    cv::warpAffine(image, patch, transform, cv::Size(side, side), cv::INTER_LINEAR, cv::BORDER_CONSTANT);
    cv::resize(patch, patch, cv::Size(96, 96), 0.0, 0.0, cv::INTER_AREA);

    cv::Mat gray8;
    cv::Mat gray;
    cv::Mat hsv;
    cv::Mat blurred;
    cv::cvtColor(patch, gray8, cv::COLOR_BGR2GRAY);
    gray8.convertTo(gray, CV_32F, 1.0 / 255.0);
    cv::cvtColor(patch, hsv, cv::COLOR_BGR2HSV);
    cv::GaussianBlur(gray, blurred, cv::Size(), 3.0, 3.0, cv::BORDER_DEFAULT);

    cv::Mat dx;
    cv::Mat dy;
    cv::Mat gradient;
    cv::Mat laplacian;
    cv::Sobel(gray, dx, CV_32F, 1, 0, 3, 1.0, 0.0, cv::BORDER_REFLECT_101);
    cv::Sobel(gray, dy, CV_32F, 0, 1, 3, 1.0, 0.0, cv::BORDER_REFLECT_101);
    cv::magnitude(dx, dy, gradient);
    const cv::Mat laplacian_kernel = (cv::Mat_<float>(3, 3) << 2, 0, 2, 0, -8, 0, 2, 0, 2);
    cv::filter2D(gray, laplacian, CV_32F, laplacian_kernel, cv::Point(-1, -1), 0.0, cv::BORDER_REFLECT_101);
    laplacian = cv::abs(laplacian);
    cv::Mat highpass = cv::abs(gray - blurred);

    cv::Mat saturation(96, 96, CV_32F);
    cv::Mat value(96, 96, CV_32F);
    for (int y = 0; y < 96; ++y) {
        for (int x = 0; x < 96; ++x) {
            const cv::Vec3b pixel = hsv.at<cv::Vec3b>(y, x);
            saturation.at<float>(y, x) = pixel[1] / 255.0F;
            value.at<float>(y, x) = pixel[2] / 255.0F;
        }
    }

    const std::array<const cv::Mat*, 6> channels { &gray, &saturation, &value, &gradient, &laplacian, &highpass };
    const auto& masks = spatial_masks();
    RawFeature result {};
    std::size_t output = 0;
    for (const auto& region : masks.regions) {
        for (const cv::Mat* channel : channels) {
            std::vector<float> values;
            values.reserve(region.size());
            double sum = 0.0;
            for (const int index : region) {
                const float sample = channel->at<float>(index / 96, index % 96);
                values.emplace_back(sample);
                sum += sample;
            }
            const double mean_value = sum / std::max<std::size_t>(1, values.size());
            double squared = 0.0;
            for (const float sample : values) {
                squared += (sample - mean_value) * (sample - mean_value);
            }
            result[output++] = static_cast<float>(mean_value);
            result[output++] = static_cast<float>(std::sqrt(squared / std::max<std::size_t>(1, values.size())));
            result[output++] = static_cast<float>(percentile(std::move(values), 0.9));
        }
        int white = 0;
        int colorful = 0;
        for (const int index : region) {
            const float sat = saturation.at<float>(index / 96, index % 96);
            const float val = value.at<float>(index / 96, index % 96);
            white += sat < 0.22F && val > 0.55F;
            colorful += sat > 0.25F && val > 0.25F;
        }
        result[output++] = static_cast<float>(white) / region.size();
        result[output++] = static_cast<float>(colorful) / region.size();
    }

    const std::array<const cv::Mat*, 4> sector_channels { &gradient, &highpass, &value, &saturation };
    for (const cv::Mat* channel : sector_channels) {
        std::vector<float> energies;
        energies.reserve(masks.sectors.size());
        for (const auto& sector : masks.sectors) {
            double sum = 0.0;
            for (const int index : sector) {
                sum += channel->at<float>(index / 96, index % 96);
            }
            energies.emplace_back(static_cast<float>(sum / std::max<std::size_t>(1, sector.size())));
        }
        const double mean_value = std::accumulate(energies.begin(), energies.end(), 0.0) / energies.size();
        double squared = 0.0;
        for (const float energy : energies) {
            squared += (energy - mean_value) * (energy - mean_value);
        }
        result[output++] = static_cast<float>(mean_value);
        result[output++] = static_cast<float>(std::sqrt(squared / energies.size()));
        result[output++] = static_cast<float>(percentile(energies, 0.25));
        result[output++] = static_cast<float>(percentile(energies, 0.50));
        result[output++] = static_cast<float>(percentile(energies, 0.75));
        result[output++] = *std::ranges::max_element(energies);
    }
    const auto frequency = frequency_features(gray);
    for (const float feature : frequency) {
        result[output++] = feature;
    }
    if (output != RawFeatureCount) {
        throw std::runtime_error("ideal-domain feature count mismatch");
    }
    return result;
}

std::vector<ModelFeature> feature_matrix(const cv::Mat& image, const std::vector<PositionedNode>& nodes)
{
    const double pitch = node_pitch(nodes);
    std::vector<RawFeature> raw;
    raw.reserve(nodes.size());
    for (const auto& node : nodes) {
        raw.emplace_back(node_feature(image, node.center, pitch));
    }
    std::array<float, RawFeatureCount> medians {};
    std::array<float, RawFeatureCount> deviations {};
    for (std::size_t feature = 0; feature < RawFeatureCount; ++feature) {
        std::vector<float> values;
        values.reserve(raw.size());
        for (const auto& row : raw) {
            values.emplace_back(row[feature]);
        }
        medians[feature] = static_cast<float>(median(values));
        for (float& value : values) {
            value = std::abs(value - medians[feature]);
        }
        deviations[feature] = std::max(1e-4F, static_cast<float>(median(std::move(values)) * 1.4826));
    }
    std::vector<ModelFeature> result(raw.size());
    for (std::size_t row = 0; row < raw.size(); ++row) {
        for (std::size_t feature = 0; feature < RawFeatureCount; ++feature) {
            result[row][feature] = raw[row][feature];
            result[row][RawFeatureCount + feature] =
                std::clamp((raw[row][feature] - medians[feature]) / deviations[feature], -10.0F, 10.0F);
        }
    }
    return result;
}

int manhattan(const IdealCoordinate& left, const IdealCoordinate& right)
{
    return std::abs(left.row - right.row) + std::abs(left.column - right.column);
}

std::vector<double> standardize(const std::vector<double>& values)
{
    const double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    double squared = 0.0;
    for (const double value : values) {
        squared += (value - mean) * (value - mean);
    }
    const double deviation = std::sqrt(squared / values.size());
    std::vector<double> result;
    result.reserve(values.size());
    for (const double value : values) {
        result.emplace_back((value - mean) / (deviation + 1e-6));
    }
    return result;
}

std::pair<std::size_t, double> top_two(const std::vector<double>& values)
{
    std::vector<std::size_t> order(values.size());
    std::iota(order.begin(), order.end(), 0);
    std::ranges::sort(order, [&](std::size_t left, std::size_t right) {
        return values[left] != values[right] ? values[left] > values[right] : left < right;
    });
    return { order.front(), values[order.front()] - values[order[1]] };
}

std::vector<float> read_f32_tensor(
    const std::vector<std::uint8_t>& bytes,
    std::size_t data_start,
    const nlohmann::json& header,
    std::string_view name,
    std::size_t expected)
{
    const auto found = header.find(std::string(name));
    if (found == header.end() || found->value("dtype", std::string {}) != "F32") {
        throw std::runtime_error("missing F32 tensor " + std::string(name));
    }
    const auto offsets = found->at("data_offsets");
    if (!offsets.is_array() || offsets.size() != 2) {
        throw std::runtime_error("invalid tensor offsets " + std::string(name));
    }
    const std::size_t first = data_start + offsets[0].get<std::size_t>();
    const std::size_t last = data_start + offsets[1].get<std::size_t>();
    if (last < first || last > bytes.size() || last - first != expected * sizeof(float)) {
        throw std::runtime_error("invalid tensor size " + std::string(name));
    }
    std::vector<float> result(expected);
    std::memcpy(result.data(), bytes.data() + first, last - first);
    return result;
}
} // namespace

bool IdealDomainRecognizer::load(const std::filesystem::path& model_path, std::string& error)
{
    try {
        std::ifstream input(model_path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot open ideal-domain safetensors: " + model_path.string());
        }
        std::vector<std::uint8_t> bytes(
            (std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        if (bytes.size() < sizeof(std::uint64_t)) {
            throw std::runtime_error("ideal-domain safetensors header is truncated");
        }
        std::uint64_t header_size = 0;
        std::memcpy(&header_size, bytes.data(), sizeof(header_size));
        const std::size_t data_start = sizeof(header_size) + static_cast<std::size_t>(header_size);
        if (data_start > bytes.size()) {
            throw std::runtime_error("ideal-domain safetensors header length is invalid");
        }
        const nlohmann::json header = nlohmann::json::parse(
            bytes.begin() + sizeof(header_size),
            bytes.begin() + data_start);
        std::unordered_map<std::string, RidgeHead> heads;
        for (const std::string prefix : {
                 "ideal/ordinary/source",
                 "ideal/ordinary/domain",
                 "ideal/hope/filter",
                 "ideal/hope/contrast",
             }) {
            RidgeHead head;
            head.mean = read_f32_tensor(bytes, data_start, header, prefix + "/mean", ModelFeatureCount);
            head.scale = read_f32_tensor(bytes, data_start, header, prefix + "/scale", ModelFeatureCount);
            head.coefficient =
                read_f32_tensor(bytes, data_start, header, prefix + "/coefficient", ModelFeatureCount + 1);
            heads.emplace(prefix, std::move(head));
        }
        m_heads = std::move(heads);
        m_loaded = true;
        return true;
    }
    catch (const std::exception& exception) {
        error = "BlackFlow ideal-domain initialization failed: " + std::string(exception.what());
        m_heads.clear();
        m_loaded = false;
        return false;
    }
}

IdealDomainRecognition IdealDomainRecognizer::recognize(
    const cv::Mat& canonical_bgr,
    const std::vector<Node>& nodes,
    int difficulty,
    std::string_view ideology,
    std::string_view policy) const
{
    IdealDomainRecognition result;
    result.ideology = ideology;
    result.policy = policy;
    if (!m_loaded || ideology.empty()) {
        result.reason = !m_loaded ? "ideal_model_not_loaded" : "utopia_title_not_observed";
        return result;
    }
    try {
        std::vector<PositionedNode> positions;
        for (const Node& node : nodes) {
            if (node.exists) {
                positions.emplace_back(PositionedNode { { node.row, node.column }, node.center });
            }
        }
        if (positions.size() < 2) {
            result.reason = "ideal_domain_has_too_few_nodes";
            return result;
        }
        const auto features = feature_matrix(canonical_bgr, positions);
        const int radius = difficulty >= 12 ? 3 : 2;
        const bool hopeful_soil = ideology == "hopeful-soil";

        const auto probabilities = [&](std::string_view prefix, const std::vector<ModelFeature>& rows) {
            const RidgeHead& head = m_heads.at(std::string(prefix));
            std::vector<double> output;
            output.reserve(rows.size());
            for (const auto& row : rows) {
                double score = head.coefficient.front();
                for (std::size_t index = 0; index < ModelFeatureCount; ++index) {
                    const double normalized = std::clamp(
                        (row[index] - head.mean[index]) / head.scale[index],
                        -10.0F,
                        10.0F);
                    score += normalized * head.coefficient[index + 1];
                }
                score = std::clamp(score, -20.0, 20.0);
                output.emplace_back(1.0 / (1.0 + std::exp(-score)));
            }
            return output;
        };
        const auto domain_scores = [&](const std::vector<double>& probability) {
            std::vector<double> output;
            output.reserve(positions.size());
            for (const auto& source : positions) {
                double score = 0.0;
                for (std::size_t index = 0; index < positions.size(); ++index) {
                    const bool inside = manhattan(source.position, positions[index].position) <= radius;
                    score += std::log((inside ? probability[index] : 1.0 - probability[index]) + 1e-5);
                }
                output.emplace_back(score);
            }
            return output;
        };

        std::vector<double> first;
        std::vector<double> second;
        std::vector<double> domain_observation;
        if (!hopeful_soil) {
            const auto source_probability = probabilities("ideal/ordinary/source", features);
            domain_observation = probabilities("ideal/ordinary/domain", features);
            std::vector<double> log_source;
            log_source.reserve(source_probability.size());
            for (const double probability : source_probability) {
                log_source.emplace_back(std::log(probability + 1e-5));
            }
            first = standardize(log_source);
            second = standardize(domain_scores(domain_observation));
        }
        else {
            domain_observation = probabilities("ideal/hope/filter", features);
            first = standardize(domain_scores(domain_observation));
            std::vector<ModelFeature> differences;
            std::vector<std::pair<std::size_t, std::size_t>> pairs;
            for (std::size_t left = 0; left < features.size(); ++left) {
                for (std::size_t right = left + 1; right < features.size(); ++right) {
                    ModelFeature difference {};
                    for (std::size_t index = 0; index < ModelFeatureCount; ++index) {
                        difference[index] = features[left][index] - features[right][index];
                    }
                    differences.emplace_back(difference);
                    pairs.emplace_back(left, right);
                }
            }
            const auto contrast = probabilities("ideal/hope/contrast", differences);
            std::vector<double> contrast_scores;
            contrast_scores.reserve(positions.size());
            for (const auto& source : positions) {
                double score = 0.0;
                int count = 0;
                for (std::size_t index = 0; index < pairs.size(); ++index) {
                    const auto [left, right] = pairs[index];
                    const bool left_inside = manhattan(source.position, positions[left].position) <= radius;
                    const bool right_inside = manhattan(source.position, positions[right].position) <= radius;
                    if (left_inside == right_inside) {
                        continue;
                    }
                    score += std::log((left_inside ? contrast[index] : 1.0 - contrast[index]) + 1e-5);
                    ++count;
                }
                contrast_scores.emplace_back(count == 0 ? 0.0 : score / count);
            }
            second = standardize(contrast_scores);
        }

        std::vector<double> combined(first.size());
        for (std::size_t index = 0; index < first.size(); ++index) {
            combined[index] = 0.5 * first[index] + 0.5 * second[index];
        }
        const auto [combined_index, combined_margin] = top_two(combined);
        const auto [first_index, first_margin] = top_two(first);
        const auto [second_index, second_margin] = top_two(second);
        result.score_margin = combined_margin;
        result.heads_agree = first_index == second_index;
        if (combined_margin < (hopeful_soil ? HopefulSoilCombinedMargin : OrdinaryCombinedMargin)) {
            result.status = "abstained";
            result.reason = "ideal_source_margin_below_threshold";
            return result;
        }
        if (combined_index != first_index && combined_index != second_index) {
            result.status = "abstained";
            result.reason = "ideal_source_not_supported_by_head";
            return result;
        }
        const bool disagreement_supported = hopeful_soil
                                                ? std::min(first_margin, second_margin) >= HeadDisagreementMargin
                                                : first_margin >= HeadDisagreementMargin;
        if (!result.heads_agree && !disagreement_supported) {
            result.status = "abstained";
            result.reason = "ideal_source_heads_disagree";
            return result;
        }

        result.status = "recognized";
        result.source = positions[combined_index].position;
        for (std::size_t index = 0; index < positions.size(); ++index) {
            if (manhattan(*result.source, positions[index].position) <= radius) {
                result.domain.emplace_back(positions[index].position);
            }
            if (domain_observation[index] >= DomainObservationThreshold) {
                result.observed_domain.emplace_back(positions[index].position);
            }
        }
        return result;
    }
    catch (const std::exception& exception) {
        result.status = "skipped";
        result.reason = std::string("ideal_domain_recognition_failed: ") + exception.what();
        return result;
    }
}
} // namespace asst::blackflow::perception
