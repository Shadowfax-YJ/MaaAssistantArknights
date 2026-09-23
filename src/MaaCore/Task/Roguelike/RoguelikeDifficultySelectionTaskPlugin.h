#pragma once
#include <functional>

#include "AbstractRoguelikeTaskPlugin.h"

namespace asst
{
class RoguelikeDifficultySelectionTaskPlugin : public AbstractRoguelikeTaskPlugin
{
public:
    using AbstractRoguelikeTaskPlugin::AbstractRoguelikeTaskPlugin;
    virtual ~RoguelikeDifficultySelectionTaskPlugin() override = default;
    virtual bool verify(AsstMsg msg, const json::value& details) const override;
    virtual bool load_params(const json::value& params) override;

    void set_difficulty_observer(std::function<bool(int, int, bool, const cv::Mat&)> observer)
    {
        m_difficulty_observer = std::move(observer);
    }

protected:
    virtual bool _run() override;
    int detect_blackflow_home_difficulty(const cv::Mat& image) const;

private:
    int detect_current_difficulty() const;
    bool select_difficulty(const int difficulty = 0);
    bool verify_blackflow_difficulty(int target, int& observed, cv::Mat& image);

    std::function<bool(int, int, bool, const cv::Mat&)> m_difficulty_observer;

    int m_current_difficulty = -1;
    mutable bool m_has_changed = false;

    mutable int m_collectible_difficulty = -1;
};
}
