# Zero-cost processed movement

Source: MAA-003 `run-20261009-172505-455085`, collector v1.1.21.
On floor 2, M07 (`rogue_6_scrap_M_07`) selected the node `未知的诡秘`.
All eight independently captured preview failures show `加工品前往` with the
action-point cost `-0`; the button is reachable. The sampler exhausted its
24-frame semantic stability budget because the fee OCR was missing. It then
cancelled, replanned and opened the same preview again. The ninth attempt was
interrupted by the user's Stop; it is not an additional timeout fixture.

The JPEGs are copied byte-for-byte from the source run's `images/` directory:

- `001560-move-preview-failed.jpg`: SHA-256
  `06188d5753eae3bb6d46384a314f17adeaf28781603858452e19734f7be01fbf`.
- `001587-move-preview-failed.jpg`: SHA-256
  `80f04c849525bb23fc559e4a205d1b7ad3274d1ddf5cdade29f1229064210aa8`.
- `001614-move-preview-failed.jpg`: SHA-256
  `aa7a003035ccdbc456802155a846eb406b852b5e78668483c61a3323c6576c55`.
- `001641-move-preview-failed.jpg`: SHA-256
  `9ba363e1b1126b1f1013901516296c1ff6cd42747f7ec6e55ce6630b6dd98ffa`.
- `001668-move-preview-failed.jpg`: SHA-256
  `ae92a426d30edaaf3af61ca7cb57a5112465db6e250ca36def9d4cf5457e03fd`.
- `001695-move-preview-failed.jpg`: SHA-256
  `31d72d53ea27f2b4a85796ff1f5eb335ff795006d197522be5c34c2ea123eb95`.
- `001722-move-preview-failed.jpg`: SHA-256
  `c70ee4b8f0ad454d0e5a6260b0d05f248ddb167248d2c57064288921d46fc97a`.
- `001749-move-preview-failed.jpg`: SHA-256
  `c1679b94f4ad5cee9331c191faba4e6a5ce6562870c57a204a3174584d8210f7`.
- `charged-preview.jpg`: the same run's
  `images/001056-move-preview-completed.jpg`, SHA-256
  `03dddc1ed3e5af4092a2388e336e8752fa00de775275c6266f9b5e560bc24f3b`.
  This processed battle preview displays `-1` and its successful preview event
  records `state.transaction.authoritative_cost=1`.

The original fee ROI `[1089, 544, 38, 27]` clips the left part of the zero.
The native TaskPort recognizer fails on six of these eight images, including
`001587`; button matching succeeds. Moving only x to 1084 is insufficient:
`001749` is misread as -7. Moving only x to 1080 also misses the complete
regression: `charged-preview.jpg` becomes -7, despite the other five charged
fixtures passing. Widening to 47 pixels then misreads that same fee as 0.

The final ROI `[1080, 542, 38, 30]` captures the full zero with vertical padding.
All eight costs are exactly 0; the five existing walking, battle-intel,
processed and legacy previews and all ten successful previews from this run
retain the exact -1 fee. A diagnostic scan found adjacent crop heights and y
positions also pass the 23 original images. The strict cost bounds and
consecutive semantic-frame requirement are unchanged; unreadable costs are
never replaced with zero.

`BlackFlowMovePreviewReplay.h` uses the native OCR engine and loaded task resource
to assert exact zero on every failure frame. It also removes just the physical
fee text from walking, processed, legacy and zero-cost previews while leaving
the depart button visible: all four must reject the missing fee. These removed
text variants are synthetic negatives, not live observations. The existing
replay continues to cover charged previews and button recognition.

Run the release CI's existing recovery replay entry:

```powershell
./unit_test/MaaCore/run_blackflow_recovery_replay.ps1 `
    -CoreBuildDirectory build/package-blackflow-v1.1.20/core `
    -OutputDirectory build/diagnostics/20261009/zero-cost/replay
```

Before the fix the new zero-cost tests give 143 passing / 6 failing checks.
Adding the independently charged frame exposes the shift-only candidate as
160 passing / 1 failing checks. The final resources pass all 161 recovery and
recognition checks, including the existing 11 entry points on that charged
frame. The separate production TaskPort replay confirms the exact expected
fee, reachable control, recognized identity and semantic stability on all 23
original images, without an in-memory ROI override.

The compiled Core cost recognizer is unchanged between v1.1.20 and v1.1.21;
the harness loads the current repository resources at runtime. Screenshot
replay does not send controller input or establish a completed live-game run.
