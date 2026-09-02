#include "BlackFlowRoutingTaskPlugin.h"

#include "BlackFlowRoutingLoop.h"
#include "BlackFlowAutomationCollectionRules.h"

#include "Config/TaskData.h"
#include "Utils/Logger.hpp"

namespace asst::blackflow
{
bool BlackFlowRoutingTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (msg != AsstMsg::SubTaskStart || details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }
    const std::string task = details.get("details", "task", "");
    if (task == "BlackFlow@Roguelike@Routing") {
        m_pending = PendingWork::ObserveAndPlan;
        return true;
    }
    if (task == "BlackFlow@Roguelike@RoutingResume") {
        m_pending = PendingWork::ResumePendingMove;
        return true;
    }
    return false;
}

void BlackFlowRoutingTaskPlugin::reset_in_run_variables()
{
    m_pending = PendingWork::None;
    m_page_recovery_attempted = false;
    m_floor_recovery_attempted = false;
}

bool BlackFlowRoutingTaskPlugin::_run()
{
    LogTraceFunction;
    const PendingWork work = m_pending;
    m_pending = PendingWork::None;
    if (work == PendingWork::None) {
        return true;
    }
    Task.set_task_base("BlackFlow@Roguelike@RoutingAction", "BlackFlow@Roguelike@RecoveryFailed");

    if (m_session == nullptr || m_port == nullptr) {
        if (m_session != nullptr) {
            m_session->fail("perception_port_missing", "BlackFlow perception and task port is not attached");
        }
        Task.set_task_base("BlackFlow@Roguelike@RoutingAction", "BlackFlow@Roguelike@StrategyTerminated-Enter");
        report_outputs();
        return true;
    }

    if (m_session->profile() == "automation_collection") {
        const auto core_operator = m_config->status().opers.find(std::string(AutomationCollectionCoreOperator));
        const bool elite_two =
            core_operator != m_config->status().opers.end() && core_operator->second.elite >= 2;
        std::string fact_error;
        if (!m_session->set_automation_collection_core_operator_elite_two(elite_two, &fact_error)) {
            m_session->fail("core_operator_status_sync_failed", fact_error, FailureDisposition::RestartRun);
            Task.set_task_base("BlackFlow@Roguelike@RoutingAction", "BlackFlow@Roguelike@StrategyTerminated-Enter");
            report_outputs();
            return true;
        }
    }

    const RoutingCycleOutcome cycle = work == PendingWork::ResumePendingMove
                                          ? execute_pending_routing_cycle(*m_session, *m_port)
                                          : execute_routing_cycle(*m_session, *m_port);
    if (cycle.status == RoutingCycleStatus::NeedsPageRecovery) {
        const bool completed_page = m_session->page_context().has_value() &&
                                    m_session->page_context()->stage == PageExecutionStage::Resolved &&
                                    m_session->transaction() != nullptr &&
                                    m_session->transaction()->stage() == MoveTransactionStage::PageResolved;
        if (completed_page && !m_page_recovery_attempted) {
            m_page_recovery_attempted = true;
            Task.set_task_base("BlackFlow@Roguelike@RoutingAction", "BlackFlow@Roguelike@RecoverMap-Enter");
        }
        else if (!m_floor_recovery_attempted) {
            // 页面已经把角色送进下一层、但页面身份未能提前表达这种语义时，旧楼层模板必然建图失败。
            // 放弃整局前再 OCR 一次顶部楼层名。这里只是探测：保留当前楼层，让 set_current_floor
            // 用实际层号决定是否换代；若仍在同层，不得伪造新地图代或追忆四层。
            m_floor_recovery_attempted = true;
            Task.set_task_base("BlackFlow@Roguelike@RoutingAction", "BlackFlow@Roguelike@NextLevel-Enter");
        }
        else {
            m_page_recovery_attempted = false;
            m_floor_recovery_attempted = false;
            m_session->fail("map_rebuild_failed", cycle.error, FailureDisposition::RestartRun);
            Task.set_task_base("BlackFlow@Roguelike@RoutingAction", "BlackFlow@Roguelike@StrategyTerminated-Enter");
        }
        report_outputs();
        return true;
    }
    m_page_recovery_attempted = false;
    m_floor_recovery_attempted = false;
    if (cycle.status == RoutingCycleStatus::MovementInventoryObservationRequired) {
        Task.set_task_base("BlackFlow@Roguelike@RoutingAction", "BlackFlow@Roguelike@MovementInventoryCheck");
        report_outputs();
        return true;
    }
    if (cycle.status == RoutingCycleStatus::DirectExhaustionRequired) {
        Task.set_task_base("BlackFlow@Roguelike@RoutingAction", "BlackFlow@Roguelike@DirectExhaust-Enter");
        report_outputs();
        return true;
    }
    if (cycle.status == RoutingCycleStatus::MovementSelectionRequired) {
        Task.set_task_base("BlackFlow@Roguelike@RoutingAction", "BlackFlow@Roguelike@SelectMovement-Enter");
        report_outputs();
        return true;
    }
    if (cycle.status == RoutingCycleStatus::ReplanRequired) {
        Task.set_task_base("BlackFlow@Roguelike@RoutingAction", "BlackFlow@Roguelike@MapPrepare");
        report_outputs();
        return true;
    }
    if (cycle.status == RoutingCycleStatus::PreviewNeedsDismiss) {
        if (!cycle.failure_code.empty() || !cycle.error.empty()) {
            Log.info("BlackFlow move preview dismissed", cycle.failure_code, cycle.error);
        }
        Task.set_task_base("BlackFlow@Roguelike@RoutingAction", "BlackFlow@Roguelike@CancelNodeSelection");
        report_outputs();
        return true;
    }
    if (cycle.status == RoutingCycleStatus::MoveCommittedToMap) {
        Task.set_task_base("BlackFlow@Roguelike@RoutingAction", "BlackFlow@Roguelike@MapPrepare");
        report_outputs();
        return true;
    }
    if (cycle.status == RoutingCycleStatus::MoveCommitted) {
        Task.set_task_base("BlackFlow@Roguelike@RoutingAction", "BlackFlow@Roguelike@NodeDispatch");
        report_outputs();
        return true;
    }
    if (cycle.status == RoutingCycleStatus::SessionTerminated) {
        Task.set_task_base("BlackFlow@Roguelike@RoutingAction", "BlackFlow@Roguelike@StrategyTerminated-Enter");
        report_outputs();
        return true;
    }

    m_session->fail(cycle.failure_code, cycle.error, FailureDisposition::RestartRun);
    Task.set_task_base("BlackFlow@Roguelike@RoutingAction", "BlackFlow@Roguelike@StrategyTerminated-Enter");
    report_outputs();
    return true;
}
} // namespace asst::blackflow
