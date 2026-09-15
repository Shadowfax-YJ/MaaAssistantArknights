"""Replay production movement selection and reward clicks with injected controller/OCR I/O."""
import argparse
import subprocess
from pathlib import Path

def extract(source, signature):
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
    parser.add_argument('--source-ref', help='Replay old production methods to demonstrate regression failures')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    repo = here.parents[1]
    core = Path('src/MaaCore/Task/Roguelike/BlackFlow')
    result = (here / 'BlackFlowInteractionReplay.cpp.in').read_text('utf-8')
    for marker, file, signature in (
        ('SELECT', 'BlackFlowMovementTaskPlugin.cpp', 'BlackFlowMovementTaskPlugin::SelectionOutcome\n'),
        ('WALK', 'BlackFlowMovementTaskPlugin.cpp', 'bool BlackFlowMovementTaskPlugin::verify_walking_on_map('),
        ('REWARD', 'BlackFlowNodeEvidenceTaskPlugin.cpp', 'bool BlackFlowNodeEvidenceTaskPlugin::_run('),
        ('GUARD', 'BlackFlowNodeEvidenceTaskPlugin.cpp', 'void BlackFlowNodeEvidenceTaskPlugin::click_drop_with_progress_check('),
    ):
        path = core / file
        source = subprocess.check_output(['git', 'show', f'{args.source_ref}:{path.as_posix()}'], cwd=repo).decode('utf-8') if args.source_ref else (repo / path).read_text('utf-8')
        body = extract(source, signature) if signature in source else ''
        result = result.replace('/*' + marker + '*/', body)
    args.output.write_text(result, encoding='utf-8')
