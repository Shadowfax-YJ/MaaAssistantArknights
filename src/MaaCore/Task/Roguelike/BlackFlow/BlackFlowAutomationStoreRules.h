#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "BlackFlowModel.h"

namespace asst::blackflow
{
enum class AutomationStoreKind : std::uint8_t
{
    Eerie,
    Secret,
};

struct EerieStoreStitchAnchor
{
    std::string_view name;
    int x = 0;
    int y = 0;
};

struct EerieStoreStitchLayout
{
    int output_width = 0;
    int output_height = 0;
    Rect base_source;
    Rect base_destination;
    Rect continuation_source;
    Rect continuation_destination;
};

// 商店的第二排会同时出现在回顶截图和滑到底截图中。用同名、同列商品的
// 纵坐标差标定实际滚动距离，避免把分辨率、模拟器手势误差写成固定像素。
[[nodiscard]] inline std::optional<int> eerie_store_scroll_offset(
    const std::vector<EerieStoreStitchAnchor>& top_goods,
    const std::vector<EerieStoreStitchAnchor>& bottom_goods)
{
    constexpr int SameColumnTolerance = 80;
    constexpr int MinimumScrollOffset = 80;
    constexpr int MaximumScrollOffset = 480;
    constexpr int OffsetClusterTolerance = 8;

    std::vector<int> candidates;
    for (const EerieStoreStitchAnchor& top : top_goods) {
        if (top.name.empty()) {
            continue;
        }
        for (const EerieStoreStitchAnchor& bottom : bottom_goods) {
            const int column_delta = top.x >= bottom.x ? top.x - bottom.x : bottom.x - top.x;
            const int offset = top.y - bottom.y;
            if (top.name == bottom.name && column_delta <= SameColumnTolerance &&
                offset >= MinimumScrollOffset && offset <= MaximumScrollOffset) {
                candidates.emplace_back(offset);
            }
        }
    }
    // OCR 偶尔会漏掉恰好重叠的同一件商品；两张图各自可见的相邻两排
    // 间距仍等于这次滚动距离，可作为不依赖商品名的次级证据。
    const auto append_visible_row_pitch = [&candidates](const std::vector<EerieStoreStitchAnchor>& goods) {
        constexpr int SameRowTolerance = 30;
        std::vector<int> ys;
        ys.reserve(goods.size());
        for (const EerieStoreStitchAnchor& good : goods) {
            ys.emplace_back(good.y);
        }
        std::ranges::sort(ys);

        std::vector<std::vector<int>> rows;
        for (const int y : ys) {
            if (rows.empty() || y - rows.back().back() > SameRowTolerance) {
                rows.emplace_back();
            }
            rows.back().emplace_back(y);
        }
        std::vector<int> centers;
        centers.reserve(rows.size());
        for (const auto& row : rows) {
            centers.emplace_back(row[(row.size() - 1) / 2]);
        }
        for (std::size_t index = 1; index < centers.size(); ++index) {
            const int pitch = centers[index] - centers[index - 1];
            if (pitch >= MinimumScrollOffset && pitch <= MaximumScrollOffset) {
                candidates.emplace_back(pitch);
            }
        }
    };
    if (candidates.empty()) {
        append_visible_row_pitch(top_goods);
        append_visible_row_pitch(bottom_goods);
        if (candidates.empty()) {
            return std::nullopt;
        }
    }

    std::ranges::sort(candidates);
    int best_center = candidates.front();
    std::size_t best_size = 0;
    for (const int center : candidates) {
        const auto first = std::ranges::lower_bound(candidates, center - OffsetClusterTolerance);
        const auto last = std::ranges::upper_bound(candidates, center + OffsetClusterTolerance);
        const std::size_t size = static_cast<std::size_t>(last - first);
        if (size > best_size) {
            best_size = size;
            best_center = center;
        }
    }

    std::vector<int> cluster;
    std::ranges::copy_if(candidates, std::back_inserter(cluster), [best_center](int offset) {
        const int delta = offset >= best_center ? offset - best_center : best_center - offset;
        return delta <= OffsetClusterTolerance;
    });
    return cluster[(cluster.size() - 1) / 2];
}

// 与事件长截图相同：完整保留首屏，只把末屏从可滚动区域的顶部覆盖到
// 对应纵坐标并向下扩展。这样页头、最后一排商品和页脚都不会被裁掉。
[[nodiscard]] inline std::optional<EerieStoreStitchLayout> eerie_store_stitch_layout(
    int image_width,
    int image_height,
    const Rect& scroll_roi,
    int scroll_offset) noexcept
{
    if (image_width <= 0 || image_height <= 0 || scroll_roi.x < 0 || scroll_roi.y < 0 ||
        scroll_roi.x >= image_width || scroll_roi.y >= image_height || scroll_offset <= 0 ||
        scroll_offset >= image_height) {
        return std::nullopt;
    }

    const int continuation_width = image_width - scroll_roi.x;
    const int continuation_height = image_height - scroll_roi.y;
    return EerieStoreStitchLayout {
        .output_width = image_width,
        .output_height = image_height + scroll_offset,
        .base_source = Rect { 0, 0, image_width, image_height },
        .base_destination = Rect { 0, 0, image_width, image_height },
        .continuation_source = Rect { scroll_roi.x, scroll_roi.y, continuation_width, continuation_height },
        .continuation_destination =
            Rect { scroll_roi.x, scroll_roi.y + scroll_offset, continuation_width, continuation_height },
    };
}

[[nodiscard]] inline constexpr bool
    automation_store_should_capture_cultivation_result(std::string_view task) noexcept
{
    return task == "BlackFlow@Roguelike@AutomationCultivateHarvestReady";
}

[[nodiscard]] inline constexpr bool automation_store_purchase_succeeded(
    std::optional<int> ingots_before,
    std::optional<int> ingots_after) noexcept
{
    return ingots_before.has_value() && ingots_after.has_value() && *ingots_after < *ingots_before;
}

struct AutomationStorePurchaseVerification
{
    std::string_view status;
    std::string_view reason;

