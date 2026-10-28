"""Replay pending UI operations through native recognition, recorders and routing."""
import argparse
from pathlib import Path
import re
import subprocess
from generate_blackflow_interaction_replay import extract

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--source-ref')
a = p.parse_args()
here = Path(__file__).resolve().parent
repo = here.parents[1]


def read(path):
    return (subprocess.check_output(['git', 'show', a.source_ref + ':' + path], cwd=repo).decode('utf-8')
            if a.source_ref else (repo / path).read_text('utf-8'))


movement = read('src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowMovementTaskPlugin.cpp')
header = (repo / 'src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowMovementTaskPlugin.h').read_text('utf-8')
encounter = read('src/MaaCore/Task/Roguelike/RoguelikeStageEncounterTaskPlugin.cpp')
text = (here / 'BlackFlowUiRecoveryReplay.cpp.in').read_text('utf-8')
top = movement[:movement.index('bool BlackFlowMovementTaskPlugin::verify(')]
constants = '\n'.join(re.findall(r'constexpr (?:int|std::string_view)\s+[^;]+;', top))
constants += '\n' + extract(movement, 'bool task_contains_inventory_name(')
constants += '\n' + extract(movement, 'std::optional<std::string_view> inventory_boundary_label(')
text = text.replace('/*CONSTANTS*/', constants)
types = '\n'.join(extract(header, token + name) + ';' for token, name in [
    ('enum class ', 'PendingWork'), ('enum class ', 'SelectionOutcome'),
    ('enum class ', 'InventoryAnalysisOutcome'), ('struct ', 'PanelItem'),
    ('struct ', 'PanelNameHit'), ('struct ', 'PanelFrame'), ('struct ', 'InventoryItem'),
    ('struct ', 'InventoryBoundaryItem'), ('struct ', 'InventoryFrame')])
text = text.replace('/*MOVEMENT_TYPES*/', types)
names = [('bool ', '_run'), ('bool ', 'observe_inventory'), ('BlackFlowMovementTaskPlugin::SelectionOutcome\n    ', 'select_movement'),
         ('bool ', 'verify_walking_on_map'), ('bool ', 'ensure_panel_open'), ('bool ', 'close_panel'),
         ('bool ', 'wait_for_movement_network'), ('bool ', 'scan_current_frame'), ('bool ', 'analyze_frame'),
         ('bool ', 'scan_inventory_frame'), ('bool ', 'wait_for_inventory_stability'),
         ('bool ', 'reopen_inventory_for_scan'),
         ('BlackFlowMovementTaskPlugin::InventoryAnalysisOutcome ', 'analyze_inventory_frame'),
         ('void ', 'record_panel_evidence'), ('void ', 'record_inventory_evidence'),
         ('bool ', 'report_target_observation'), ('const MovementSpec* ', 'movement_from_name'),
         ('std::optional<int> ', 'remaining_uses_from_text'), ('std::vector<std::string> ', 'ocr_candidates'),
         ('std::vector<std::string> ', 'inventory_ocr_candidates')]
bodies = []
for prefix, name in names:
    sig = prefix + 'BlackFlowMovementTaskPlugin::' + name + '('
    if name == 'select_movement':
        sig = 'BlackFlowMovementTaskPlugin::SelectionOutcome\n'
    if sig in movement:
        body = extract(movement, sig).replace('BlackFlowMovementTaskPlugin::', 'Movement::')
        if a.source_ref:
            body = body.replace('Movement::verify_walking_on_map()', 'Movement::verify_walking_on_map(bool*)')
            body = body.replace('Movement::close_panel(std::string* error)', 'Movement::close_panel(std::string* error, bool*)')
            body = body.replace('Movement::ensure_panel_open(std::string* error)', 'Movement::ensure_panel_open(std::string* error, bool*)')
        bodies.append(body)
text = text.replace('/*MOVEMENT_METHODS*/', '\n'.join(bodies))
names = ['report_expedition', 'abort_expedition', 'confirm_expedition_dispatch', 'wait_for_secondary_event']
bodies = [extract(encounter, 'bool asst::RoguelikeStageEncounterTaskPlugin::' + name + '(')
          .replace('asst::RoguelikeStageEncounterTaskPlugin::', 'Expedition::') for name in names]
caller = extract(encounter, 'bool asst::RoguelikeStageEncounterTaskPlugin::handle_expedition_picker(')
caller = caller[caller.index('{') + 1:caller.index('    if (m_expedition_context.has_value()')]
text = text.replace('/*EXPEDITION_METHODS*/', '\n'.join(bodies))
text = text.replace('/*PENDING_DISPATCH*/', caller)
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(text, encoding='utf-8')
