"""Connect real producer methods to the separately compiled production recorder.

Only controller/recognition/wait boundaries use the established replay stubs.
The recorder runs synchronously: its rejection prevents the next real producer input.
"""
from pathlib import Path
import subprocess
import sys
import re

here = Path(__file__).resolve().parent
output = Path(sys.argv[1])
subprocess.run([sys.executable, str(here / 'generate_blackflow_expedition_evidence_replay.py'), str(output)], check=True)
source = output.read_text('utf-8').split('int main(int argc, char** argv)')[0]
# Keep controlled boundary types out of the production TU's namespace (ODR).
assert source.count('namespace asst\n{') == 1, 'controlled namespace seam changed'
source = source.replace('namespace asst\n{',
    'namespace blackflow_replay\n{\nusing namespace ::asst;\nnamespace blackflow { using namespace ::asst::blackflow; }', 1)
source = source.replace('asst::RoguelikeStageEncounterTaskPlugin::', 'blackflow_replay::RoguelikeStageEncounterTaskPlugin::')
source = source.replace('namespace blackflow::perception {',
    'namespace blackflow::perception {\nusing ::asst::blackflow::perception::SacrificeItemList;')
source = source.replace('struct Scenario', 'using namespace blackflow_replay;\nstruct Scenario', 1)
boundary = source.index('using namespace blackflow_replay;')
source = source[:boundary] + re.sub(r'(?<![\w:])blackflow::', '::asst::blackflow::', source[boundary:])
source += (here / 'BlackFlowMultiStageReplay.cpp.in').read_text('utf-8')
output.write_text(source, 'utf-8')
