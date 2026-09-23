"""Execute production difficulty methods with injected game frames and task outcomes."""
import argparse
from pathlib import Path
import subprocess
import re

from generate_blackflow_interaction_replay import extract

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--source-ref')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
path = 'src/MaaCore/Task/Roguelike/RoguelikeDifficultySelectionTaskPlugin.cpp'
source = (subprocess.check_output(['git', 'show', f'{args.source_ref}:{path}'], cwd=root).decode('utf-8')
          if args.source_ref else (root / path).read_text('utf-8'))
source = re.sub(r'\b(bool|int)\s+(asst::RoguelikeDifficultySelectionTaskPlugin::)', r'\1 \2', source)
signatures = [
    'bool asst::RoguelikeDifficultySelectionTaskPlugin::verify(',
    'bool asst::RoguelikeDifficultySelectionTaskPlugin::_run()',
    'int asst::RoguelikeDifficultySelectionTaskPlugin::detect_current_difficulty() const',
    'bool asst::RoguelikeDifficultySelectionTaskPlugin::select_difficulty(',
    'int asst::RoguelikeDifficultySelectionTaskPlugin::detect_blackflow_home_difficulty(',
    'bool asst::RoguelikeDifficultySelectionTaskPlugin::verify_blackflow_difficulty(',
]
methods = '\n'.join(extract(source, signature) for signature in signatures if signature in source)
template = Path(__file__).with_name('BlackFlowDifficultyReplay.cpp.in').read_text('utf-8')
args.output.write_text(template.replace('/*METHODS*/', methods), encoding='utf-8')
