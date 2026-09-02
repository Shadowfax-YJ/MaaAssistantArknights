#include "BlackFlowTaskPort.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <string_view>
#include <utility>

namespace asst::blackflow
{
EnteredPageObservation classify_entered_page_texts(std::vector<std::string> matched_texts)
{
    std::set<std::string> texts(
        std::make_move_iterator(matched_texts.begin()),
        std::make_move_iterator(matched_texts.end()));
    EnteredPageObservation observation;
    observation.matched_texts.assign(texts.begin(), texts.end());

    const auto contains_prefix = [&texts](std::string_view prefix) {
        return std::ranges::any_of(texts, [prefix](const std::string& text) { return text.starts_with(prefix); });
    };

    const bool final = texts.contains("险路尽头");
    const bool shop = texts.contains("前瞻性投资系统") && texts.contains("刷新");
    const bool scrap_shop = texts.contains("机械师的园圃");
    const bool emergency_aid = texts.size() == 1 && texts.contains("刷新");
    // “右侧干员以查看详情”和“助战招募”属于快捷编队展开后的干员选择界面，
    // 不是战斗身份本身。只有回到快捷编队主界面并命中其专用模板后才能确认战斗。
    observation.combat_operator_selection_open =
        texts.contains("右侧干员以查看详情") || texts.contains("助战招募");
    // 实际弹窗的第一行是“零件箱已满，无法进入下个节点”，不是单独的“零件箱已满”。
    observation.inventory_overloaded = contains_prefix("零件箱已满");
    const int classifications = static_cast<int>(final) + static_cast<int>(shop) + static_cast<int>(scrap_shop) +
                                static_cast<int>(emergency_aid);
    if (classifications > 1) {
        observation.classification_conflict = true;
    }
    else if (final) {
        observation.classified_type = NodeType::Final;
    }
    else if (shop) {
        observation.classified_type = NodeType::Shop;
    }
    else if (scrap_shop) {
        observation.classified_type = NodeType::ScrapShop;
    }
    else if (emergency_aid) {
        observation.classified_type = NodeType::Employ;
    }
    return observation;
}

EnteredPageObservation classify_entered_event_name(std::string event_name)
{
    EnteredPageObservation observation;
    observation.matched_texts.emplace_back(event_name);
    observation.event_name = std::move(event_name);
    const PageContentEffect effect =
        classify_page_content_effect("RoguelikeEvent", *observation.event_name);
    // 未收录的新事件仍按通用事件页执行；收录事件则在页面派发前恢复真实节点语义。
    observation.classified_type = effect.resolved_type.value_or(NodeType::Incident);
    return observation;
}

PageIdentityResolution resolve_page_identity(
    NodeType map_type,
    std::string map_name,
    const MovePreview* preview,
    const EnteredPageObservation& entered_page)
{
    PageIdentityResolution result { map_type, std::move(map_name) };
    const bool map_identity_unresolved =
        map_type == NodeType::Unknown || map_type == NodeType::HideInvisible || map_type == NodeType::HideBattle;
    if (entered_page.classification_conflict) {
        return result;
    }
    if (entered_page.classified_type.has_value()) {
        result.type = *entered_page.classified_type;
        if (entered_page.event_name.has_value()) {
            result.name = *entered_page.event_name;
        }
        else if (preview != nullptr && preview->displayed_type == result.type && !preview->displayed_name.empty()) {
            result.name = preview->displayed_name;
        }
        return result;
    }
    if (preview != nullptr && preview->identity_revealed && preview->displayed_type != NodeType::Unknown) {
        result.type = preview->displayed_type;
        if (!preview->displayed_name.empty()) {
            result.name = preview->displayed_name;
        }
        return result;
    }
    if (map_identity_unresolved && preview != nullptr && preview->displayed_type != NodeType::Unknown) {
        result.type = preview->displayed_type;
        if (!preview->displayed_name.empty()) {
            result.name = preview->displayed_name;
        }
    }
    return result;
}
} // namespace asst::blackflow
