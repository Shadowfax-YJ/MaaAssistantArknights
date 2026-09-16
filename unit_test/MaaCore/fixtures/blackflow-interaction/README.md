# BlackFlow interaction regression evidence

Original JPEG bytes, copied without changing either source run:

- `stuck-before.jpg`: run-20260915-203239-216559, BF-T8-23, event 2914.
- `stuck-after.jpg`: the same transaction, event 18050. Rewards are still present after 1,893 attempts.
- `claim-before.jpg`: the same run, event 2622, immediately before claiming five ingots.
- `claim-after.jpg`: event 2630. Five ingots have been added and the first card has disappeared.

`run_blackflow_interaction_replay.ps1` extracts the production selection and click methods;
controller, panel OCR and storage are injected. The walking OCR geometry is from
run-20260915-183834-079101, BF-A3-286 frames 4–10: name `[1154,191,72,20]`,
loaded marker outside the panel OCR ROI, both engine cards no longer loaded.
Map icon responses are controlled test inputs, not screenshots from the failed run.

Use `-SourceRef blackflow-v1.1.9 -OutputDirectory build/blackflow-interaction-before`
to run the old implementation against the same cases. Old code rejects walking and
keeps attempting unchanged rewards. The generated `reward-click-events.json` contains
synthetic controller feedback; timestamps and ordering are supplied by the test harness.

2026-09-16: this replay also extracts the actual `close_panel` method. It rejects empty,
unknown and one-frame map observations and accepts a delayed close during final settling.
Reward tests inject failure of the old-page capture after a successful page transition,
while unchanged rewards and unavailable evidence retain their bounded retries.
Run `-SourceRef f7dfb68170c92787babcfe7fb08dc71fe6d6140c` to reproduce the v1.1.10 failures.
Actual recruitment/map/panel recognition and the Session lifecycle are covered by the
companion `run_blackflow_transition_replay.ps1` and `blackflow-transitions` fixtures.
