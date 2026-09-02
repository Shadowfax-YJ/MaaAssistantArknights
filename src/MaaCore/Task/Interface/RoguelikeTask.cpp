#include "RoguelikeTask.h"

#include <utility>

#include "Common/AsstBattleDef.h"
#include "Config/TaskData.h"
#include "Task/ProcessTask.h"

// ------------------ 通用配置及插件 ------------------
#include "Task/Miscellaneous/ScreenshotTaskPlugin.h"
#include "Task/Roguelike/RoguelikeBattleTaskPlugin.h"
#include "Task/Roguelike/RoguelikeConfig.h"
#include "Task/Roguelike/RoguelikeControlTaskPlugin.h"
#include "Task/Roguelike/RoguelikeCustomStartTaskPlugin.h"
#include "Task/Roguelike/RoguelikeDebugTaskPlugin.h"
#include "Task/Roguelike/RoguelikeDifficultySelectionTaskPlugin.h"
#include "Task/Roguelike/RoguelikeFormationTaskPlugin.h"
#include "Task/Roguelike/RoguelikeInvestTaskPlugin.h"
#include "Task/Roguelike/RoguelikeIterateDeepExplorationPlugin.h"
#include "Task/Roguelike/RoguelikeIterateMonthlySquadPlugin.h"
#include "Task/Roguelike/RoguelikeLastRewardTaskPlugin.h"
#include "Task/Roguelike/RoguelikeLevelTaskPlugin.h"
#include "Task/Roguelike/RoguelikeRecruitTaskPlugin.h"
#include "Task/Roguelike/RoguelikeResetTaskPlugin.h"
#include "Task/Roguelike/RoguelikeSettlementTaskPlugin.h"
#include "Task/Roguelike/RoguelikeShoppingTaskPlugin.h"
#include "Task/Roguelike/RoguelikeSkillSelectionTaskPlugin.h"
#include "Task/Roguelike/RoguelikeStageEncounterTaskPlugin.h"
#include "Task/Roguelike/RoguelikeStrategyChangeTaskPlugin.h"

// ------------------ 萨米主题专用配置及插件 ------------------
#include "Config/Roguelike/Sami/RoguelikeCollapsalParadigmConfig.h"
#include "Task/Roguelike/Sami/RoguelikeCollapsalParadigmTaskPlugin.h"
#include "Task/Roguelike/Sami/RoguelikeFoldartalGainTaskPlugin.h"
#include "Task/Roguelike/Sami/RoguelikeFoldartalStartTaskPlugin.h"
#include "Task/Roguelike/Sami/RoguelikeFoldartalUseTaskPlugin.h"

// ------------------ 萨卡兹主题专用配置及插件 ------------------
#include "Task/Roguelike/RoguelikeInputSeedTaskPlugin.h"

// ------------------ 导航相关配置及插件 ------------------
#include "Task/Roguelike/Map/RoguelikeBoskyPassageRoutingTaskPlugin.h"
#include "Task/Roguelike/Map/RoguelikeRoutingTaskPlugin.h"

// ------------------ 界园主题专用配置及插件 ------------------
#include "Task/Roguelike/JieGarden/RoguelikeCoppersTaskPlugin.h"

// ------------------ 黑流树海主题专用配置及插件 ------------------
#include "Task/Roguelike/BlackFlow/BlackFlowAutomationStoreTaskPlugin.h"
#include "Task/Roguelike/BlackFlow/BlackFlowAutomationCollectionRules.h"
#include "Task/Roguelike/BlackFlow/BlackFlowCollectionPopupTaskPlugin.h"
#include "Task/Roguelike/BlackFlow/BlackFlowCultivationTaskPlugin.h"
#include "Task/Roguelike/BlackFlow/BlackFlowLifecycleTaskPlugin.h"
#include "Task/Roguelike/BlackFlow/BlackFlowMapObservationSource.h"
#include "Task/Roguelike/BlackFlow/BlackFlowMovementTaskPlugin.h"
#include "Task/Roguelike/BlackFlow/BlackFlowNodeEvidenceTaskPlugin.h"
#include "Task/Roguelike/BlackFlow/BlackFlowNodeTaskPlugin.h"
#include "Task/Roguelike/BlackFlow/BlackFlowRoutingTaskPlugin.h"
#include "Task/Roguelike/BlackFlow/BlackFlowRunLogTaskPlugin.h"
#include "Task/Roguelike/BlackFlow/BlackFlowTaskPort.h"
#include "Task/Roguelike/BlackFlow/BlackFlowTrophyRewardTaskPlugin.h"

