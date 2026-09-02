#include "RoguelikeCustomStartTaskPlugin.h"

#include <algorithm>
#include <random>

#include "Config/GeneralConfig.h"
#include "Config/Miscellaneous/BattleDataConfig.h"
#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "Task/ProcessTask.h"
#include "Utils/Logger.hpp"
#include "Vision/Miscellaneous/PipelineAnalyzer.h"
#include "Vision/OCRer.h"

#include "BlackFlow/BlackFlowStartRewardRules.h"
#include "BlackFlow/BlackFlowAutomationCollectionRules.h"

bool asst::RoguelikeCustomStartTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }

    if (!RoguelikeConfig::is_valid_theme(m_config->get_theme())) {
        Log.error("Roguelike name doesn't exist!");
        return false;
    }

    const std::string roguelike_name = m_config->get_theme() + "@";
    const std::string& task = details.get("details", "task", "");
    std::string_view task_view = task;
    if (task_view.starts_with(roguelike_name)) {
        task_view.remove_prefix(roguelike_name.length());
    }
    static const std::array<std::tuple<AsstMsg, std::string_view, RoguelikeCustomType>, 4> TaskMap = {
        std::make_tuple(AsstMsg::SubTaskCompleted, "Roguelike@Squad-EnterPoint", RoguelikeCustomType::Squad),
        std::make_tuple(AsstMsg::SubTaskStart, "Roguelike@LastReward-EnterPoint", RoguelikeCustomType::Reward),
        std::make_tuple(AsstMsg::SubTaskCompleted, "Roguelike@RolesDefault", RoguelikeCustomType::Roles),
        std::make_tuple(AsstMsg::SubTaskStart, "Roguelike@RecruitMain", RoguelikeCustomType::CoreChar),
    };

    m_waiting_to_run = RoguelikeCustomType::None;
    for (const auto& [t_msg, t_task, t] : TaskMap) {
        if (t_msg == msg && task_view.ends_with(t_task)) {
            m_waiting_to_run = t;
            break;
        }
    }

    const bool automation_collection = m_config->get_theme() == RoguelikeTheme::BlackFlow &&
                                       m_config->get_mode() == RoguelikeMode::BlackFlowAutomationCollection;
    if (m_waiting_to_run == RoguelikeCustomType::None && automation_collection && msg == AsstMsg::SubTaskCompleted &&
        !m_automation_collection_core_char_voucher_selected &&
        task_view.ends_with("StartExplore@Roguelike@CoreCharVoucherReady")) {
        // 只有招募券页面模板重新出现后才切换核心干员所属职业。RecruitWithoutButton 只是
        // 展示页的中央点击兜底；在它完成时触发会对仍在播放的招募动画反复做无效 OCR。
        // 同时必须早于 RecruitOther：后者一旦命中，主流程会缓存并补点原职业券。
        m_waiting_to_run = RoguelikeCustomType::CoreCharVoucher;
    }

    if (m_waiting_to_run == RoguelikeCustomType::None) {
        return false;
    }
    if (m_waiting_to_run == RoguelikeCustomType::Reward) {
        return true;
    }
    if (m_waiting_to_run == RoguelikeCustomType::Squad) {
        if (m_config->get_run_for_collectible()) { // 烧水分队
        }
        else {                                     // 开局分队
        }
        return true;
    }
    if (m_waiting_to_run == RoguelikeCustomType::CoreChar) {
        return !m_config->get_core_char().empty();
    }
    if (m_waiting_to_run == RoguelikeCustomType::CoreCharVoucher) {
        return true;
    }

    // Roles CoreChar
    if (auto it = m_customs.find(m_waiting_to_run); it == m_customs.cend()) {
        return false;
    }
    else if (it->second.empty()) {
        return false;
    }

    return true;
}

