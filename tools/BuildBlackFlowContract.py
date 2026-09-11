"""Export the producer-owned verifier and fixtures; consumers pin bundle SHA-256."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def build(output: Path) -> dict:
    root = Path(__file__).resolve().parents[1]
    sources = {
        'VerifyBlackFlowRunArchive.py': root / 'tools/VerifyBlackFlowRunArchive.py',
        'raw-contract.json': root / 'docs/blackflow/raw-contract.json',
        'fixtures/signed-run.zip': root / 'unit_test/fixtures/BlackFlow/signed-run.zip',
    }
    output.mkdir(parents=True, exist_ok=True)
    files = {}
    for name, source in sources.items():
        data = source.read_bytes()
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        files[name] = {'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data)}
    manifest = {
        'bundle_version': 1, 'contract_id': 'maa.blackflow.raw', 'contract_version': 1,
        'source_repository': 'MaaAssistantArknights',
        'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
        'source_dirty': bool(subprocess.check_output(['git', 'status', '--porcelain', '--',
            'tools/VerifyBlackFlowRunArchive.py', 'docs/blackflow/raw-contract.json'], cwd=root)),
        'files': files, 'expected': {'fixtures/signed-run.zip': 'valid_local_signature'},
    }
    (output / 'bundle.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    print(json.dumps(build(parser.parse_args().output), indent=2))
