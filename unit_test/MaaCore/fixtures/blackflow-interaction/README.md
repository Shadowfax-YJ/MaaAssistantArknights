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