    [[nodiscard]] bool succeeded() const noexcept { return status == "confirmed"; }
};

[[nodiscard]] inline AutomationStorePurchaseVerification verify_store_purchase_evidence(
    bool wallet_page_verified,
    std::optional<int> before,
    std::optional<int> after,
    std::optional<int> price,
    bool item_consumption_verified) noexcept
{
    if (!wallet_page_verified) {
        return { "unknown", "wallet_page_unverified" };
    }
    if (!before.has_value() || !after.has_value() || !price.has_value()) {
        return { "unknown", "price_or_wallet_unavailable" };
    }
    if (*before == *after) {
        return { "failed", "wallet_unchanged" };
    }
    if (*before - *after != *price || *price <= 0) {
        return { "mismatch", "payment_differs_from_observed_price" };
    }
    if (!item_consumption_verified) {
        return { "unknown", "actual_item_unverified" };
    }
    return { "confirmed", "sold_out_and_exact_payment" };
}

struct AutomationStoreIdentity
{
    std::uint64_t map_section_generation = 0;
    NodeId node = InvalidNodeId;
    AutomationStoreKind kind = AutomationStoreKind::Eerie;

    bool operator==(const AutomationStoreIdentity&) const noexcept = default;
};

struct AutomationStoreIdentityHash
{
    [[nodiscard]] std::size_t operator()(const AutomationStoreIdentity& identity) const noexcept
    {
        std::size_t result = std::hash<std::uint64_t> {}(identity.map_section_generation);
        result ^= std::hash<NodeId> {}(identity.node) + 0x9e3779b9U + (result << 6) + (result >> 2);
        result ^= std::hash<unsigned> {}(static_cast<unsigned>(identity.kind)) + 0x9e3779b9U + (result << 6) +
                  (result >> 2);
        return result;
    }
};

inline constexpr int AutomationStoreRefreshPriceStep = 4;
// 自动采集在第 2 轮后停止付费刷新；N6 商店实际共有 4 次刷新机会，
// 无缘果同样消耗这些机会，不能拿策略预算当作游戏的耗尽条件。
inline constexpr int AutomationStoreMaxRefreshTimes = 2;
inline constexpr int N6StoreRefreshLimit = 4;

[[nodiscard]] inline constexpr bool automation_store_refresh_exhausted(int completed_refreshes) noexcept
{
    return completed_refreshes >= N6StoreRefreshLimit;
}

[[nodiscard]] inline constexpr int automation_store_refresh_price(int completed_paid_refreshes) noexcept
{
    return AutomationStoreRefreshPriceStep * (completed_paid_refreshes + 1);
}

[[nodiscard]] inline constexpr bool
    automation_store_can_refresh(int completed_refreshes, int completed_paid_refreshes, int wallet, int reserve) noexcept
{
    return completed_refreshes >= 0 && completed_refreshes < AutomationStoreMaxRefreshTimes &&
           completed_paid_refreshes >= 0 && completed_paid_refreshes <= completed_refreshes &&
           wallet >= automation_store_refresh_price(completed_paid_refreshes) + reserve;
}

// 没有出售触发刷新的商店，总轮次与付费次数相同。
[[nodiscard]] inline constexpr bool
    automation_store_can_refresh(int completed_refreshes, int wallet, int reserve) noexcept
{
    return automation_store_can_refresh(completed_refreshes, completed_refreshes, wallet, reserve);
}

enum class AutomationStoreRefreshSource
{
    Paid,
    ItemSale,
};

class AutomationStoreRefreshLedger
{
public:
    [[nodiscard]] int refresh_count(const AutomationStoreIdentity& identity) const noexcept
    {
        const auto iter = m_counts.find(identity);
        return iter == m_counts.end() ? 0 : iter->second.total;
    }

