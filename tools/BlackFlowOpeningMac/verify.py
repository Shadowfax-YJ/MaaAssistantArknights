"""Verify the actual packaged universal app without starting a game."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--app', type=Path, required=True)
parser.add_argument('--version', required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
app = args.app.resolve()
output = args.output.resolve()
info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
assert info['MAAOpeningVersion'] == args.version
assert info['CFBundleIdentifier'] == 'wiki.lubiao.MAAPioneer'
assert 'SUFeedURL' not in info
executable = app / 'Contents/MacOS' / info['CFBundleExecutable']
libraries = list(app.rglob('libMaaCore.dylib'))
assert len(libraries) == 1, libraries
library = libraries[0]
deployment_targets = {}
for path in [executable, *app.rglob('*.dylib')]:
    subprocess.run(['lipo', str(path), '-verify_arch', 'arm64', 'x86_64'], check=True)
    commands = subprocess.check_output(['otool', '-l', str(path)], text=True)
    minimums = re.findall(r'cmd LC_(?:BUILD_VERSION|VERSION_MIN_MACOSX).*?\b(?:minos|version) ([0-9.]+)', commands, flags=re.S)
    assert minimums, f'No macOS deployment target found: {path}'
    deployment_targets[str(path.relative_to(app))] = minimums
minimum_macos = max([info['LSMinimumSystemVersion'], *[v for values in deployment_targets.values() for v in values]],
                    key=lambda value: tuple(map(int, value.split('.'))))
resources = app / 'Contents/Resources'
metadata = json.loads((resources / 'build-info-opening-macos.json').read_bytes())
task_path = resources / 'resource/tasks/MiniGame/BlackFlow.json'
assert hashlib.sha256(task_path.read_bytes()).hexdigest() == metadata['criticalSourceSha256']['resource/tasks/MiniGame/BlackFlow.json']
core = ctypes.CDLL(str(library))
core.AsstSetUserDir.argtypes = [ctypes.c_char_p]
core.AsstSetUserDir.restype = ctypes.c_bool
core.AsstGetVersion.restype = ctypes.c_char_p
core.AsstLoadResource.argtypes = [ctypes.c_char_p]
core.AsstLoadResource.restype = ctypes.c_bool
core.AsstCreate.restype = ctypes.c_void_p
core.AsstAppendTask.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
core.AsstAppendTask.restype = ctypes.c_int
core.AsstDestroy.argtypes = [ctypes.c_void_p]
with tempfile.TemporaryDirectory(prefix='maa-pioneer-smoke-') as directory:
    os.chdir(resources)
    assert core.AsstSetUserDir(directory.encode())
    assert core.AsstGetVersion().decode() == args.version
    assert core.AsstLoadResource(str(resources).encode()), 'Resource load failed'
    handle = core.AsstCreate()
    assert handle
    try:
        task_id = core.AsstAppendTask(handle, b'Custom', b'{"task_names":["MiniGame@BlackFlow@Begin"]}')
        assert task_id > 0, 'Opening task registration failed'
    finally:
        core.AsstDestroy(handle)
result = {'passed': True, 'version': args.version, 'architectures': ['arm64', 'x86_64'],
          'resourceLoad': True, 'openingTaskRegistration': task_id, 'gameStarted': False,
          'bundleIdentifier': info['CFBundleIdentifier'], 'dataContractChanged': False}
result['minimumMacOS'] = minimum_macos
result['deploymentTargets'] = deployment_targets
output.write_bytes((json.dumps(result, indent=2)+'\n').encode())
print(json.dumps(result))
