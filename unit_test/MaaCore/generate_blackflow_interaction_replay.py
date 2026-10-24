"""Replay production movement selection and reward clicks with injected controller/OCR I/O."""
import argparse
import subprocess
from pathlib import Path

def extract(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--source-ref', help='Replay old production methods to demonstrate regression failures')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    repo = here.parents[1]
    core = Path('src/MaaCore/Task/Roguelike/BlackFlow')
    result = (here / 'BlackFlowInteractionReplay.cpp.in').read_text('utf-8')
    for marker, file, signature in (
        ('INVENTORY_RESET_EARLY', 'BlackFlowMovementRecognition.cpp', 'bool inventory_reset_can_confirm_early('),
        ('SELECT', 'BlackFlowMovementTaskPlugin.cpp', 'BlackFlowMovementTaskPlugin::SelectionOutcome\n'),
        ('WALK', 'BlackFlowMovementTaskPlugin.cpp', 'bool BlackFlowMovementTaskPlugin::verify_walking_on_map('),
        ('CLOSE', 'BlackFlowMovementTaskPlugin.cpp', 'bool BlackFlowMovementTaskPlugin::close_panel('),
        ('INVENTORY_OBSERVE', 'BlackFlowMovementTaskPlugin.cpp', 'bool BlackFlowMovementTaskPlugin::observe_inventory('),
        ('INVENTORY_REGION', 'BlackFlowMovementTaskPlugin.cpp', 'cv::Mat inventory_card_region('),
        ('INVENTORY_NAMES_UNCHANGED', 'BlackFlowMovementTaskPlugin.cpp', 'bool inventory_name_rows_unchanged('),
        ('INVENTORY_SCAN', 'BlackFlowMovementTaskPlugin.cpp', 'bool BlackFlowMovementTaskPlugin::scan_inventory_frame('),
        ('INVENTORY_STABLE', 'BlackFlowMovementTaskPlugin.cpp', 'bool BlackFlowMovementTaskPlugin::wait_for_inventory_stability('),
        ('INVENTORY_REOPEN', 'BlackFlowMovementTaskPlugin.cpp', 'bool BlackFlowMovementTaskPlugin::reopen_inventory_for_scan('),
        ('INVENTORY_ANALYZE', 'BlackFlowMovementTaskPlugin.cpp', 'BlackFlowMovementTaskPlugin::InventoryAnalysisOutcome BlackFlowMovementTaskPlugin::analyze_inventory_frame('),
        ('REWARD', 'BlackFlowNodeEvidenceTaskPlugin.cpp', 'bool BlackFlowNodeEvidenceTaskPlugin::_run('),
        ('GUARD', 'BlackFlowNodeEvidenceTaskPlugin.cpp', 'void BlackFlowNodeEvidenceTaskPlugin::click_drop_with_progress_check('),
    ):
        path = core / file
        source = subprocess.check_output(['git', 'show', f'{args.source_ref}:{path.as_posix()}'], cwd=repo).decode('utf-8') if args.source_ref else (repo / path).read_text('utf-8')
        body = extract(source, signature) if signature in source else ''
        result = result.replace('/*' + marker + '*/', body)
    path = Path('src/MaaCore/Task/Roguelike/RoguelikeCustomStartTaskPlugin.cpp')
    source = subprocess.check_output(['git', 'show', f'{args.source_ref}:{path.as_posix()}'], cwd=repo).decode('utf-8') if args.source_ref else (repo / path).read_text('utf-8')
    reward_method = extract(source, 'bool asst::RoguelikeCustomStartTaskPlugin::hijack_reward()')
    result = result.replace('/*START_REWARD*/', extract(reward_method, 'if (automation_collection)'))
    failure_method = extract(source, 'bool asst::RoguelikeCustomStartTaskPlugin::on_run_fails()')
    result = result.replace('/*START_REWARD_FAILURE*/', failure_method[failure_method.index('{'):])
    completion_source = (repo / 'src/MaaCore/Task/Interface/RoguelikeTask.cpp').read_text('utf-8')
    if args.source_ref:
        completion_source = subprocess.check_output(['git', 'show', f'{args.source_ref}:src/MaaCore/Task/Interface/RoguelikeTask.cpp'], cwd=repo).decode('utf-8')
    result = result.replace('/*TASK_COMPLETION*/', extract(completion_source, 'bool asst::RoguelikeTask::run()').replace('bool asst::RoguelikeTask::run()', 'bool RoguelikeTask::run()'))
    args.output.write_text(result, encoding='utf-8')
