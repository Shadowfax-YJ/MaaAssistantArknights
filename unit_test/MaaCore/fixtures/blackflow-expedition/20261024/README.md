# Expedition choice, dispatch and archive evidence

`lifecycle-payloads.json` is emitted by the actual collector choice, click, picker,
confirmation and reporting methods in `BlackFlowExpeditionEvidenceReplay.cpp.in`.
Only controller, OCR, task execution and persistence boundaries are controlled.
The replay checks this fixture for structural equality, including both choices,
Gummy/Ethan/core operator and story/map success paths. The test also covers
unknown and already-elite-two state snapshots; core eligibility is supplied at
this seam, not proof that an already-elite-two core is eligible in the real policy.
The existing eligibility and core-away routing rules remain authoritative.

`producer-events.json` contains twelve corresponding cases passed through the
production `BlackFlowSession`, evidence bridge, `BlackFlowTaskPort`, map source
and run logger. Runtime revisions are rebased to each real test Session. These
are actual serialized logger envelopes and files, with synthetic 1280 x 720 solid
frames; they are not live game expedition captures or complete real runs.
`evidence-frame.jpg` is one such production-encoded frame. Tests reopen every
written image, check its node directory, replay the JSONL and wrap the same
events/images in the independent verifier's v1 signed archive fixture.

The identical `producer-events.json` is shared with the analysis consumer at
`tests/fixtures/blackflow-expedition-events.json`. Missing old choices remain
unknown. Planned/click-issued/prepared records are not dispatch receipts;
duplicate callbacks do not add visits, dispatches or rewards. A confirmed
dispatch retains pre-dispatch runtime elite separately from rule-predicted
post-dispatch elite; it does not claim image-confirmed elite.

`secondary-story.jpg` is an unmodified original archived event page, copied
read-only from the existing 2026-10-23 expedition audit index. Provenance,
original member, sequence and SHA-256 are in `secondary-story-provenance.json`.
It validates the existing story-body ROI and later-text inference only, not an
independently recorded initial choice. It is outside the 127 unresolved initial
choice audit and does not repair or reclassify those archives.

Commands from the repository root:

```powershell
./unit_test/MaaCore/run_blackflow_expedition_evidence_replay.ps1 -Python D:/Projects/lubiao-analysis/.venv/Scripts/python.exe
./unit_test/MaaCore/run_blackflow_recovery_replay.ps1 -CoreBuildDirectory build/core-shop-refresh
$env:BLACKFLOW_ANALYSIS_REPO='D:/Projects/lubiao-analysis'
& D:/Projects/lubiao-analysis/.venv/Scripts/python.exe -m pytest unit_test/MaaCore/test_blackflow_expedition_contract.py -q
```

No live-game full expedition, release, deployment, instance replacement or
historical archive revision was performed.
