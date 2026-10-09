# Movement button label compatibility

The v1.1.20 run `MAA-002/run-20261009-134035-184183` repeatedly opened a
reachable battle preview, rejected it as `preview controls are not visible`,
cancelled the selection and replanned. There were 252 failed `move.preview`
attempts; the last was interrupted when the task stopped. Battle-intel previews
also failed at the same control-recognition gate. The game now displays
`步行前往` or `加工品前往`, while all 11 depart recognizers only accepted the
`出发前往` template.

All fixture images are copied without altering their bytes:

- `walk-preview.jpg`: this run's `images/000158-move-preview-failed.jpg`,
  SHA-256 `f7f0457091ab043cf7714817ec259c9b2d42614016b5623768f0a32f80fd8610`.
- `walk-preview-next.jpg`: `images/000185-move-preview-failed.jpg`,
  SHA-256 `3655543c0edda1f2872eef847fe4bf7a29987af9c6820397a5924c22949dbe98`.
- `battle-intel-preview.jpg`: `images/000093-battle-intel-preview-failed.jpg`,
  SHA-256 `fcbd4d2f004ffbb0aeb1f7aeb4046f753738b92c8efb45d6f53161ddaf91b075`.
- `map-before-preview.jpg`: `images/000091-map-refresh-completed.jpg`,
  SHA-256 `a8bd4b83ba930a53ec3653fd706b36dd946b5bd3401e3f941a08116269956d91`.
- `processed-preview.png`: the user's October 9 attachment
  `codex-clipboard-a19b8332-f055-48ba-9729-420d268870fc.png`, 1278 × 716,
  SHA-256 `ffe59fa8114e3c537965ec06a069c02974850465475e42038f3a968b4214136b`.
- `legacy-preview.jpg`: the September 29 run
  `run-20260929-115004-683159/images/002318-move-preview-completed.jpg`,
  SHA-256 `59b9f4b1c386174b61a6dc1fce54f95fe66c1d479043b3c9638f0580a601d905`.

The new templates are lossless, unscaled full-label crops. Coordinates use
`[x, y, width, height]` in the original image:

- `MovePreviewWalkEnter.png`: `[1147, 541, 103, 27]` from `walk-preview.jpg`,
  SHA-256 `bacab5409916ff492c70e67412076213effb32380c8aecd9a43e52dc24c6481d`.
- `MovePreviewProcessedEnter.png`: `[1125, 539, 125, 28]` from `processed-preview.png`,
  SHA-256 `470ae032c6f24c981d14f483e391ded19cc0052171bee690e5b65632901a3d34`.

The original `出发前往` template is retained byte-for-byte. Every affected task
accepts the same three legacy, walking and processing templates. Matching thresholds remain 0.8; recognition ROI, fixed
click rectangle, passive observation, retry budgets and task successors remain
unchanged. Native `Matcher` covers the control gate used by preview sampling,
battle intel and map-overlay rejection; `PipelineAnalyzer` covers all 11 resource
entries. The original action-point OCR still reads `-1` from each preview.
The labels establish button visibility; movement selection, consumption and log
identity still come from the existing planner, HUD and transaction validation.

Native replay also covers an independent later walking frame, the battle-intel
frame, the legacy button through all 11 resource entrances, and real map, recruit,
inventory, shop and home-page negative examples. JPEG quality 45, brightness gains 0.8/1.15, standard
1280 × 720 resizing, removed buttons and a pasted unreachable button are
explicitly synthetic variants, not additional live observations. Normalized
frames retain both button recognition and the `-1` cost. The unreachable variant
continues to match `MovePreviewCannotEnter` without matching a reachable button.

Command:
`unit_test/MaaCore/run_blackflow_recovery_replay.ps1 -CoreBuildDirectory
build/package-blackflow-v1.1.20/core -OutputDirectory build/diagnostics/20261009/movement-labels/all-buttons-replay`.
With both new labels and all affected entrances checked, the old resource gave
72 passing / 36 failing tests. After the final resource change and
negative/robustness coverage, all 137 tests pass. The existing 34 transition replays and 352 routing,
transaction and archive tests also pass. These checks exercise native recognizers
and resource graphs; they do not issue controller clicks or claim a complete
live-game run. The new replay is included by the existing recovery replay entry
already required in release CI.

The subsequent local Windows x64 build uses `tools/BuildBlackFlowWindowsPackage.ps1`.
Core, app host, updater and collection WPF UI build successfully; startup DLL
policy and resource loading pass before packaging and after extracting the ZIP.
The archive contains the exact current task JSON, all three template byte sequences
and corresponding file-list entries. The local package
keeps the configured v1.1.20 identity and includes uncommitted fixes; it is not a
published release. The current package is built separately in
`build/diagnostics/20261009/windows-local-build-all-buttons`; the earlier package
and the running extracted instance are not overwritten.
SHA-256 of the current three-label package:
`e84e36f5d7b3a499ef55b60aeebd4e456e1352c7083fff15796493fb5ac6337b`.