#include "Utils/Logger.hpp"

asst::RoguelikeTask::RoguelikeTask(const AsstCallback& callback, Assistant* inst) :
    InterfaceTask(callback, inst, TaskType),
    m_roguelike_task_ptr(std::make_shared<ProcessTask>(callback, inst, TaskType)),
    m_config_ptr(std::make_shared<RoguelikeConfig>())
{
    LogTraceFunction;

    // m_roguelike_task_ptr->set_ignore_error(true);
    m_control_ptr = m_roguelike_task_ptr->register_plugin<RoguelikeControlTaskPlugin>(m_config_ptr);

    // ------------------ 通用插件 ------------------
    m_roguelike_task_ptr->register_plugin<ScreenshotTaskPlugin>();
    m_roguelike_task_ptr->register_plugin<RoguelikeFormationTaskPlugin>(m_config_ptr, m_control_ptr);
    m_roguelike_task_ptr->register_plugin<RoguelikeResetTaskPlugin>(m_config_ptr, m_control_ptr);
    m_roguelike_task_ptr->register_plugin<RoguelikeSettlementTaskPlugin>(m_config_ptr, m_control_ptr);
    m_invest_ptr = m_roguelike_task_ptr->register_plugin<RoguelikeInvestTaskPlugin>(m_config_ptr, m_control_ptr);

    m_debug_ptr = m_roguelike_task_ptr->register_plugin<RoguelikeDebugTaskPlugin>(m_config_ptr, m_control_ptr);
    m_custom_ptr = m_roguelike_task_ptr->register_plugin<RoguelikeCustomStartTaskPlugin>(m_config_ptr, m_control_ptr);
    m_roguelike_task_ptr->register_plugin<RoguelikeShoppingTaskPlugin>(m_config_ptr, m_control_ptr)->set_retry_times(0);

    auto battle_plugin =
        m_roguelike_task_ptr->register_plugin<RoguelikeBattleTaskPlugin>(m_config_ptr, m_control_ptr);
    battle_plugin->set_retry_times(0).set_ignore_error(true);
    m_roguelike_task_ptr->register_plugin<RoguelikeRecruitTaskPlugin>(m_config_ptr, m_control_ptr)
        ->set_retry_times(2)
        .set_ignore_error(true);

    m_roguelike_task_ptr->register_plugin<RoguelikeSkillSelectionTaskPlugin>(m_config_ptr, m_control_ptr)
        ->set_retry_times(2)
        .set_ignore_error(true);

    auto encounter_plugin =
        m_roguelike_task_ptr->register_plugin<RoguelikeStageEncounterTaskPlugin>(m_config_ptr, m_control_ptr);
    encounter_plugin->set_retry_times(0);

    m_roguelike_task_ptr->register_plugin<RoguelikeLastRewardTaskPlugin>(m_config_ptr, m_control_ptr);

    m_roguelike_task_ptr->register_plugin<RoguelikeDifficultySelectionTaskPlugin>(m_config_ptr, m_control_ptr);
    m_roguelike_task_ptr->register_plugin<RoguelikeStrategyChangeTaskPlugin>(m_config_ptr, m_control_ptr);

    m_roguelike_task_ptr->register_plugin<RoguelikeIterateMonthlySquadPlugin>(m_config_ptr, m_control_ptr)
        ->set_retry_times(3);
    m_roguelike_task_ptr->register_plugin<RoguelikeIterateDeepExplorationPlugin>(m_config_ptr, m_control_ptr)
        ->set_retry_times(3);

    m_roguelike_task_ptr->register_plugin<RoguelikeLevelTaskPlugin>(m_config_ptr, m_control_ptr);

    // ------------------ 萨米主题专用插件 ------------------
    m_roguelike_task_ptr->register_plugin<RoguelikeFoldartalGainTaskPlugin>(m_config_ptr, m_control_ptr);
    m_foldartal_use_ptr =
        m_roguelike_task_ptr->register_plugin<RoguelikeFoldartalUseTaskPlugin>(m_config_ptr, m_control_ptr);
    m_foldartal_start_ptr =
        m_roguelike_task_ptr->register_plugin<RoguelikeFoldartalStartTaskPlugin>(m_config_ptr, m_control_ptr);
    m_roguelike_task_ptr->register_plugin<RoguelikeCollapsalParadigmTaskPlugin>(m_config_ptr, m_control_ptr);

    // ------------------ 萨卡兹主题专用插件 ------------------
    m_roguelike_task_ptr->register_plugin<RoguelikeInputSeedTaskPlugin>(m_config_ptr, m_control_ptr);

    // ------------------ 导航相关插件 ------------------
    m_roguelike_task_ptr->register_plugin<RoguelikeRoutingTaskPlugin>(m_config_ptr, m_control_ptr);
    m_roguelike_task_ptr->register_plugin<RoguelikeBoskyPassageRoutingTaskPlugin>(m_config_ptr, m_control_ptr);

    // ------------------ 界园主题专用插件 ------------------
    m_roguelike_task_ptr->register_plugin<RoguelikeCoppersTaskPlugin>(m_config_ptr, m_control_ptr);

    // ------------------ 黑流树海主题专用插件 ------------------
    m_blackflow_map_source_ptr = std::make_shared<blackflow::BlackFlowMapObservationSource>();
    m_blackflow_port_ptr =
        std::make_shared<blackflow::BlackFlowTaskPort>(callback, inst, TaskType, m_blackflow_map_source_ptr);
    m_blackflow_session_ptr = std::make_shared<blackflow::BlackFlowSession>();
    m_blackflow_port_ptr->set_collection_popup_session(m_blackflow_session_ptr);
    m_custom_ptr->set_blackflow_start_reward_observer(
        [weak_session = std::weak_ptr<blackflow::BlackFlowSession>(m_blackflow_session_ptr)](
            std::string_view reward) {
            if (const auto session = weak_session.lock(); session != nullptr) {
                session->set_start_reward(std::string(reward));
            }
        });
    encounter_plugin->set_blackflow_encounter_context_provider(
        [weak_config = std::weak_ptr<RoguelikeConfig>(m_config_ptr),
         weak_session = std::weak_ptr<blackflow::BlackFlowSession>(m_blackflow_session_ptr)]()
            -> std::optional<blackflow::LakeFairyContext> {
            const auto config = weak_config.lock();
            const auto session = weak_session.lock();
            if (config == nullptr || session == nullptr) {
                return std::nullopt;
            }
            const auto core_operator =
                config->status().opers.find(std::string(blackflow::AutomationCollectionCoreOperator));
            return blackflow::LakeFairyContext {
                .core_operator_elite_two =
                    core_operator != config->status().opers.end() && core_operator->second.elite >= 2,
                .ingots = session->run().resources.ingots,
            };
        });
    encounter_plugin->set_blackflow_encounter_choice_provider(
        [weak_session = std::weak_ptr<blackflow::BlackFlowSession>(m_blackflow_session_ptr)](
            std::string_view event_name) -> std::optional<std::size_t> {
            const auto session = weak_session.lock();
            return session == nullptr ? std::nullopt : session->preferred_encounter_choice(event_name);
        });
    encounter_plugin->set_blackflow_encounter_choice_order_provider(
        [weak_session = std::weak_ptr<blackflow::BlackFlowSession>(m_blackflow_session_ptr)](
            std::string_view event_name) -> std::optional<std::vector<std::string>> {
            const auto session = weak_session.lock();
            return session == nullptr ? std::nullopt : session->preferred_encounter_choice_order(event_name);
        });
    const auto observe_blackflow_page_content =
        [weak_session = std::weak_ptr<blackflow::BlackFlowSession>(m_blackflow_session_ptr)](
            std::string_view content,
            std::string_view source,
            bool expect_combat) {
            const auto session = weak_session.lock();
            if (session == nullptr || !session->page_context().has_value() ||
                session->page_context()->stage != blackflow::PageExecutionStage::Running ||
                blackflow::is_combat_node_type(session->page_context()->node_type) != expect_combat) {
                return;
            }
            std::string error;
            if (!session->observe_page_content(std::string(content), std::string(source), &error)) {
                Log.warn("BlackFlow direct page content observation ignored", error);
            }
        };
    encounter_plugin->set_event_observer(
        [observe_blackflow_page_content](std::string_view content) {
            observe_blackflow_page_content(content, "RoguelikeEvent", false);
        });
    encounter_plugin->set_event_capture_observer(
        [weak_port = std::weak_ptr<blackflow::IBlackFlowTaskPort>(m_blackflow_port_ptr)](
            std::string_view event_name,
            const cv::Mat& stitched_image) {
            const auto port = weak_port.lock();
            if (port == nullptr) {
                return;
            }
            std::string error;
            if (!port->capture_event_page(event_name, stitched_image, &error)) {
                Log.warn("BlackFlow stitched event capture failed", event_name, error);
            }
        });
    battle_plugin->set_stage_observer(
        [weak_session = std::weak_ptr<blackflow::BlackFlowSession>(m_blackflow_session_ptr)](
            std::string_view content) {
            const auto session = weak_session.lock();
            if (session == nullptr) {
                return;
            }
            std::string error;
            if (!session->observe_battle_stage_name(std::string(content), &error)) {
                Log.warn("BlackFlow battle stage observation ignored", error);
            }
        });
    battle_plugin->set_battle_result_observer(
        [weak_session = std::weak_ptr<blackflow::BlackFlowSession>(m_blackflow_session_ptr)](
            std::string_view stage_name,
            int total_kills) {
            const auto session = weak_session.lock();
            if (session == nullptr) {
                return;
            }
            std::string error;
            if (!session->observe_battle_total_kills(std::string(stage_name), total_kills, &error)) {
                Log.warn("BlackFlow battle total-kills observation ignored", error);
            }
        });
    battle_plugin->set_virtual_auto_skill_observer(
        [weak_session = std::weak_ptr<blackflow::BlackFlowSession>(m_blackflow_session_ptr)](
            std::string_view device_name) {
            const auto session = weak_session.lock();
            if (session != nullptr) {
                session->record_virtual_auto_skill_activation(device_name);
            }
        });
    m_roguelike_task_ptr->register_plugin<blackflow::BlackFlowCollectionPopupTaskPlugin>(
        [weak_port = std::weak_ptr<blackflow::IBlackFlowTaskPort>(m_blackflow_port_ptr)](
            std::string_view task,
            std::string* error) {
            const auto port = weak_port.lock();
            return port == nullptr ? true : port->capture_collection_popup(task, error);
        });
    m_roguelike_task_ptr->register_plugin<blackflow::BlackFlowNodeEvidenceTaskPlugin>(
        [weak_port = std::weak_ptr<blackflow::IBlackFlowTaskPort>(m_blackflow_port_ptr)](
            std::string_view task,
            std::optional<Rect> selected_button,
            std::string* error) {
            const auto port = weak_port.lock();
            return port == nullptr ? true : port->capture_get_drop(task, selected_button, error);
        });
    m_roguelike_task_ptr->register_plugin<blackflow::BlackFlowRunLogTaskPlugin>(
        m_config_ptr,
        m_control_ptr,
        m_blackflow_session_ptr,
        m_blackflow_port_ptr);
    m_roguelike_task_ptr->register_plugin<blackflow::BlackFlowLifecycleTaskPlugin>(
        m_config_ptr,
        m_control_ptr,
        m_blackflow_session_ptr,
        m_blackflow_port_ptr);
    m_roguelike_task_ptr->register_plugin<blackflow::BlackFlowRoutingTaskPlugin>(
        m_config_ptr,
        m_control_ptr,
        m_blackflow_session_ptr,
        m_blackflow_port_ptr);
    m_roguelike_task_ptr->register_plugin<blackflow::BlackFlowMovementTaskPlugin>(
        m_config_ptr,
        m_control_ptr,
        m_blackflow_session_ptr,
        m_blackflow_port_ptr);
    m_roguelike_task_ptr->register_plugin<blackflow::BlackFlowCultivationTaskPlugin>(
        m_config_ptr,
        m_control_ptr,
        m_blackflow_session_ptr,
        m_blackflow_port_ptr);
    m_roguelike_task_ptr->register_plugin<blackflow::BlackFlowAutomationStoreTaskPlugin>(
        m_config_ptr,
        m_control_ptr,
        m_blackflow_session_ptr,
        m_blackflow_port_ptr);
    m_roguelike_task_ptr->register_plugin<blackflow::BlackFlowNodeTaskPlugin>(
        m_config_ptr,
        m_control_ptr,
        m_blackflow_session_ptr,
        m_blackflow_port_ptr);
    m_roguelike_task_ptr
        ->register_plugin<blackflow::BlackFlowTrophyRewardTaskPlugin>(
            m_config_ptr,
            m_control_ptr,
            m_blackflow_session_ptr,
            m_blackflow_port_ptr)
        ->set_retry_times(0);
    m_subtasks.emplace_back(m_roguelike_task_ptr);
}

