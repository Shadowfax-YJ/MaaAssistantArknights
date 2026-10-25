from pathlib import Path
import re,sys
from generate_blackflow_interaction_replay import extract

here=Path(__file__).resolve().parent
source=(here.parents[1]/'src/MaaCore/Task/Roguelike/RoguelikeStageEncounterTaskPluginSacrifice.cpp').read_text('utf-8')
template=(here/'BlackFlowCivilizationReplay.h.in').read_text('utf-8')
signature=re.search(r'[^\n]+\n\s*asst::RoguelikeStageEncounterTaskPlugin::scan_sacrifice_natural_items\(',source).group()
method=extract(source,signature)
method=method.replace('asst::RoguelikeStageEncounterTaskPlugin::','Plugin::')
declaration=method[:method.index('{')].replace('Plugin::','').strip()+';'
template=template.replace('/*scan_declaration*/',declaration).replace('/*production_scan*/',method)
report=extract(source,'bool asst::RoguelikeStageEncounterTaskPlugin::report_sacrifice(').replace('asst::RoguelikeStageEncounterTaskPlugin::','Plugin::')
template=template.replace('/*production_report*/',report)
Path(sys.argv[1]).write_text(template,encoding='utf-8')
