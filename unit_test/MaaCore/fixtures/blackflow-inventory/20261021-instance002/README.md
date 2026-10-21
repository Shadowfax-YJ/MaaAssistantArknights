# MAA-002 explicit inventory tail

Source: v1.1.24, `run-20261021-212020-353628`, floor 1, action `BF-A1-13`.
`first-page.jpg` and `empty-tail.jpg` are unchanged evidence frames 1 and 9.
`runtime-ocr-layouts.json` contains the nine actual scan OCR results from
`debug/asst.log`, 2026-10-21 21:25:43.318–21:25:56.003 Asia/Shanghai.

The first scan page already contains two movement items and complete
`待收集零件` labels. Finish after collecting that page; no forward swipe is
needed. The later empty-only pages are evidence of the unnecessary scrolling.
Their OCR boxes alternate between heights 23 and 28 despite centers moving
only 0.5 pixels. Re-OCRing the JPEGs changes those boxes and masks the failure,
so the whole-scan replay also uses the original runtime OCR results.

The layout comparison regression separately keeps the actual 23/28-pixel
rectangle pair and its half-pixel centers. Names and centers must remain stable;
changing the OCR box size alone must not prevent either resetting to the start
or completing a scan without explicit empty slots. Actual movement, missing
names, and clipped titles remain negative cases.