bool asst::RoguelikeTask::run()
{
    bool use_blackflow_map = false;
    {
        std::lock_guard lock(m_run_state_mutex);
        if (m_run_started) {
            Log.warn(__FUNCTION__, "RoguelikeTask is already running");
            return false;
        }
        m_run_started = true;
        use_blackflow_map = get_enable() && m_config_ptr->get_theme() == RoguelikeTheme::BlackFlow &&
                            m_blackflow_map_source_ptr != nullptr;
    }

    const auto finish_run = [&]() noexcept {
        if (use_blackflow_map) {
            try {
                if (m_blackflow_session_ptr != nullptr && m_blackflow_port_ptr != nullptr &&
                    m_blackflow_session_ptr->profile() == "automation_collection") {
                    json::object state = m_blackflow_session_ptr->run_log_state();
                    const blackflow::RunLogEvent event {
                        .level = blackflow::RunLogLevel::Info,
                        .action = "task.finished",
                        .phase = "completed",
                        .outcome = m_blackflow_session_ptr->terminated() ? "terminated" : "stopped",
                        .task = "RoguelikeTask",
                        .transaction_id = state.get("transaction_id", std::string()),
                        .state = std::move(state),
                        .details = {},
                    };
                    std::string ignored_error;
                    m_blackflow_port_ptr->record_run_event(
                        m_blackflow_session_ptr->run_revision(), event, nullptr, true, &ignored_error);
                }
            }
            catch (...) {
                // 终止阶段的诊断日志不得反过来使 noexcept 清理路径崩溃。
            }
            m_blackflow_map_source_ptr->release();
        }
        std::lock_guard lock(m_run_state_mutex);
        m_run_started = false;
    };

    if (use_blackflow_map) {
        std::string error;
        if (!m_blackflow_map_source_ptr->prepare(&error)) {
            Log.error("BlackFlow map perception preparation failed:", error);
            if (m_blackflow_session_ptr != nullptr) {
                m_blackflow_session_ptr->fail(
                    "perception_port_missing",
                    error.empty() ? "BlackFlow map recognition component failed to initialize" : error,
                    blackflow::FailureDisposition::StopTask);
                if (m_blackflow_session_ptr->claim_result_report()) {
                    auto info = basic_info_with_what("BlackFlowStrategyResult");
                    info["details"] = m_blackflow_session_ptr->result()->to_json();
                    callback(AsstMsg::SubTaskExtraInfo, info);
                }
            }
            finish_run();
            return false;
        }
    }
    try {
        const bool result = InterfaceTask::run();
        finish_run();
        return result;
    }
    catch (...) {
        finish_run();
        throw;
    }
}

