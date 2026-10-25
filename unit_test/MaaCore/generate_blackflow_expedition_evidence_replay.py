"""Compile actual expedition lifecycle and click methods with controlled I/O."""
from pathlib import Path
import sys
import re
from generate_blackflow_interaction_replay import extract

here = Path(__file__).resolve().parent
source = (here.parents[1] / 'src/MaaCore/Task/Roguelike/RoguelikeStageEncounterTaskPlugin.cpp').read_text('utf-8')
result = (here / 'BlackFlowExpeditionEvidenceReplay.cpp.in').read_text('utf-8')
methods = ['refresh_expedition_context', 'report_expedition', 'abort_expedition', 'capture_expedition_candidates', 'handle_expedition_choice',
           'confirm_expedition_dispatch', 'wait_for_secondary_event', 'handle_expedition_picker', 'select_analyzed_option',
           'reset_in_run_variables']
methods += ['refresh_encounter_choice_context', 'report_encounter_choice', 'prepare_encounter_choice', 'plan_encounter_choice', 'randomize_encounter_choice_order',
            'confirm_encounter_choice', 'observe_encounter_choice_result', 'confirm_cargo_choice']
for name in methods:
    prefix = 'void' if name in ('refresh_expedition_context', 'refresh_encounter_choice_context', 'reset_in_run_variables') else 'bool'
    method = extract(source, prefix + ' asst::RoguelikeStageEncounterTaskPlugin::' + name + '(')
    if name == 'randomize_encounter_choice_order':
        method, count = re.subn(r'std::random_device\s*\{\s*\}\(\)', 'std::mt19937::result_type { 1234 }', method)
        assert count == 2, 'controlled entropy seam no longer matches production'  # controlled entropy only
    result = result.replace('/*' + name + '*/', method)
header = (here.parents[1] / 'src/MaaCore/Task/Roguelike/RoguelikeStageEncounterTaskPlugin.h').read_text('utf-8')
for name, member in [('EncounterChoiceEvidence', 'm_encounter_choice'), ('SacrificeState', 'm_sacrifice')]:
    start = header.index('    struct ' + name)
    end = header.index('} ' + member + ';', start) + len('} ' + member + ';')
    result = result.replace('/*' + member + '*/', header[start:end])
sacrifice = (here.parents[1] / 'src/MaaCore/Task/Roguelike/RoguelikeStageEncounterTaskPluginSacrifice.cpp').read_text('utf-8')
for name in ['refresh_sacrifice_context', 'report_sacrifice', 'handle_sacrifice_event', 'handle_sacrifice_picker', 'finish_sacrifice_civilization']:
    result = result.replace('/*' + name + '*/', extract(sacrifice, 'bool asst::RoguelikeStageEncounterTaskPlugin::' + name + '('))
result = result.replace('/*encounter_tests*/', (here / 'BlackFlowEncounterChoiceReplayTests.h').read_text('utf-8'))
recognition = (here.parents[1] / 'src/MaaCore/Vision/Roguelike/BlackFlow/SacrificeRecognition.cpp').read_text('utf-8')
probe = (here / 'BlackFlowSacrificeRecognitionReplay.h.in').read_text('utf-8')
probe = probe.replace('/*production_sacrifice_item_list*/', extract(recognition, 'std::optional<SacrificeItemList> sacrifice_item_list('))
result = result.replace('/*sacrifice_recognition_probe*/', probe)
Path(sys.argv[1]).write_text(result, 'utf-8')
