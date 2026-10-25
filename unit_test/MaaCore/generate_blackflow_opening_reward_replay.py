"""Replay the live reward method with actual OCR/images and controlled device input."""
from pathlib import Path
import re
import sys

from generate_blackflow_interaction_replay import extract

here = Path(__file__).resolve().parent
repo = here.parents[1]
source = (repo / 'src/MaaCore/Task/Roguelike/RoguelikeCustomStartTaskPlugin.cpp').read_text('utf-8')
method = extract(source, 'bool asst::RoguelikeCustomStartTaskPlugin::hijack_reward()')
branch = extract(method, 'if (automation_collection)')
branch, count = re.subn(r'std::random_device\s*\{\s*\}\(\)', 'std::mt19937::result_type { 0 }', branch)
assert count == 1, 'controlled opening entropy seam changed'
assert branch.count('static thread_local std::mt19937 random_engine') == 1
# Each fixture starts at the same controlled entropy state; live RNG lifetime is unchanged.
branch = branch.replace('static thread_local std::mt19937 random_engine', 'std::mt19937 random_engine')
template = (here / 'BlackFlowOpeningRewardControllerReplay.h.in').read_text('utf-8')
Path(sys.argv[1]).write_text(template.replace('/*production_reward_branch*/', branch), 'utf-8')
