# Shop identity after a tree-hole return

`return-refresh-wallet-34.jpg` is the unmodified 1280 × 720 JPEG from MAA-002,
v1.1.18, `run-20261002-193526-530219/images/005731-recovery-store-refresh-failed.jpg`.
SHA-256: `72935e76c20185c1dac8e8795e10e9b43e84d49146e84b7e5fb69ef11a44f083`.

The outer-floor-five shop at node `1407374950662152` was captured with refresh
indices 0, 1 and 2 at 20:23:42, 20:24:27 and 20:24:51 (UTC+8). Its perception
generation was 8. After a tree-hole round trip, the same node was captured with
index 0 at 20:32:46, now in generation 10. The shelf already showed the next
refresh price as 12. At 20:33:09 the receipt checker recorded wallet 46 → 34,
but expected 42 because it treated the refresh as the first payment of 4.

`BlackFlowStoreIdentityReplay.h` uses controlled two-node map observations and
seeds the earlier verified refresh counts. These are synthetic inputs, not
full map recognition or a replay of the earlier payment clicks. It executes
the actual Session move/portal transactions, tree-hole entry/return, and both
merchant entry callbacks. It checks retained counts, screenshot refresh index,
next-price expectations, and the two-refresh limit. Repeated tree holes, fresh
child maps, four-floor remembrance maps and new runs check identity isolation.
The JPEG independently exercises production wallet/page recognition.

On the old production objects, the full native recovery replay reported
52 passed and 4 failed; with the fix, all 56 checks pass. Existing receipt polling
remains covered separately by `run_blackflow_store_refresh_replay.ps1`; this fix
does not relax debit, confirmation-page, loading or deadline checks. The 21
receipt checks, 352 existing C++ regressions and 2 archive/consumer contract
checks also pass, and the Core builds successfully. No complete live-game run with
the fix has been recorded.