bool asst::RoguelikeCustomStartTaskPlugin::load_params(const json::value& params)
{
    const bool automation_collection = m_config->get_theme() == RoguelikeTheme::BlackFlow &&
                                       m_config->get_mode() == RoguelikeMode::BlackFlowAutomationCollection;

    m_squad = automation_collection ? std::string(blackflow::AutomationCollectionSquad) : params.get("squad", "");
    if (m_config->get_mode() == RoguelikeMode::Collectible) {
        m_collectible_mode_squad = params.get("collectible_mode_squad", m_squad);
    }

    m_config->set_core_char(
        automation_collection ? std::string(blackflow::AutomationCollectionCoreOperator) : params.get("core_char", ""));
    set_custom(
        RoguelikeCustomType::Roles,
        automation_collection ? std::string(blackflow::AutomationCollectionRoles) : params.get("roles", ""));
    m_config->set_use_support(params.get("use_support", false));
    m_config->set_use_nonfriend_support(params.get("use_nonfriend_support", false));
    m_automation_collection_core_char_voucher_selected = false;

    if (auto select_list = params.find<json::object>("collectible_mode_start_list"); select_list) {
        RoguelikeStartSelect list;
        list.hot_water = select_list->get("hot_water", false);
        list.shield = select_list->get("shield", false);
        list.ingot = select_list->get("ingot", false);
        list.hope = select_list->get("hope", false);
        list.random = select_list->get("random", false);
        if (m_config->get_theme() == RoguelikeTheme::Mizuki) {
            list.key = select_list->get("key", false);
            list.dice = select_list->get("dice", false);
        }
        else if (m_config->get_theme() == RoguelikeTheme::Sarkaz) {
            list.ideas = select_list->get("ideas", false);
        }
        else if (m_config->get_theme() == RoguelikeTheme::JieGarden) {
            list.ticket = select_list->get("ticket", false);
        }
        m_start_select = list;
    }

    return true;
}

void asst::RoguelikeCustomStartTaskPlugin::set_custom(RoguelikeCustomType type, std::string custom)
{
    m_customs.insert_or_assign(type, std::move(custom));
}

bool asst::RoguelikeCustomStartTaskPlugin::_run()
{
    const std::unordered_map<RoguelikeCustomType, std::function<bool(void)>> TypeActuator = {
        { RoguelikeCustomType::Squad, std::bind(&RoguelikeCustomStartTaskPlugin::hijack_squad, this) },
        { RoguelikeCustomType::Reward, std::bind(&RoguelikeCustomStartTaskPlugin::hijack_reward, this) },
        { RoguelikeCustomType::Roles, std::bind(&RoguelikeCustomStartTaskPlugin::hijack_roles, this) },
        { RoguelikeCustomType::CoreChar, std::bind(&RoguelikeCustomStartTaskPlugin::hijack_core_char, this) },
        { RoguelikeCustomType::CoreCharVoucher,
          std::bind(&RoguelikeCustomStartTaskPlugin::hijack_core_char_voucher, this) },
    };

    auto it = TypeActuator.find(m_waiting_to_run);
    if (it == TypeActuator.cend()) {
        return false;
    }

    return it->second();
}

void asst::RoguelikeCustomStartTaskPlugin::reset_in_run_variables()
{
    m_automation_collection_core_char_voucher_selected = false;
}

bool asst::RoguelikeCustomStartTaskPlugin::hijack_squad()
{
    std::string squad = !m_config->get_run_for_collectible() ? m_squad : m_collectible_mode_squad;
    if (squad.empty()) { // 简单处理，认为指挥分队无需滑屏，没有就随机
        return ProcessTask(
                   *this,
                   { m_config->get_theme() + "@Roguelike@SquadDefault",
                     m_config->get_theme() + "@Roguelike@Squad-Random" })
            .run();
    }

    constexpr size_t SwipeTimes = 7;
    for (size_t i = 0; i != SwipeTimes; ++i) {
        if (need_exit()) {
            return false;
        }
        auto image = ctrler()->get_image();
        OCRer analyzer(image);
        analyzer.set_task_info("RoguelikeCustom-HijackSquad");
        analyzer.set_required({ squad });

        if (!analyzer.analyze()) {
            ProcessTask(*this, { "Roguelike@SquadSlowlySwipeToTheRight" }).run();
            sleep(Task.get("RoguelikeCustom-HijackSquad")->post_delay);
            continue;
        }
        const auto& rect = analyzer.get_result().front().rect;
        ctrler()->click(rect);

        m_config->set_squad(std::move(squad));
        return true;
    }
    ProcessTask(*this, { "SwipeToTheLeft" }).run();
    return false;
}

