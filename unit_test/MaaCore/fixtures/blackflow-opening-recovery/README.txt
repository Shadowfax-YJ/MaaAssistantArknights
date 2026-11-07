BlackFlow opening recovery regression evidence (2026-10-18)

004-recruitment.png: unmodified 1280x720 screenshot from MAA-004/debug/interface/2026.10.18-13.45.55.703_raw.png.
005-recruitment.png: unmodified 1280x720 screenshot from MAA-005/debug/interface/2026.10.18-04.27.20.986_raw.png.
Both failures occurred after ConfirmRecruitCombination -> ChooseRecruitCombination while the game had advanced to the operator list.

BlackFlowOpeningRecoveryReplay links built production MaaCore objects. It loads actual task resources, runs native OCR and PipelineAnalyzer on the two original frames, and checks the next action is GiveUpRecruit1.
The blank image case is synthetic and only tests that a non-squad page cannot cause a swipe. Observed OCR strings are fed through production replacement and full-match logic; this is not a live squad-screen recognition test.
The failure-terminal case runs the real CustomTask offline and verifies its return value and error callback. No emulator is connected and no game input is sent.

Old squad-v2 resources: 7 checks passed, 18 failed.
Recovery-v3: 27 checks passed, 0 failed. Existing collection tests: 7 passed.
Full current-game cycles have not been run as part of this validation.

Data contract:
Existing processing-item / parts-box images, paths, delays, pairing and first-floor-only scope remain unchanged. A fresh exploration clears any incomplete in-memory group.
New diagnostics live in debug/roguelike/black_flow/opening_failures, separate from opening_cycles. Existing analysis enumerates opening_cycles/*/*_processing-item.png and *_parts-box.png and does not include failure evidence.
No signed-archive schema, independent verifier bundle, downstream consumer/plugin protocol or unknown-version policy changes. Old samples remain valid; cache and manual review do not need invalidation; statistical numerator/denominator definitions do not change. Historical reports and raw data are not rewritten.

2026-10-19 recovery-v4 additions:
squad-gap.png: live 005 squad page with a non-draggable gap; resized from 1920x1080 to MAA 1280x720. Three native minitouch swipes from (794,350) to (234,350) yielded zero name-row change; changing only start x to 940 (end 380) moved the cards.
005-network-400.png: unmodified 1280x720 failure screenshot at 2026-10-18 23:26:47, identifying a server disconnect rather than a squad-page failure.
