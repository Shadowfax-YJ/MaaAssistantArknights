"""Replay the production settlement plugin with native recognition and lifecycle records."""
import argparse
from pathlib import Path
import subprocess
import sys
from generate_blackflow_interaction_replay import extract

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--source-ref')
a = p.parse_args()
here = Path(__file__).resolve().parent
repo = here.parents[1]
a.output.parent.mkdir(parents=True, exist_ok=True)
base = a.output.parent / 'ui-base.cpp'
subprocess.run([sys.executable, str(here / 'generate_blackflow_ui_recovery_replay.py'), str(base)], check=True)
prefix = base.read_text('utf-8').split('\nint main(')[0]
path = 'src/MaaCore/Task/Roguelike/RoguelikeSettlementTaskPlugin.cpp'
s = (subprocess.check_output(['git', 'show', a.source_ref + ':' + path], cwd=repo).decode('utf-8')
     if a.source_ref else (repo / path).read_text('utf-8'))
methods = []
for name in ('verify', '_run', 'reset_in_run_variables', 'record_blackflow_settlement',
             'fail_blackflow_settlement', 'report_blackflow_statistics', 'prepare_blackflow_settlement',
             'get_settlement_info', 'wait_for_whole_page'):
    signature = ('void ' if name == 'reset_in_run_variables' else 'bool ') + 'asst::RoguelikeSettlementTaskPlugin::' + name + '('
    if signature in s:
        methods.append(extract(s, signature).replace('asst::RoguelikeSettlementTaskPlugin::', 'Settlement::'))
    elif name == 'reset_in_run_variables':
        methods.append('void Settlement::reset_in_run_variables() {}')
template = (here / 'BlackFlowSettlementReplay.cpp.in').read_text('utf-8')
template = template.replace('/*METHODS*/', '\n'.join(methods))
a.output.write_text(prefix + '\n' + template, encoding='utf-8')
