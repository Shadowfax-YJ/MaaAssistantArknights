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

## 2026-09-30 floor-title split regression

MAA-002 `run-20260930-204118-731004` stopped at 20:56:54 after entering floor three.
The lossless `debug/roguelike/2026.09.30-20.56.54.20_raw.png` is preserved as
`floor-three-split.png`. The detector split the title into 血色 `[593,2,53,32]`
and 空脉 `[640,5,50,25]`; node labels also intersect the title ROI.
The production entry-zoom successor fails on this image before the repair.
The lifecycle replay passes the actual recognized result into the production callback,
then merges a controlled two-node third-floor observation. Both current-floor and
run/map attribution must become three. The observation is not a full reconstruction
of the original screenshot; title recognition uses the unmodified original image.

All images retain their original bytes; no title or map pixels were edited.
Synthetic fragment geometry exercises the real joining fallback for row, distance,
size, overlapping-box, partial-title and conflicting-title rejection. It does not
claim those synthetic layouts were captured in game. All six title names are checked
through both PipelineAnalyzer and direct OCR; outer-floor return-title inheritance
and entry-popup priority are checked as well.

Sources and SHA-256:

- `floor-three-split.png`: original PNG from the failure above; SHA-256 `26fb5cc2415f81b463e88b0819df1a4ae916ffbd3cc43a9bf8fbe0167fb687cf`.
- `floor-1-title.jpg`: `run-20260930-204118-731004/images/000068-map-refresh-completed.jpg`; SHA-256 `20300472830fc0fc046b9da78c840f489259a5420737798f53f474a60501404a`.
- `floor-2-title.jpg`: `run-20260930-204118-731004/images/000876-map-refresh-completed.jpg`; SHA-256 `274dbdf7f27d56e4902fb1db348c753d29b4fa59e541fd677aa6978aa0ed9d4d`.
- `floor-5-title.jpg`: `run-20260930-194347-602471.zip!/run-20260930-194347-602471/images/004410-map-refresh-completed.jpg`; SHA-256 `77fa3a4c3854209e2067b640faf99602837f8357d6ab18455251e61ae694acfd`.
- `floor-6-title.jpg`: `run-20260930-194347-602471.zip!/run-20260930-194347-602471/images/005674-map-refresh-completed.jpg`; SHA-256 `d2cfc9976a53efe3afcde6619ef14e455b8a2cfa08ca9e034bb846a26e197c9f`.
- `floor-three-entry-next.jpg`: same failing run, floor-three entry reward, event 1666; SHA-256 `b284237bb3d7095b5ab4b95ed3ec327e50c00631748609800d7657051a9b6f43`.
- `floor-three-entry-close.jpg`: same failing run, floor-three entry reward, event 1670; SHA-256 `43f5df1503aea854b6531e2ab7509eb7820275825f9f8c4174341f473d460403`.
