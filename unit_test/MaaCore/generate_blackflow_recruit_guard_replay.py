"""Replay the actual initial-core dispatch with injected recruitment outcomes."""
import argparse
import subprocess
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('output',type=Path)
parser.add_argument('--source-ref')
a=parser.parse_args()
repo=Path(__file__).resolve().parents[2]
path='src/MaaCore/Task/Roguelike/RoguelikeRecruitTaskPlugin.cpp'
src=subprocess.check_output(['git','show',f'{a.source_ref}:{path}'],cwd=repo).decode('utf-8') if a.source_ref else (repo/path).read_text('utf-8')
block=src[src.index('    const int core_char_recruit_count'):src.index('    blackflow::AutomationCollectionTeamProgress')]
template=(Path(__file__).with_name('BlackFlowRecruitGuardReplay.cpp.in')).read_text('utf-8')
a.output.write_text(template.replace('/*DISPATCH*/',block),encoding='utf-8')
