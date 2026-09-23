# Recovery regression fixtures

`shop-refresh-not-open.jpg` is the unmodified final frame from
`run-20260922-044826-100503/images/003781-task-finished-completed.jpg`.
The refresh control remains visible; the payment dialog never opened.
SHA-256: `5f3b8f52db8cde9129fece36b8c27965db00ee9c7f5ba75d2d6488d8de23327d`.

`diagnostics.json` contains synthetic schema-1 failure events for verifier and
consumer compatibility checks. These are not game observations or complete runs.
The native lifecycle replay separately exercises real event emission, bounded
recovery, screenshot policy and preservation of the outer floor.
