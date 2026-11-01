# Co-op preparation count noise and false completion

Original v1.1.40 evidence from MAA-002 on 2026-10-30 (Asia/Shanghai):

- `002-2024.png`: preparation failure saved at 20:24:16.892. The final count
  observation was `2/2` plus an unrelated `X` (confidence 0.128875).
- `002-2230.png`: preparation failure saved at 22:29:57.238. The final count
  observation was `2/2` plus `GO` (confidence 0.459434).
- `003-2233.jpg`: MAA-003's successful before-start frame at 22:33:08.589,
  `run-20261030-214635-511746`, event 4693. Its preceding deployment checks
  also saw `2/2 + b` and `2/2 + 2`, then recovered with clean observations.

Both failed runs had deployed the mechanic and Kal'tsit successfully and still
showed the start button. The old final check required exactly one OCR result;
unrelated text stopped the parent without retaining an explicit failure, so
the task chain and `task.finished` incorrectly reported success.

Image bytes are copied without editing. `counters.json` preserves SHA-256,
the actual noisy OCR boxes/scores and original log lines. The failure screenshots
were saved just after the rejected reads, and the 003 image is a later successful
frame; re-running OCR need not reproduce the same incidental characters.

The native recovery replay checks the original images and recorded OCR results.
The production-method duel replay checks recorded noise, consecutive confirmation,
bounded uncertainty, conflicting counters, cancelled input and failed evidence saves.
The interaction replay follows the task result through `RoguelikeTask::run`,
including failure propagation, per-task reset and existing cancellation semantics.
Controller timing and input effects are controlled, not a live emulator execution.
