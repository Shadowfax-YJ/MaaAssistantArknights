"""Generate a replay from the production methods; only controller/OCR/storage I/O is replaced.

Run run_blackflow_evidence_replay.ps1 to compile and execute with the repository's OpenCV.
No emulator, game state, or historical archive is modified.
"""
import argparse
from pathlib import Path


def method(path, signature):
    source = path.read_text(encoding='utf-8')
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    core = here.parents[1] / 'src/MaaCore/Task/Roguelike/BlackFlow'
    source = (here / 'BlackFlowEvidenceReplay.cpp.in').read_text(encoding='utf-8')
    for marker, file, signature in (
        ('RELOCATE', 'BlackFlowAutomationStoreTaskPlugin.cpp', 'bool BlackFlowAutomationStoreTaskPlugin::relocate_selection('),
        ('CLICK', 'BlackFlowAutomationStoreTaskPlugin.cpp', 'bool BlackFlowAutomationStoreTaskPlugin::click_verified_selection('),
        ('FINALIZE', 'BlackFlowAutomationStoreTaskPlugin.cpp', 'void BlackFlowAutomationStoreTaskPlugin::finalize_pending_purchase('),
        ('CLEAR_PURCHASE', 'BlackFlowAutomationStoreTaskPlugin.cpp', 'void BlackFlowAutomationStoreTaskPlugin::clear_pending_purchase('),
        ('SELECT_VOUCHER', 'BlackFlowTaskPort.cpp', 'bool BlackFlowTaskPort::select_recruitment_voucher('),
    ):
        source = source.replace('/*' + marker + '*/', method(core / file, signature))
    args.output.write_text(source, encoding='utf-8')
