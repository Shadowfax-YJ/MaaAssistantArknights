"""Apply the dedicated opening UI to the pinned, compatible macOS GUI."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import subprocess

GUI_COMMIT = '2bc3b7b17a50dad7ddd1d55cc98841ea1b34d41f'
BASE_COMMIT = '0aa4624c2a9742617270832f0c467fa16a856df7'
ROOT = Path(__file__).resolve().parents[2]


def replace_function(text, signature, body):
    start = text.index('    ' + signature)
    end = text.index('\n    }', start) + len('\n    }')
    return text[:start] + '    ' + signature + '\n' + body + '\n    }' + text[end:]


def prepare(gui, version):
    actual = subprocess.check_output(['git', '-C', str(gui), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != GUI_COMMIT:
        raise ValueError('GUI revision must match the opening package snapshot')
    app = gui / 'MeoAsstMac'
    for name, destination in [('ContentView.swift', app / 'Navigation/ContentView.swift'),
                              ('MeoAsstMacApp.swift', app / 'MeoAsstMacApp.swift')]:
        destination.write_bytes((Path(__file__).parent / name).read_bytes())
    model = app / 'Model/MAAViewModel.swift'
    # Read the pinned original so repeated preparation is deterministic.
    text = subprocess.check_output(['git', '-C', str(gui), 'show', f'{GUI_COMMIT}:MeoAsstMac/Model/MAAViewModel.swift']).decode()
    text = replace_function(text, 'private func loadResource(channel: MAAClientChannel) async throws {',
                            '        try await loadResource(url: Bundle.main.resourceURL!, channel: channel)')
    text = replace_function(text, 'func reloadResources(channel: MAAClientChannel) async throws {',
                            '        try await loadResource(url: Bundle.main.resourceURL!, channel: channel)')
    text = replace_function(text, 'private static var userDirectory: URL {',
                            '        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!\n'
                            '            .appendingPathComponent("MAAPioneer", isDirectory: true)')
    text = text.replace('        try await MAAProvider.shared.setUserDirectory(path: Self.userDirectory.path)',
                        '        try FileManager.default.createDirectory(at: Self.userDirectory, withIntermediateDirectories: true)\n'
                        '        try await MAAProvider.shared.setUserDirectory(path: Self.userDirectory.path)')
    model.write_bytes(text.encode())
    log_view = app / 'Main Menu/OpenLogFileView.swift'
    text = subprocess.check_output(['git', '-C', str(gui), 'show', f'{GUI_COMMIT}:MeoAsstMac/Main Menu/OpenLogFileView.swift']).decode()
    text = replace_function(text, 'private static var userDirectory: URL? {',
                            '        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?\n'
                            '            .appendingPathComponent("MAAPioneer", isDirectory: true)')
    log_view.write_bytes(text.encode())
    info_path = app / 'Info.plist'
    info = plistlib.loads(info_path.read_bytes())
    for key in ('SUFeedURL', 'SUPublicEDKey', 'SUEnableInstallerLauncherService'):
        info.pop(key, None)
    info['MAAOpeningVersion'] = version
    info['CFBundleDisplayName'] = 'MAA 开拓者'
    info_path.write_bytes(plistlib.dumps(info))
    (gui / 'Version.xcconfig').write_bytes(b'MARKETING_VERSION = 6.0.0\nCURRENT_PROJECT_VERSION = 20261024\n')
    hashes = {}
    for name in ['resource/tasks/MiniGame/BlackFlow.json',
                 'src/MaaCore/Task/Interface/CustomTask.cpp',
                 'src/MaaCore/Task/Miscellaneous/BlackFlowDataCollectionTaskPlugin.cpp',
                 'src/MaaCore/Task/Miscellaneous/BlackFlowSquadSwipe.hpp']:
        hashes[name] = hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
    metadata = {'name': 'MAA Pioneer Opening Collection', 'version': version, 'revision': 'recovery-v4',
                'platform': 'macos-universal', 'architectures': ['arm64', 'x86_64'],
                'baseCommit': BASE_COMMIT, 'guiCommit': GUI_COMMIT,
                'buildCommit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                'criticalSourceSha256': hashes,
                'captureContract': {'changed': False, 'relativeDirectory': 'debug/roguelike/black_flow/opening_cycles/<cycle-id>',
                                    'images': ['processing-item', 'parts-box'],
                                    'failureDirectory': 'debug/roguelike/black_flow/opening_failures'},
                'dataDirectory': '~/Library/Application Support/MAAPioneer', 'automaticUpdates': False,
                'validationLimit': 'Windows recovery-v4 has live-run validation; macOS CI validates native loading and task registration, not a connected game run.'}
    (ROOT / 'build-info-opening-macos.json').write_bytes((json.dumps(metadata, ensure_ascii=False, indent=2)+'\n').encode())
    return metadata


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gui', type=Path, default=ROOT / 'src/MaaMacGui')
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.gui, args.version), ensure_ascii=False, indent=2))
