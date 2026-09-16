# BlackFlow page-transition regressions

Unmodified JPEG bytes from the four runs supplied on 2026-09-16:

- `recruit-caster.jpg`: run-20260916-103643-185881, event 3927; reward claim has opened recruitment.
- `recruit-supporter.jpg`: run-20260916-150622-117068, event 5126; same incorrect reward recovery entry.
- `floor-four-small.jpg`: run-20260916-123549-547901, event 3895; valid fourth-floor map rejected against the previous move.
- `floor-four-large.jpg`: the same run, event 3911; floor recovery has enlarged that map.
- `panel-open.jpg`: run-20260916-075606-029671, event 1041; selecting Structural Principle.
- `panel-closed.jpg`: the same run, event 1042; error capture already shows the map.

Run `unit_test/MaaCore/run_blackflow_transition_replay.ps1 -CoreBuildDirectory BUILD_DIR`
after building MaaCore with MSVC / RelWithDebInfo. It links the production Core objects,
loads current TaskData and recognizers, and executes real Session methods. No emulator is touched.
The floor recovery entrance is read from the production routing call site, not duplicated in the test.
Session map observations are synthetic two-node inputs reproducing the empty-landing/pursuit lifecycle;
they are not claimed to reconstruct every node or animation frame of the original run.

Before the repair, the real Session rejects the fourth-floor observation, both recruitment images
fail the recovery entrance, and the small-map recovery enlarges the map then loses floor recognition.
Negative cases retain rejection of unconfirmed/skipped-floor transitions, stale pursuit authorization,
and normal same-floor/node-page behavior. Original reward, panel and map recognition are also checked.

The separate interaction replay extracts the production click and panel-close methods, with controlled
capture/controller/OCR responses. It covers the missing capture boundary and delayed final close checks.
The close-check frames between the two original screenshots were not saved: injected timing is a
boundary regression, not a claim about the exact original OCR or capture delay.

Both replays and the C++ BlackFlow suite gate Windows release artifacts before upload/publishing.
