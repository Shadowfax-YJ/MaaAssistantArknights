# Experience overlay after settlement statistics

MAA-002 v1.1.40, `run-20261102-022334-524296`, 2026-11-02 03:28–03:29
Asia/Shanghai. The original run first advanced the summary, reported score 1063,
then confirmed statistics. It remained on the level-109 源流堆栈 overlay until
the user stopped MAA at 03:29:43.

`summary.jpg` and `statistics.jpg` are unchanged run images. `experience.png`
is the user's unchanged 1280×720 screenshot, with its SHA-256 and sources in
`provenance.json`. The existing checkbox template matches at `[624,536,44,47]`
(score 0.984911), outside the statistics ROI `[580,590,130,130]`; none of the
statistics/ending header templates matches this overlay.

The current fix reuses `Roguelike@GamePassTheEndConfirm` through the actual
resource `next` chain. The experience-specific C++ classifier was removed;
`RoguelikeSettlementTaskPlugin.cpp/.h` remain identical to v1.1.40 HEAD.

The replay executes TaskData.next with native PipelineAnalyzer matching. It
covers no optional page, experience only, tasks only, both pages, and a delayed
task overlay after the entrance first appears. Task overlays are controlled
frames retaining the existing confirmation control; they are not claimed to
be original task-page screenshots. No task-page title or fixed sequence is
assumed by production code. Input effects and later entrance/opening frames
remain controlled/reused as described in provenance.json.

The recorder sample in `experience-event.json` now comes from navigation-ready
in the passing resource-chain replay. Its scope remains run_settlement and
not_an_acquisition; it supplies no new reward or duplicate statistics report.
