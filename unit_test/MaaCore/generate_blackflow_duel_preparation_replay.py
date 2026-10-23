"""Inject unchanged production preparation methods into the controlled I/O replay."""
from pathlib import Path
import sys

from generate_blackflow_interaction_replay import extract

here = Path(__file__).resolve().parent
root = here.parents[1]
source = (root / 'src/MaaCore/Task/Roguelike/RoguelikeBattleTaskPlugin.cpp').read_text('utf-8')
result = (here / 'BlackFlowDuelPreparationReplay.cpp.in').read_text('utf-8')
for marker, name in [('SELECT_TIER', 'select_duel_preparation_tier'),
                     ('RECORD', 'record_duel_preparation'), ('PREPARE', 'run_preparation_phase')]:
    result = result.replace('/*' + marker + '*/', extract(source, 'bool asst::RoguelikeBattleTaskPlugin::' + name + '('))
Path(sys.argv[1]).write_text(result, 'utf-8')
