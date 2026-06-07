#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "MaaUtils/NoWarningCVMat.hpp"
#include "RoguelikeBoskyPassageMap.h"
#include "Task/Roguelike/AbstractRoguelikeTaskPlugin.h"

namespace asst
{
class RoguelikeBoskyPassageRoutingTaskPlugin : public AbstractRoguelikeTaskPlugin
{
public:
    using AbstractRoguelikeTaskPlugin::AbstractRoguelikeTaskPlugin;
    virtual ~RoguelikeBoskyPassageRoutingTaskPlugin() override = default;
    virtual bool verify(AsstMsg msg, const json::value& details) const override;
    virtual bool load_params(const json::value& params) override;
    virtual void reset_in_run_variables() override;

    enum class RoutingStrategy
    {
        None,
        BoskyPassage_JieGarden,
        FindPlaytime_JieGarden,
    };

protected:
    virtual bool _run() override;

private:
    bool bosky_update_map(); // 从当前截图识别所有可见节点并更新/创建
    void record_bosky_map_snapshot(std::string_view entry_map_image, std::string_view entry_map_overlay) const;
    void bosky_update_edges(
        const cv::Mat& image,
        const RoguelikeBoskyPassageMap::BoskyPassageMapConfig& view_config,
        std::optional<std::reference_wrapper<cv::Mat>> image_draw_opt = std::nullopt);
    void bosky_decide_and_click(const std::vector<RoguelikeNodeType>& priority_order);      // 策略，可指定优先级策略
    std::vector<RoguelikeNodeType> get_bosky_passage_priority(const std::string& strategy); // 从配置文件读取优先级

    inline static std::function<std::string(RoguelikeNodeType)> type2name = &RoguelikeMapConfig::type2name;
    inline static std::function<RoguelikeNodeType(const std::string&)> name2type = &RoguelikeMapConfig::name2type;
    // ———————— constants and variables ———————————————————————————————————————————————
    RoutingStrategy m_bosky_routing_strategy = RoutingStrategy::None;
    RoguelikeBoskyPassageMap::BoskyPassageMapConfig m_bosky_config;
    RoguelikeBoskyPassageMap::BoskyPassageMapConfig m_bosky_view_config;
    bool m_saved_entry_map = false;
    std::string m_entry_map_image;
    std::string m_entry_map_overlay;

    // 界园树洞地图使用单例模式，通过 RoguelikeBoskyPassageMap::get_instance() 访问
};
}
