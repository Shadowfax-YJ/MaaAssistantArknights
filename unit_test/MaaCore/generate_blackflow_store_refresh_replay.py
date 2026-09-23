"""Replay actual shop refresh callbacks and receipt polling with injected vision/controller I/O."""
import argparse
import subprocess
from pathlib import Path
from generate_blackflow_interaction_replay import extract
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('output',type=Path)
parser.add_argument('--source-ref')
a=parser.parse_args()
repo=Path(__file__).resolve().parents[2]
path='src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowAutomationStoreTaskPlugin.cpp'
src=subprocess.check_output(['git','show',f'{a.source_ref}:{path}'],cwd=repo).decode('utf-8') if a.source_ref else (repo/path).read_text('utf-8')
template=Path(__file__).with_name('BlackFlowStoreRefreshReplay.cpp.in').read_text('utf-8')
start='    if (work == PendingWork::ShopRefreshOpening) {'
if start not in src: start='    if (work == PendingWork::ShopRefreshCompleted) {'
branches=src[src.index(start):src.index('    if (work == PendingWork::ShopLeave) {')]
signature='bool BlackFlowAutomationStoreTaskPlugin::verify_shop_refresh_receipt()'
receipt=extract(src,signature) if signature in src else ''
a.output.write_text(template.replace('/*DISPATCH*/',branches).replace('/*RECEIPT*/',receipt),encoding='utf-8')
