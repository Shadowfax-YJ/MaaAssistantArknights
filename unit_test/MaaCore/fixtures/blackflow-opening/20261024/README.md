# 002 opening selection failure

Original JPEG bytes from MAA-002 `run-20261024-125319-080435` (collector v1.1.28):

- `last-support.jpg`: `images/000029-start-reward-candidates-observed.jpg`, SHA-256 `a5476bd51657205237738609a9cb6c496936203521242ee487ed1c61ebf4eb19`.
- `wrong-vouchers.jpg`: `images/000035-task-finished-completed.jpg`, SHA-256 `802a08615abd58c58ae558146a655b4938f9ba11e6260605b363a744275c0b4b`.

The event stream has no RolesDefault/RolesConfirm between these frames. The
runtime log records a missing tank voucher and a disabled parent followed by
TaskChainCompleted. The old intermediate combo frame was not captured; these
images do not establish which historical click skipped that page.

Native tests run production OCR and the opening state machine. Injected combo
frames test timing, storage failures and input failures independently of the
captured image recognition. `opening-diagnostic-events.json` is exported from
the production lifecycle callback using this real wrong-voucher frame; its
envelope time/sequence are fixed for the shared contract test. It is diagnostic
evidence, not a recruitment or reward fact.