bool asst::RoguelikeCustomStartTaskPlugin::hijack_reward()
{
    const bool automation_collection = m_config->get_theme() == RoguelikeTheme::BlackFlow &&
                                       m_config->get_mode() == RoguelikeMode::BlackFlowAutomationCollection;
    if (automation_collection) {
        const cv::Mat image = ctrler()->get_image();
        std::vector<blackflow::AutomationCollectionStartReward> rewards(
            blackflow::AutomationCollectionStartRewards.begin(),
            blackflow::AutomationCollectionStartRewards.end());
        static thread_local std::mt19937 random_engine(std::random_device {}());
        std::shuffle(rewards.begin(), rewards.end(), random_engine);
        std::vector<std::string_view> ordinary_priority;
        ordinary_priority.reserve(rewards.size());
        for (const auto& reward : rewards) {
            ordinary_priority.emplace_back(reward.display_name);
        }

        OCRer analyzer(image);
        analyzer.set_task_info("BlackFlow@Roguelike@AutomationCollectionPreferredStartReward");
        // 取回 ROI 内原始 OCR 框，由封闭词表做带唯一性约束的模糊归一；避免 OCRer
        // 先过滤掉“强裸骏鹰”这种两个字误的标题。
        analyzer.set_required({});
        analyzer.set_fuzzy_match(false);
        const auto detected = analyzer.analyze();
        if (!detected.has_value()) {
            Log.error("BlackFlow automation collection could not OCR any start reward title");
            return false;
        }

        std::vector<std::string> titles;
        titles.reserve(detected->size());
        for (const auto& result : *detected) {
            titles.emplace_back(result.text);
        }
        const auto selected = blackflow::select_automation_collection_start_reward(titles, ordinary_priority);
        if (!selected.has_value() || selected->detected_index >= detected->size()) {
            Log.error("BlackFlow automation collection could not fuzzy-match any allowed start reward", titles);
            return false;
        }
        Log.info(
            "BlackFlow automation collection selected start reward",
            selected->canonical,
            "preferred=",
            selected->preferred,
            "captured=",
            (*detected)[selected->detected_index].text);
        ctrler()->click((*detected)[selected->detected_index].rect);

        // 第一次点击只会把卡片切换到“你确定要这么做”的待确认状态。旧逻辑在这里
        // 直接返回，随后依赖一张带卡片背景的 LastRewardConfirm 模板；卡片底图变化时
        // 模板无法命中，流程就会在已勾选的奖励页重试至失败。改为用提示文字确认状态，
        // 再沿提示中轴点击底部确认勾，并验证提示确实消失。
        constexpr int PromptRecognitionAttempts = 5;
        constexpr int PromptRecognitionDelay = 250;
        std::optional<Rect> confirmation_prompt;
        for (int attempt = 0; attempt < PromptRecognitionAttempts; ++attempt) {
            sleep(PromptRecognitionDelay);
            OCRer prompt_analyzer(ctrler()->get_image());
            prompt_analyzer.set_task_info("BlackFlow@Roguelike@AutomationCollectionStartRewardConfirmationPrompt");
            if (const auto prompt = prompt_analyzer.analyze(); prompt.has_value() && !prompt->empty()) {
                confirmation_prompt = prompt->front().rect;
                break;
            }
        }
        if (!confirmation_prompt.has_value()) {
            Log.error(
                "BlackFlow automation collection selected a start reward but did not observe its confirmation prompt",
                selected->canonical);
            return false;
        }

        const Point confirmation_point =
            blackflow::automation_collection_start_reward_confirmation_point(*confirmation_prompt);
        constexpr int ConfirmationClickAttempts = 2;
        constexpr int ConfirmationSettleChecks = 6;
        constexpr int ConfirmationSettleDelay = 250;
        for (int click_attempt = 0; click_attempt < ConfirmationClickAttempts; ++click_attempt) {
            ctrler()->click(confirmation_point);
            for (int settle_check = 0; settle_check < ConfirmationSettleChecks; ++settle_check) {
                sleep(ConfirmationSettleDelay);
                OCRer prompt_analyzer(ctrler()->get_image());
                prompt_analyzer.set_task_info(
                    "BlackFlow@Roguelike@AutomationCollectionStartRewardConfirmationPrompt");
                const auto prompt = prompt_analyzer.analyze();
                if (!prompt.has_value() || prompt->empty()) {
                    if (m_blackflow_start_reward_observer) {
                        m_blackflow_start_reward_observer(selected->canonical);
                    }
                    return true;
                }
            }
        }
        Log.error(
            "BlackFlow automation collection start reward confirmation did not leave the confirmation state",
            selected->canonical,
            confirmation_point);
        return false;
    }

    const auto& list = get_select_list();
    if (list.empty()) {
        // 执行默认选择顺序
        ProcessTask(*this, { m_config->get_theme() + "@Roguelike@LastReward-Strategy" }).run();
        return true;
    }

    // 处理选择顺序
    PipelineAnalyzer analyzer(ctrler()->get_image());
    analyzer.set_tasks(list);
    if (auto ret = analyzer.analyze(); !ret) {
        // 未获取到期望物品，设置烧水flag，重开
        m_config->set_run_for_collectible(true);
        m_control_ptr->exit_then_stop(true);
    }
    else if (m_config->get_start_with_elite_two() || m_config->get_first_floor_foldartal()) {
        // 之后还要凹开局精二或第一层密文板，不停止任务，继续探索
        ctrler()->click(ret->rect);
        sleep(Config.get_options().task_delay);
    }
    else {
        m_control_ptr->exit_then_stop(false);
        m_task_ptr->set_enable(false);
    }

    return true;
}

