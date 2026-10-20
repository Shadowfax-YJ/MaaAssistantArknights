# Expedition picker recognition

All fixtures are original 1280 × 720 game frames. They are replayed through the
production matcher and event settlement pipeline without issuing device input.

- `battle-depart-with-expedition-label.png`: read-only ADB capture from instance
  MAA-003 on 2026-10-20, run `run-20261020-140336-195296`. The actual page is the
  `搏杀` battle departure reached from `原始娱乐`; the background map contains a
  `先行一步` node at `[361,156,56,17]`. The old OCR picker preempts the valid battle
  departure and repeatedly fails to recognize `ExpeditionBack`.
  SHA-256: `994774bddcad34cd25b8f2d945481cf915941729fed68184e85ff5de58475b21`.
- `picker-unselected.png`: the original game frame
  `debug/interface/2026.09.12-19.09.30.592_raw.png` from
  [upstream issue #18199](https://github.com/MaaAssistantArknights/MaaAssistantArknights/issues/18199),
  [report part 02](https://github.com/user-attachments/files/32144534/report_09-12_21-48-28_part02.zip).
  SHA-256: `c8d324390ec7ff21b8c9d02b3b6fe57dae8d9904694a8f6d1f453042996b7131`.

`BlackFlow@Roguelike@ExpeditionPicker.png` is an unscaled crop `[232,132,180,52]`
from `picker-unselected.png`, including the title, both ornaments, and `Scout`.
The production search ROI is `[230,120,200,70]`, with threshold `0.85`.
Template SHA-256: `5f6cae826b9cd6dae5cf6b99202c4e3e0d234fb71cc907739be27a95be73d828`.

The tests that hide the title or blank the operator/body area are explicit
in-memory counterfactuals. They are not additional captured game states.
The tests do not claim a full live-game expedition or battle run was performed.
