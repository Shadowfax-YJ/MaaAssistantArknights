# Real refresh confirmation regression

`refresh-confirm-wallet-16.jpg` is the unmodified 1280 × 720 screenshot from
MAA-003, `run-20260930-062054-472028/images/000550-recovery-store-refresh-failed.jpg`.
SHA-256: `e21cad865331ae5729d675b2d5d9dc9d45ba1e6cd8b7cceb06b8981e4033db82`.

The run used v1.1.16. At 06:29:43 the refresh button opened a real payment dialog
asking for 4 ingots, with 16 remaining. Its right button says `确定`; the
inventory-full prompt used by the v1.1.16 template says `确认` and has a different
button-row image. The live refresh score was 0.869918, below 0.9, on every retry.
No confirmation click or debit occurred; collection stopped at 06:29:55.
Native replay of the saved JPEG scored 0.872369647 and rejected it too.

The replacement template is a lossless, unscaled crop `[278, 454, 792, 66]` of this
real refresh frame. Its SHA-256 is
`bf1c7f6ae497662b7b5c76e8ee49f81e6bf16d90ca14c360c2cf0a89b9d7dc63`.
The threshold, search ROI, right-button click rectangle, legacy red template and
payment receipt checks remain unchanged. The September 29 inventory-full frame
is now a negative example, alongside both real paid-shelf false-positive frames.

The native `BlackFlowRecoveryReplay` checks recognition and the actual next-task
pipeline on this frame, and rejects the unpaid dialog as a settled shop page.
JPEG quality 45, brightness gains 0.8/1.15, missing buttons, a shifted row, an
isolated green check and a legacy red button are explicitly synthetic variants;
they do not represent additional live recordings.

Command: `unit_test/MaaCore/run_blackflow_recovery_replay.ps1 -CoreBuildDirectory
build/core-shop-refresh -OutputDirectory build/diagnostics/20260930/recovery-red`.
The updated expectations failed with the v1.1.16 resource: 43 passed, 5 failed.
The same executable with the replacement resource passed all 48 checks.
No complete live-game run with this fix has been recorded.