bool asst::RoguelikeTask::set_params(const json::value& params)
{
    LogTraceFunction;
    std::lock_guard lock(m_run_state_mutex);
    if (m_run_started) {
        Log.warn(__FUNCTION__, "RoguelikeTask is running, cannot set params");
        return false;
    }
    if (!m_config_ptr->verify_and_load_params(params)) {
        m_roguelike_task_ptr->set_tasks({ "Stop" });
        return false;
    }

    const auto& theme = m_config_ptr->get_theme();
    const auto& mode = m_config_ptr->get_mode();

    m_roguelike_task_ptr->set_tasks({ theme + "@Roguelike@Begin" });

    if (mode == RoguelikeMode::Investment) {
        // 刷源石锭模式是否进入第二层
        if (m_config_ptr->get_invest_with_more_score()) {
            // 战斗后奖励默认
            Task.set_task_base(theme + "@Roguelike@DropsFlag", theme + "@Roguelike@DropsFlag_default");
            m_roguelike_task_ptr->set_times_limit("StageTraderInvestCancel", INT_MAX);
            if (theme == RoguelikeTheme::JieGarden) {
                m_roguelike_task_ptr->set_times_limit("StageTraderLeaveConfirm", INT_MAX);
            }
            else {
                m_roguelike_task_ptr->set_times_limit("StageTraderLeaveConfirm", 0, ProcessTask::TimesLimitType::Post);
            }
        }
        else {
            // 战斗后奖励只拿钱
            Task.set_task_base(theme + "@Roguelike@DropsFlag", theme + "@Roguelike@DropsFlag_mode1");
            m_roguelike_task_ptr->set_times_limit("StageTraderInvestCancel", 0);
            m_roguelike_task_ptr->set_times_limit("StageTraderLeaveConfirm", INT_MAX);
        }
    }
    else {
        // 重置战斗后奖励next
        Task.set_task_base(theme + "@Roguelike@DropsFlag", theme + "@Roguelike@DropsFlag_default");
        m_roguelike_task_ptr->set_times_limit("StageTraderInvestCancel", INT_MAX);
        m_roguelike_task_ptr->set_times_limit("StageTraderLeaveConfirm", INT_MAX);
    }

    bool stop_at_final_boss = params.get("stop_at_final_boss", false);
    // 傀影肉鸽3层和5层boss图标一样,禁用
    if (stop_at_final_boss && theme != RoguelikeTheme::Phantom) {
        m_roguelike_task_ptr->set_times_limit(theme + "@Roguelike@StageDreadfulFoe-5", 0);
    }
    else {
        // 重置boss进点
        m_roguelike_task_ptr->set_times_limit(theme + "@Roguelike@StageDreadfulFoe-5", INT_MAX);
    }

    if (theme == RoguelikeTheme::Sami) {
        if (auto opt = params.find<json::array>("start_foldartal_list"); opt) {
            std::vector<std::string> list;
            for (const auto& name : *opt) {
                if (std::string name_str = name.as_string(); !name_str.empty()) {
                    list.emplace_back(name_str);
                }
            }
            /* 由于插件 load_param返回值仅决定自身是否启用，二次读取参数进行验证 */
            if (list.empty()) {
                Log.error(__FUNCTION__, "| Empty start_foldartal_list");
                return false;
            }
        }
    }

    // 对局数据自动化收集是长期挂机策略：成功、战败、追猎弃局等正常局终止都必须继续下一局，
    // 只由用户主动停止或真正的任务错误结束，不受普通“探索次数”上限影响。
    const bool automation_collection =
        theme == RoguelikeTheme::BlackFlow && mode == RoguelikeMode::BlackFlowAutomationCollection;
    m_roguelike_task_ptr->set_times_limit(
        theme + "@Roguelike@StartExplore",
        automation_collection ? INT_MAX : params.get("starts_count", INT_MAX));
    // 通过 exceededNext 禁用投资系统，进入商店购买逻辑
    m_roguelike_task_ptr->set_times_limit(
        "StageTraderInvestSystem",
        params.get("investment_enabled", true) ? INT_MAX : 0);
    m_roguelike_task_ptr->set_times_limit(
        "StageTraderRefreshWithDice",
        params.get("refresh_trader_with_dice", false) ? INT_MAX : 0);

    for (const auto& plugin : m_roguelike_task_ptr->get_plugins()) {
        if (const auto& p_ptr = std::dynamic_pointer_cast<AbstractRoguelikeTaskPlugin>(plugin); p_ptr != nullptr) {
            p_ptr->set_enable(p_ptr->load_params(params));
        }
    }

    return true;
}