    [[nodiscard]] int paid_refresh_count(const AutomationStoreIdentity& identity) const noexcept
    {
        const auto iter = m_counts.find(identity);
        return iter == m_counts.end() ? 0 : iter->second.paid;
    }

    int record_refresh(
        const AutomationStoreIdentity& identity,
        AutomationStoreRefreshSource source = AutomationStoreRefreshSource::Paid)
    {
        auto& count = m_counts[identity];
        if (!automation_store_refresh_exhausted(count.total)) {
            ++count.total;
            count.paid += source == AutomationStoreRefreshSource::Paid;
        }
        return count.total;
    }

    void clear() noexcept { m_counts.clear(); }

private:
    struct Counts
    {
        int total = 0;
        int paid = 0;
    };
    std::unordered_map<AutomationStoreIdentity, Counts, AutomationStoreIdentityHash> m_counts;
};

[[nodiscard]] inline constexpr bool merchant_sale_may_refresh(AutomationStoreKind kind, std::string_view name) noexcept
{
    return kind == AutomationStoreKind::Eerie && name == "无缘果";
}

[[nodiscard]] inline constexpr std::string_view automation_shop_resume_base_task() noexcept
{
    return "BlackFlow@Roguelike@AutomationShopDecision-Enter";
}

[[nodiscard]] inline constexpr std::string_view automation_shop_resume_fallback_base_task() noexcept
{
    return "BlackFlow@Roguelike@AutomationShopResumeGeneric";
}

[[nodiscard]] inline Rect merchant_buyability_color_roi(const Rect& name_rect) noexcept
{
    return name_rect.move({ -20, 130, 200, 80 });
}

[[nodiscard]] inline constexpr std::optional<bool>
    merchant_buyability_color_evidence(std::size_t red_pixels, std::size_t green_pixels, std::size_t total_pixels) noexcept
{
    if (total_pixels == 0) {
        return std::nullopt;
    }

    // The lower price band is visibly tinted across a large part of the card:
    // green/teal when affordable and red when unaffordable. Requiring at least
    // 20% of the ROI plus 3:2 dominance keeps item glows and white price text
    // from turning an otherwise neutral/animating card into hard evidence.
    const auto is_dominant = [total_pixels](std::size_t candidate, std::size_t other) {
        return candidate * 5 >= total_pixels && candidate * 2 > other * 3;
    };
    if (is_dominant(red_pixels, green_pixels)) {
        return false;
    }
    if (is_dominant(green_pixels, red_pixels)) {
        return true;
    }
    return std::nullopt;
}

inline constexpr std::array<std::string_view, 8> EerieStoreCollectibleBuyPriority {
    "猎犬咖啡",
    "悲伤的红",
    "《炎国字汇》",
    "三尺万象",
    "医者-自医",
    "铁卫-侵掠",
    "迷藏",
    "“小格兰法洛”",
};

inline constexpr std::size_t EerieStoreCollectibleRunPurchaseLimit = 2;

[[nodiscard]] inline constexpr bool is_eerie_store_collectible(std::string_view name) noexcept
{
    return std::ranges::find(EerieStoreCollectibleBuyPriority, name) != EerieStoreCollectibleBuyPriority.end();
}

[[nodiscard]] inline constexpr bool eerie_store_collectible_purchase_allowed(
    std::string_view name,
    std::size_t purchased_in_run) noexcept
{
    return !is_eerie_store_collectible(name) || purchased_in_run < EerieStoreCollectibleRunPurchaseLimit;
}

[[nodiscard]] inline constexpr bool eerie_store_purchase_opens_recruitment(std::string_view name) noexcept
{
    return name.ends_with("招募券");
}

inline constexpr std::array<std::string_view, 34> ShopBuyPriority {
    "沙盘β",
    "医者-新典训",
    "医疗招募券",
    "重装招募券",
    "精锐重装招募券",
    "堡垒协议招募券",
    "特种招募券",
    "一次性喷气背包",
    "重弹簧",
    "种子",
    "老妈妈的融雪",
    "坎诺特的触须",
    "试作外骨骼",
    "小八界",
    "气垫底座",
    "标准引擎",
    "报废轮子",
    "报废假肢",
    "多生苔藓",
    "雾滚草",
    "白模鱼",
    "猎犬咖啡",
    "悲伤的红",
    "《炎国字汇》",
    "三尺万象",
    "医者-自医",
    "铁卫-侵掠",
    "迷藏",
    "“小格兰法洛”",
    "生逝",
    "血橙",
    "无缘果",
    "力反馈方向盘",
    "洛伦茨之眼",
};

inline constexpr std::array<std::string_view, 19> ScrapShopBuyPriority {
    "一次性喷气背包",
    "重弹簧",
    "种子",
    "老妈妈的融雪",
    "坎诺特的触须",
    "试作外骨骼",
    "小八界",
    "气垫底座",
    "标准引擎",
    "报废轮子",
    "报废假肢",
    "多生苔藓",
    "雾滚草",
    "白模鱼",
    "生逝",
    "血橙",
    "无缘果",
    "力反馈方向盘",
    "洛伦茨之眼",
};

// 只列有限配额；未列出的可购买物品不限量。加工品还扣除进节点时的可用实例，
// 自然物/概念体沿用本节点成功购买次数，不推算完整持有清单。
struct StorePurchaseQuota
{
    std::string_view name;
    std::size_t maximum;
};

inline constexpr std::array<StorePurchaseQuota, 15> StorePurchaseQuotas { {
    { "重弹簧", 2 },
    { "种子", 2 },
    { "老妈妈的融雪", 2 },
    { "坎诺特的触须", 1 },
    { "试作外骨骼", 2 },
    { "小八界", 1 },
    { "气垫底座", 2 },
    { "标准引擎", 2 },
    { "报废轮子", 2 },
    { "报废假肢", 2 },
    { "多生苔藓", 2 },
    { "雾滚草", 2 },
    { "白模鱼", 2 },
    { "生逝", 2 },
    { "血橙", 2 },
} };

[[nodiscard]] inline constexpr std::optional<std::size_t>
    automation_store_purchase_quota(std::string_view name) noexcept
{
    const auto iter = std::ranges::find(StorePurchaseQuotas, name, &StorePurchaseQuota::name);
    return iter == StorePurchaseQuotas.end() ? std::nullopt : std::optional<std::size_t> { iter->maximum };
}

[[nodiscard]] inline constexpr bool
    automation_store_purchase_quota_allowed(
        std::string_view name,
        std::size_t usable_parts_on_entry,
        std::size_t successful_purchases) noexcept
{
    const std::optional<std::size_t> quota = automation_store_purchase_quota(name);
    return !quota.has_value() ||
           (usable_parts_on_entry < *quota && successful_purchases < *quota - usable_parts_on_entry);
}

[[nodiscard]] inline std::size_t usable_processing_item_count(
    std::string_view name,
    const std::vector<RunResources::MovementInstance>& instances) noexcept
{
    const auto movement = std::ranges::find_if(movement_specs(), [&](const MovementSpec& spec) {
        return spec.kind != MovementKind::Walk && spec.name == name;
    });
    if (movement == movement_specs().end()) {
        return 0;
    }
    return static_cast<std::size_t>(std::ranges::count_if(instances, [&](const auto& instance) {
        return instance.movement == movement->kind && instance.remaining_charges > 0;
    }));
}

// 识别完整零件词表，再按各店的出售名单筛选。未出售的相似名称也必须参与
// OCR 纠错，避免把白模狗等名称纠成名单中允许出售的白模鸟。
[[nodiscard]] inline std::vector<std::string> merchant_sale_recognition_vocabulary(
    const std::vector<std::string>& sale_candidates,
    const std::vector<std::string>& inventory_names)
{
    std::vector<std::string> vocabulary = sale_candidates;
    for (const auto& name : inventory_names) {
        if (std::ranges::find(vocabulary, name) == vocabulary.end()) {
            vocabulary.emplace_back(name);
        }
    }
    return vocabulary;
}

// 只有秘境行商的轮子和假肢仍要求持有板藤；诡意行商及其余零件均不受此条件限制。
inline constexpr std::array<std::string_view, 2> ScrapShopBoardVineConditionalPurchases {
    "报废轮子",
    "报废假肢",
};

[[nodiscard]] inline constexpr bool board_vine_purchase_allowed(
    AutomationStoreKind kind,
    std::string_view name,
    bool has_board_vine) noexcept
{
    return kind != AutomationStoreKind::Secret || has_board_vine ||
           std::ranges::find(ScrapShopBoardVineConditionalPurchases, name) ==
               ScrapShopBoardVineConditionalPurchases.end();
}

[[nodiscard]] inline constexpr bool scrap_shop_purchase_requests_cultivation(std::string_view name) noexcept
{
    return name == "种子";
}

[[nodiscard]] inline constexpr bool scrap_shop_should_start_cultivation(
    bool cultivation_requested,
    int held_seeds,
    bool start_button_visible) noexcept
{
    return cultivation_requested && held_seeds > 0 && start_button_visible;
}

struct NaturalSaleFloor
{
    std::string_view name;
    int minimum_price;
};

inline constexpr std::array<NaturalSaleFloor, 8> NaturalSaleFloors { {
    { "多生苔藓", 12 },
    { "雾滚草", 8 },
    { "浪花", 8 },
    { "板藤", 50 },
    { "生逝", 16 },
    { "血橙", 8 },
    { "旅行绒", 24 },
    { "无缘果", 14 },
} };

inline constexpr int PriceTextMaximumSaturation = 96;
inline constexpr int PriceTextMinimumValue = 160;

[[nodiscard]] inline constexpr bool price_text_hsv_allowed(int hue, int saturation, int value) noexcept
{
    return hue >= 0 && hue <= 179 && saturation >= 0 && saturation <= PriceTextMaximumSaturation &&
           value >= PriceTextMinimumValue && value <= 255;
}

// 商品名称位于卡片左上，售价位于同一卡片右下；横向避开青绿色货币图标。
[[nodiscard]] inline Rect merchant_price_roi(const Rect& name_rect) noexcept
{
    return name_rect.move({ 100, 150, 100, 45 });
}

[[nodiscard]] inline std::optional<int> minimum_natural_sale_price(std::string_view name) noexcept
{
    const auto iter = std::ranges::find(NaturalSaleFloors, name, &NaturalSaleFloor::name);
    return iter == NaturalSaleFloors.end() ? std::nullopt : std::optional<int> { iter->minimum_price };
}

// 受最低价保护的自然物只有在报价识别成功且不低于阈值时才出售；
// 其他自然物和概念体仍按原有规则出售。
[[nodiscard]] inline bool natural_sale_price_allowed(std::string_view name, std::optional<int> price) noexcept
{
    const std::optional<int> minimum = minimum_natural_sale_price(name);
    return !minimum.has_value() || (price.has_value() && *price >= *minimum);
}

// 售价位于商品卡片的最右侧；货币图标、卡片花纹偶尔会在同一 OCR 区域里产生
// 诸如 114 的伪数字。只接受完整数字，并以最靠右的结果为准。
template <std::ranges::input_range Candidates>
[[nodiscard]] inline std::optional<int> rightmost_numeric_price(const Candidates& candidates) noexcept
{
    std::optional<std::pair<int, int>> rightmost;
    for (const auto& candidate : candidates) {
        const std::string_view text = candidate.second;
        int price = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), price);
        if (error != std::errc {} || end != text.data() + text.size() || price < 0) {
            // 售价颜色过滤后的现场 OCR 会把十位的“1”稳定读成竖线（如 |7、|9）。
            // 只修复“一个竖线字形 + 一个数字”这一种窄模式，避免把卡片花纹拼成价格。
            if (text.size() != 2 || (text.front() != '|' && text.front() != 'I' && text.front() != 'l') ||
                text.back() < '0' || text.back() > '9') {
                continue;
            }
            price = 10 + (text.back() - '0');
        }
        if (!rightmost.has_value() || candidate.first > rightmost->first) {
            rightmost = std::pair { candidate.first, price };
        }
    }
    return rightmost.has_value() ? std::optional<int> { rightmost->second } : std::nullopt;
}

