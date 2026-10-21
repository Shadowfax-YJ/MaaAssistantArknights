# Opening cultivation harvest

`ingots-5.png` is the unmodified 1280×720 failure screenshot from MAA-002 v1.1.23,
`run-20261021-143149-584495`, captured at 2026-10-21 14:32:14.183 local time.
It shows 收获培育成果 / 源石锭 +5 after squad confirmation.

The title template is an exact unscaled crop `[565,20,190,32]` of this image.
The existing generic checkmark is used only after the harvest title has matched.
`ingots-5.jpg` is a JPEG quality 76 encoding of the original PNG, exported by the
native replay to exercise the existing archive image format.
The title-masked counterexample is synthetic and is not an additional game capture.

Native replay uses production title/button matching, OCR, persistence policy and
event construction. The shared `opening-cultivation-events.json` is generated
from these functions with injected successful successor observations. It is a
contract sample, not evidence that the original failed run received the reward.
