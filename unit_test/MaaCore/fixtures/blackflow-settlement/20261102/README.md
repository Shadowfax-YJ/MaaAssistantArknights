# Optional experience and monthly commission overlays after settlement statistics

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
task overlay after the entrance first appears. All monthly-task positive cases
use the unmodified screenshot. Experience cases use the original screenshot,
plus one explicitly title-masked variant checking the existing checkbox without
a title; that variant is never used as a monthly-task positive. Page transitions,
input effects and the reused entrance/opening frames remain controlled.

## MAA-003 monthly commission regression in v1.1.41

`tasks.png` is the user's original 1280x720 screenshot of 进行中的本月委托.
MAA-003's installed Core and BlackFlow resource matched the published v1.1.41
package byte for byte. In `run-20261102-094356-204843`, the log reports the
entrance at 09:46:54.832 Asia/Shanghai, then repeatedly sees the monthly page
until the user stops the task at 09:47:21.625.

The previous "tasks" replay was a masked statistics frame retaining a checkbox.
It could not catch this failure: the real monthly page has no checkbox. Replacing
it with this image makes v1.1.41 fail with `FAIL optional pages tasks only: exit
settlement`; a one-frame native next-candidate check also fails. The masked
positive task fixture has been removed, not retained as evidence of coverage.

The monthly resource now recognizes the actual title, reuses the original
ClickToStartPoint rectangle and 1500ms delay, and returns through next. The
parent remains DoNothing and the settlement C++ plugin is unchanged. The same
monthly node is also a successor of CollectionBegin and Begin, so restarting
while this page is visible can reach the return chain. Both real startup paths
are replayed; CollectionBegin failed with no matching successor before this
connection was added. CollectionBegin keeps ExitThenAbandon and Abandon as its
first two successors, with the monthly page after them; the existing routing
contract test remains unchanged. Tests cover repeated/ineffective clicks, the existing return budget, evidence-save
failure, cancellation and a page change during recording. A title-masked frame
is used only as an explicitly synthetic negative check, alongside actual
experience, summary, statistics, detail, entrance, opening and battle frames.

The recorder sample in `experience-event.json` now comes from navigation-ready
in the passing resource-chain replay. Its scope remains run_settlement and
not_an_acquisition; it supplies no new reward or duplicate statistics report.