// A click can miss while the shelf is still settling after a scroll. One miss
// must not blacklist every product with the same normalized name for the rest
// of this merchant visit; allow one fresh-coordinate retry, then stop to avoid
// an infinite click loop on a genuinely unavailable card.
[[nodiscard]] inline bool purchase_name_attempt_allowed(
    std::string_view name,
    const std::vector<std::string>& attempted_names,
    std::size_t max_attempts = 2) noexcept
{
    return static_cast<std::size_t>(std::ranges::count(attempted_names, name)) < max_attempts;
}

struct StoreGoodOffer
{
    std::string_view name;
    std::optional<int> price;
    bool eligible = true;
    // 商品卡片自身的亮起/灰暗状态比价格 OCR 可靠；有现场状态时以它为准。
    std::optional<bool> shelf_buyable;
};

// RoguelikeTraderShopping 只提供“购物标记命中”的正证据。未命中也可能是 ROI、动画或模板波动，
// 不能据此断言商品不可购买；这时回退到价格和钱包判断，并让实际点击确认承担最终校验。
[[nodiscard]] inline std::optional<bool> positive_shelf_buyability_evidence(bool shopping_marker_matched) noexcept
{
    return shopping_marker_matched ? std::optional<bool> { true } : std::nullopt;
}

// 商品卡片已明确亮起时直接视为买得起，不能让异常价格 OCR（例如把 8 读成 1148）否决它。
// 没有卡片状态证据时才回退到价格与钱包比较。
template <std::ranges::input_range Priorities>
[[nodiscard]] inline std::optional<std::size_t> select_preferred_affordable_good(
    const Priorities& priorities,
    const std::vector<StoreGoodOffer>& goods,
    std::optional<int> wallet)
{
    std::optional<std::size_t> first_unknown;
    for (const std::string_view preferred : priorities) {
        for (std::size_t index = 0; index < goods.size(); ++index) {
            const StoreGoodOffer& good = goods[index];
            if (!good.eligible || good.name != preferred) {
                continue;
            }
            if (good.shelf_buyable.has_value()) {
                if (*good.shelf_buyable) {
                    return index;
                }
                continue;
            }
            if (good.price.has_value() && wallet.has_value()) {
                if (*good.price <= *wallet) {
                    return index;
                }
            }
            else if (!first_unknown.has_value()) {
                first_unknown = index;
            }
        }
    }
    return first_unknown;
}

// AutomationShopLeave-Enter is normally inherited as a base task, so ProcessTask
// reports StageTraderLeaveConfirmCompleted after the shop has actually closed.
// Both names are accepted to keep the temporary "return to shop after recruit"
// task binding scoped to one shop visit.
[[nodiscard]] inline bool automation_shop_resume_binding_expires_on(std::string_view task) noexcept
{
    return task == "AutomationShopLeave-Enter" || task.ends_with("@AutomationShopLeave-Enter") ||
           task == "StageTraderLeaveConfirmCompleted" || task.ends_with("@StageTraderLeaveConfirmCompleted");
}
} // namespace asst::blackflow
