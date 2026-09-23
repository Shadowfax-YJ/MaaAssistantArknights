# Recovery regression fixtures

`shop-refresh-network-pending.jpg` is the unmodified failure frame from MAA-002,
`run-20260923-222414-092617/images/002306-recovery-store-refresh-failed.jpg`.
The shop and wallet (31) remain visible while the game displays its network
submission overlay. The first refresh costs 4; this frame is not a receipt.
SHA-256: `6854beb511f33eaafd0b536c5cb8e827e77a80343803b988d3c8af681c81c92b`.
The native replay checks the real `LoadingText` OCR, shop matcher and wallet OCR;
the ordinary shop fixture below is also a negative loading-text example.
The store replay injects vision/controller/clock I/O into the production callback
and polling method. Its delayed completion frames are synthetic scenarios, not
proof that the captured request eventually succeeded. It covers long/intermittent
submission, stable debit after submission, the absolute 30-second deadline and
cancellation; the v1.1.13 implementation fails six of these cases.

`shop-refresh-not-open.jpg` is the unmodified final frame from
`run-20260922-044826-100503/images/003781-task-finished-completed.jpg`.
The refresh control remains visible; the payment dialog never opened.
SHA-256: `5f3b8f52db8cde9129fece36b8c27965db00ee9c7f5ba75d2d6488d8de23327d`.

`diagnostics.json` contains synthetic schema-1 failure events for verifier and
consumer compatibility checks. These are not game observations or complete runs.
The native lifecycle replay separately exercises real event emission, bounded
recovery, screenshot policy and preservation of the outer floor.
