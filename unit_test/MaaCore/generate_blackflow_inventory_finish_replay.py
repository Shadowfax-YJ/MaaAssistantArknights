"""Replay real overload cleanup and ordinary observation through native recognition."""
import argparse
from pathlib import Path
import re
import subprocess
import sys

from generate_blackflow_interaction_replay import extract

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--source-ref')
a = p.parse_args()
here = Path(__file__).resolve().parent
repo = here.parents[1]
base = a.output.parent / 'ui-base.cpp'
a.output.parent.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, str(here / 'generate_blackflow_ui_recovery_replay.py'), str(base)], check=True)
text = base.read_text('utf-8').split('\nint main(')[0]
text = text.replace('bool cleanup_open_inventory_if_overloaded(bool& done,std::string*) override{done=false;return true;}',
    'std::function<bool(bool&,std::string*)> cleanup_hook;\n'
    'bool cleanup_open_inventory_if_overloaded(bool& done,std::string* error) override{'
    'if(cleanup_hook)return cleanup_hook(done,error);done=false;return true;}')
# The original replay only invokes recognized collapses. Here the ineffective
# top-left input is itself under test, so only the real collapse region may close.
text = text.replace('else if(page==Page::Inventory){page=Page::Map;}',
    'else if(page==Page::Inventory){const int x=r.x+r.width/2,y=r.y+r.height/2;'
    'if(x>=589&&x<690&&y>=639&&y<720)page=Page::Map;}')
path = 'src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowTaskPort.cpp'
source = (subprocess.check_output(['git', 'show', a.source_ref + ':' + path], cwd=repo).decode('utf-8')
          if a.source_ref else (repo / path).read_text('utf-8'))
names = ['InventoryOverloadPromptTask', 'InventorySwipeTask', 'InventorySwipeToStartTask',
         'InventoryResetCollapseTask', 'InventoryResetExpandTask', 'InventoryResetWaitTask',
         'InventoryOverloadOpenTask', 'InventoryOverloadedBannerTask', 'InventoryAllItemsTask',
         'InventoryDiscardButtonTask', 'InventoryEquippedDiscardConfirmTask', 'InventoryDiscardDetailWaitTask',
         'InventoryOverloadCloseTask', 'InventoryUnchangedFrameMaximumDifference', 'InventoryNewRightColumnMinimumX',
         'InventoryMaximumDiscardAttempts', 'InventoryDiscardPriorityTask', 'InventoryDiscardBeforeQuotaExcessTask',
         'InventoryNaturalPriorityTask', 'InventoryConceptPriorityTask']
constants = '\n'.join(re.search(r'constexpr [^;]+\b' + name + r'\s*=[^;]+;', source).group() for name in names)
helpers = '\n'.join(extract(source, signature) for signature in [
    'std::string normalize_inventory_name(', 'std::optional<InventoryPartCategory> inventory_part_category(',
    'std::optional<std::size_t> inventory_part_discard_priority(', 'bool inventory_part_precedes_quota_excess(',
    'std::optional<int> recognize_inventory_part_valuation(', 'cv::Mat inventory_card_region(',
    'bool recognizes_text_fragment('])
bodies = '\n'.join(extract(source, 'bool BlackFlowTaskPort::' + name + '(')
                   .replace('BlackFlowTaskPort::', 'Cleanup::') for name in
                   ('cleanup_overloaded_inventory', 'cleanup_open_inventory_if_overloaded', 'cleanup_depart_inventory_overload'))
template = (here / 'BlackFlowInventoryFinishReplay.cpp.in').read_text('utf-8')
for marker, body in [('CONSTANTS', constants), ('HELPERS', helpers), ('CLEANUP', bodies)]:
    template = template.replace('/*' + marker + '*/', body)
a.output.write_text(text + '\n' + template, encoding='utf-8')
