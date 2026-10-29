# MAA-002 full inventory tail rebound

The nine JPEGs are unchanged evidence from v1.1.38,
`run-20261029-190705-320729`, floor 5, artifact `BF-A18-137`.
The instance stopped at 2026-10-29 19:47:09 Asia/Shanghai after eight
forward swipes, reporting that the whole inventory end was not reached.
Overload cleanup had already returned the capacity to 12/12.

Frame 3 reaches the final column (板藤, 雾滚草, 洛伦茨之眼). Frames 4–9
show the same tail at different horizontal offsets. There is no empty slot.
`provenance.json` records original hashes and all eight movement instances,
including the two separate 一次性喷气背包 items and their remaining charges.

`BlackFlowInventoryTailReplay.h`, run by the existing inventory-finish replay,
uses native OCR, star recognition, the extracted production scanner and the
real session/diagnostic writer. With no passive observations, every gesture
returns the next captured frame, reproducing the original nine-frame failure.
The controlled controller models settling by advancing through original tail
images at 250 ms intervals and holding frame 7 from 750 ms onward. These
passive timings are synthetic; no additional no-input footage was captured.
No JPEG is cropped, redrawn or recompressed.

The success case requires waiting before another gesture, a complete scan and
exactly the eight original movement instances. Negative cases cover persistent
rebound, disappearance of the inventory page, cancellation and failed input.
Existing whole-inventory interaction regressions additionally cover duplicate
items, partial columns, empty slots, unreadable/clipped names and OCR box jitter.

Run the regression with:

```powershell
./unit_test/MaaCore/run_blackflow_inventory_finish_replay.ps1 -CoreBuildDirectory build/core-shop-refresh
```

This is a controlled original-image replay, not a complete live emulator run.