bool asst::RoguelikeCustomStartTaskPlugin::hijack_roles()
{
    constexpr size_t SwipeTimes = 7;
    const std::string& required_role = m_customs[RoguelikeCustomType::Roles];

    for (size_t i = 0; i != SwipeTimes; ++i) {
        if (need_exit()) {
            return false;
        }

        auto image = ctrler()->get_image();
        OCRer analyzer(image);
        analyzer.set_task_info("RoguelikeCustom-HijackRoles");
        analyzer.set_required({ required_role });

        if (analyzer.analyze()) {
            const auto& rect = analyzer.get_result().front().rect;
            ctrler()->click(rect);
            return true;
        }

        ProcessTask(*this, { "Roguelike@SquadSlowlySwipeToTheRight" }).run();
        sleep(Task.get("RoguelikeCustom-HijackRoles")->post_delay);
    }

    ProcessTask(*this, { "SwipeToTheLeft" }).run();
    return false;
}

bool asst::RoguelikeCustomStartTaskPlugin::hijack_core_char()
{
    const bool automation_collection = m_config->get_theme() == RoguelikeTheme::BlackFlow &&
                                       m_config->get_mode() == RoguelikeMode::BlackFlowAutomationCollection;
    return hijack_recruit_role_for(
        automation_collection ? std::string(blackflow::AutomationCollectionFirstOperator) : m_config->get_core_char());
}

bool asst::RoguelikeCustomStartTaskPlugin::hijack_core_char_voucher()
{
    if (!hijack_recruit_role_for(m_config->get_core_char())) {
        return false;
    }
    m_automation_collection_core_char_voucher_selected = true;
    return true;
}

