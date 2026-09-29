# Refresh confirmation false-positive regression

Unmodified 1280 × 720 screenshots, with SHA-256 hashes:

- `refresh-paid-wallet-18.jpg`: `run-20260929-115004-683159/images/002435-recovery-store-refresh-failed.jpg`; `ebd9a02b9dbc1bbf1eae07e704e0d21ea2daf8c80b135ad7511d535a0a79fb75`.
- `refresh-paid-final.jpg`: the same run, `images/002436-task-finished-completed.jpg`; `8549c8a8a5c56a4a47b270009a2f1e1bb9e95f048c2e52cb1c29847d4a86c97e`.
- `shared-confirm-dialog.jpg`: `run-20260913-195846-828939/images/003537-task-finished-completed.jpg`; `33c2b36bc67ab80d73221149bc68838808a6f63651f2bc5c6cc6dffa78c990c5`.

The September 29 run used v1.1.15. Refresh debited 26 → 18, but the green check-button
template matched shelf coordinates `[954, 442, 188, 54]` at 0.801171601, exceeding 0.8.
The receipt gate therefore never read the wallet and stopped after 30 seconds. Both
terminal frames reproduce the false match through the production Matcher.

The positive frame is a real inventory-full dialog using the shared cancel/confirm
row. It is **not** a captured refresh dialog. The new BlackFlow-only template is an
unscaled, unretouched crop `[278, 454, 792, 66]` of this frame, including both buttons.
Matching the row does not establish payment; the existing shop and consecutive
exact-debit checks remain necessary. Its click rectangle is restricted to the right
confirmation button, rather than the entire recognized row.

Native replay also checks a JPEG-quality-45 re-encoding, individual buttons covered,
an isolated old green check button, a shifted row, and a legacy red button pasted
at its expected position. These are explicitly synthetic in-memory variants, not
additional game recordings. The legacy template is retained unchanged. No new
complete live-game run has been recorded with the fix.

Entry point: `unit_test/MaaCore/run_blackflow_recovery_replay.ps1`, already included
in the Windows release pipeline. The first eight new checks produced six failures
before the resource fix and all passed after it. Further occlusion/compression
checks protect the two-button evidence and normal compressed screenshots.
