#pragma once

#include <functional>

#include "AbstractRoguelikeTaskPlugin.h"

namespace asst
{
// 肉鸽结算图保存及统计
class RoguelikeSettlementTaskPlugin : public AbstractRoguelikeTaskPlugin
{
public:
    using AbstractRoguelikeTaskPlugin::AbstractRoguelikeTaskPlugin;
    virtual ~RoguelikeSettlementTaskPlugin() override = default;
    virtual bool verify(AsstMsg msg, const json::value& details) const override;
    virtual void reset_in_run_variables() override;
    using SettlementObserver =
        std::function<bool(std::string_view, json::object, std::shared_ptr<cv::Mat>)>;
    void set_blackflow_settlement_observer(SettlementObserver observer) { m_settlement_observer = std::move(observer); }

private:
    virtual bool _run() override;
    bool get_settlement_info(json::value& info, const cv::Mat& image);
    bool wait_for_whole_page();
    enum class BlackFlowGoal
    {
        BeforeFinalPage,
        Statistics,
        Entrance,
        Failed,
    };
    bool prepare_blackflow_settlement(BlackFlowGoal goal, cv::Mat& statistics);
    bool record_blackflow_settlement(std::string_view phase, const cv::Mat& image, json::object details = {});
    bool fail_blackflow_settlement(std::string reason, const cv::Mat& image);
    bool report_blackflow_statistics(const cv::Mat& image);
    void save_img(const cv::Mat& image, const std::filesystem::path& path, std::string name);

    mutable bool m_game_pass = false;
    mutable BlackFlowGoal m_blackflow_goal = BlackFlowGoal::Statistics;
    SettlementObserver m_settlement_observer;
    bool m_statistics_reported = false;
    int m_return_invocations = 0;
};
} // namespace asst