bool asst::RoguelikeCustomStartTaskPlugin::hijack_recruit_role_for(const std::string& char_name)
{
    static const std::unordered_map<battle::Role, std::string> RoleOcrNameMap = {
        { battle::Role::Caster, "术师" }, { battle::Role::Medic, "医疗" },   { battle::Role::Pioneer, "先锋" },
        { battle::Role::Sniper, "狙击" }, { battle::Role::Special, "特种" }, { battle::Role::Support, "辅助" },
        { battle::Role::Tank, "重装" },   { battle::Role::Warrior, "近卫" }
    };
    const auto& role = BattleData.get_role(char_name);
    auto role_iter = RoleOcrNameMap.find(role);
    if (role_iter == RoleOcrNameMap.cend()) {
        Log.error("Unknown role", char_name, static_cast<int>(role));
        return false;
    }
    return hijack_recruit_role(role_iter->second);
}

bool asst::RoguelikeCustomStartTaskPlugin::hijack_recruit_role(const std::string& role_ocr_name)
{
    Log.info("role", role_ocr_name);
    constexpr int VoucherRecognitionAttempts = 5;
    constexpr int VoucherClickAttempts = 3;
    constexpr int VoucherRecognitionRetryDelay = 300;
    for (int recognition_attempt = 0; recognition_attempt < VoucherRecognitionAttempts; ++recognition_attempt) {
        OCRer analyzer(ctrler()->get_image());
        analyzer.set_task_info("RoguelikeCustom-HijackCoChar");
        analyzer.set_required({ role_ocr_name });
        if (!analyzer.analyze()) {
            if (recognition_attempt + 1 < VoucherRecognitionAttempts) {
                sleep(VoucherRecognitionRetryDelay);
            }
            continue;
        }

        const auto role_rect = analyzer.get_result().front().rect;
        for (int click_attempt = 0; click_attempt < VoucherClickAttempts; ++click_attempt) {
            ctrler()->click(role_rect);
            sleep(Task.get("RoguelikeCustom-HijackCoChar")->pre_delay);

            ProcessTask check(
                *this,
                { m_config->get_theme() + "@Roguelike@ChooseOperFlag",
                  m_config->get_theme() + "@Roguelike@RecruitCloseGuide" });
            check.set_times_limit("Roguelike@ChooseOperFlag", 0);
            check.set_retry_times(0);
            if (check.run()) {
                return true; // 进入选择干员界面
            }
        }
    }
    return false; // 进入选择干员界面失败
}

std::vector<std::string> asst::RoguelikeCustomStartTaskPlugin::get_select_list() const
{
    if (m_config->get_mode() != RoguelikeMode::Collectible ||
        m_config->get_run_for_collectible() /* 正在烧水，使用默认策略 */ ||
        m_config->get_only_start_with_elite_two() /* 只凹精二没有奖励，但第一次开时可能有之前的奖励 */) {
        return {};
    }

    std::vector<std::string> list;
    if (m_start_select.hot_water) {
        list.emplace_back(m_config->get_theme() + "@Roguelike@LastReward"); // 热水壶
    }
    if (m_start_select.shield) {
        list.emplace_back(m_config->get_theme() + "@Roguelike@LastReward2"); // 盾；傀影没盾，是生命
    }
    if (m_start_select.ingot) {
        list.emplace_back(m_config->get_theme() + "@Roguelike@LastReward3"); // 源石锭
    }
    if (m_start_select.hope) {
        list.emplace_back(m_config->get_theme() + "@Roguelike@LastReward4"); // 希望
    }

    if (m_start_select.random) {
        list.emplace_back(m_config->get_theme() + "@Roguelike@LastRewardRand"); // 随机奖励
    }
    if (m_config->get_theme() == RoguelikeTheme::Mizuki) {
        if (m_start_select.key) {
            list.emplace_back("Mizuki@Roguelike@LastReward5"); // 钥匙
        }
        if (m_start_select.dice) {
            list.emplace_back("Mizuki@Roguelike@LastReward6"); // 骰子
        }
    }
    else if (m_config->get_theme() == RoguelikeTheme::Sarkaz && m_start_select.ideas) {
        list.emplace_back("Sarkaz@Roguelike@LastReward5"); // 构想
    }
    else if (m_config->get_theme() == RoguelikeTheme::JieGarden && m_start_select.ticket) {
        list.emplace_back("JieGarden@Roguelike@LastReward5"); // 票券
    }

    return list;
}
