# Opening reward routing regression

The three JPEGs retain the original bytes from the archive. `provenance.json`
records the archive member, event sequence and SHA-256. They cover an ordinary
three-option page, a page containing 襁褓九头蛇, and multiple animals with an
incomplete ordinary-option view. No archive or original image was changed.

`run_blackflow_recovery_replay.ps1` loads production resources and OCR and checks
that each page enters `LastReward-EnterPoint` and cannot pass `SquadConfirm`'s
squad-header guard. The header ROI is inherited from the existing squad entry
recognizer. These are candidate frames, not the missing pre-click frames from
the historical bypasses; they do not reproduce an exact historical misclick.

`run_blackflow_interaction_replay.ps1` separately extracts the production reward
selector and `on_run_fails`. Injected storage/device results cover candidate,
pending-identity and final-record failures; confirmation timeout; the pending
record preceding confirmation; and stopping the parent after selection failure.
The generated `start-reward-events.json` is synthetic, combines independent
cases and is checked in as `unit_test/fixtures/BlackFlow/start-reward-lifecycle-events.json`.
The same bytes are used by the independent archive verifier and analysis tests.
The older `start-reward-events.json` fixture is retained for compatibility.
