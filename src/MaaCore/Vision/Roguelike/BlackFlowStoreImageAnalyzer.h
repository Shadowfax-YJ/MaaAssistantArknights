#pragma once

#include "Config/Miscellaneous/BlackFlowStoreConfigContract.hpp"
#include "Vision/Roguelike/BlackFlowStorePageAnalyzer.hpp"

#ifdef ASST_BUILD_SMOKE_TEST
#include "AsstPort.h"
#endif

#include <filesystem>
#include <functional>
#include <optional>

namespace cv
{
class Mat;
}

namespace asst
{
struct BlackFlowStoreReadiness
{
    bool store_anchor_visible = false;
    bool refresh_control_visible = false;
    bool overlay_visible = false;
};

using BlackFlowWordOcr = std::function<BlackFlowStoreSlotOcr(const cv::Mat&, const BlackFlowStoreTitleRoi&)>;

class BlackFlowStoreImageAnalyzer final
{
public:
    explicit BlackFlowStoreImageAnalyzer(const BlackFlowStoreConfigContract& config, BlackFlowWordOcr word_ocr = {});

    std::optional<BlackFlowStoreFrameObservation>
        observe(const cv::Mat& normalized_frame, BlackFlowStoreReadiness readiness) const;

    BlackFlowStoreSlotsAnalysis analyze_slots(const cv::Mat& stable_frame) const;

private:
    BlackFlowProductNameMatcher m_matcher;
    BlackFlowWordOcr m_word_ocr;
};

#ifdef ASST_BUILD_SMOKE_TEST
ASSTAPI_PORT bool run_black_flow_store_fixture_smoke_test(const std::filesystem::path& fixture_directory);
#endif
} // namespace asst
