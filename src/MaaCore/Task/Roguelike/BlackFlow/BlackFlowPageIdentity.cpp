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
    const bool shop = texts.contains("坎诺特");
    const bool scrap_shop = texts.contains("机械师");
    const bool emergency_aid = texts.contains("佩德洛");
    std::vector<std::string> event_titles;
    std::ranges::copy_if(texts, std::back_inserter(event_titles), [](const std::string& text) {
        const PageContentEffect effect = classify_page_content_effect("RoguelikeEvent", text);
        return effect.resolved_type.has_value() || effect.changes_floor || !effect.has_landing;
    });
    // “右侧干员以查看详情”和“助战招募”属于快捷编队展开后的干员选择界面，
    // 不是战斗身份本身。只有回到快捷编队主界面并命中其专用模板后才能确认战斗。
    observation.combat_operator_selection_open =
        texts.contains("右侧干员以查看详情") || texts.contains("助战招募");
    // 实际弹窗的第一行是“零件箱已满，无法进入下个节点”，不是单独的“零件箱已满”。
    observation.inventory_overloaded = contains_prefix("零件箱已满");
    // 本分支额外识别快捷编队干员列表和零件箱满载弹窗。它们都是覆盖在页面之上的
    // 中间态，优先级高于上游人物名分类，不能把列表里的“机械师”等文字当成落点身份。
    if (observation.combat_operator_selection_open || observation.inventory_overloaded) {
        return observation;
    }
    const int classifications = static_cast<int>(final) + static_cast<int>(shop) + static_cast<int>(scrap_shop) +
                                static_cast<int>(emergency_aid);
    if (classifications > 1) {
        observation.classification_conflict = true;
    }
    else if (final) {
        if (event_titles.size() == 1) {
            observation.classified_type = NodeType::Final;
        }
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
    else {
        // 保留本分支专门增加的首帧事件快速分类：固定页面标志优先；若它们均未命中，
        // 已知事件标题可直接恢复页面身份，不必经历后续等待和中央推进点击。
        // 窄 ROI 已排除招募干员列表，因此不会再把“黑键”模糊匹配成“黑诞”。
        if (event_titles.size() == 1) {
            EnteredPageObservation event = classify_entered_event_name(event_titles.front());
            event.matched_texts = std::move(observation.matched_texts);
            return event;
        }
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
