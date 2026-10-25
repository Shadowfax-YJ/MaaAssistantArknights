from pathlib import Path
import argparse,subprocess
from generate_blackflow_interaction_replay import extract
p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('--source-ref');a=p.parse_args()
here=Path(__file__).resolve().parent;repo=here.parents[1];path='src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowTaskPort.cpp'
s=subprocess.check_output(['git','show',a.source_ref+':'+path],cwd=repo).decode('utf-8') if a.source_ref else (repo/path).read_text('utf-8')
r=(here/'BlackFlowOverloadReplay.cpp.in').read_text('utf-8')
for marker,signature in [('OVERLOAD_CLEANUP','bool BlackFlowTaskPort::cleanup_overloaded_inventory('),('OVERLOAD_WRAPPER','bool BlackFlowTaskPort::cleanup_open_inventory_if_overloaded('),('RECOGNIZES_TEXT','bool recognizes_text_fragment('),('INVENTORY_REGION','cv::Mat inventory_card_region(')]:r=r.replace('/*'+marker+'*/',extract(s,signature))
helpers=['std::string normalize_inventory_name(', 'std::optional<InventoryPartCategory> inventory_part_category(', 'std::optional<std::size_t> inventory_part_discard_priority(', 'bool inventory_part_precedes_quota_excess(']
r=r.replace('/*POLICY_HELPERS*/','\n'.join(extract(s,f) for f in helpers))
recognition_path='src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowMovementRecognition.cpp'
recognition=(repo/recognition_path).read_text('utf-8')
r=r.replace('/*OVERLOAD_RECOGNITION*/','\n'.join(extract(recognition,f) for f in ['std::optional<InventoryCapacity> inventory_capacity_from_ocr(', 'InventoryOverloadObservation recognize_inventory_overload(']))
# Production inventory names include multi-line declarations; extract only the constants used by this method.
import re
names=['InventorySwipeTask','InventorySwipeToStartTask','InventoryResetCollapseTask','InventoryResetExpandTask','InventoryResetWaitTask','InventoryOverloadOpenTask','InventoryOverloadedBannerTask','InventoryAllItemsTask','InventoryDiscardButtonTask','InventoryEquippedDiscardConfirmTask','InventoryDiscardDetailWaitTask','InventoryOverloadCloseTask','InventoryUnchangedFrameMaximumDifference','InventoryNewRightColumnMinimumX','InventoryMaximumDiscardAttempts','InventoryDiscardPriorityTask','InventoryDiscardBeforeQuotaExcessTask','InventoryNaturalPriorityTask','InventoryConceptPriorityTask']
block='\n'.join(re.search(r'constexpr [^;]+\b'+name+r'\s*=[^;]+;',s).group() for name in names)
r=r.replace('/*CONSTANTS*/',block);a.output.write_text(r,'utf-8')
