# Difficulty verification fixtures

- `home-5.jpg`: original `images/000020-run-ended-completed.jpg` from MAA-003
  `run-20260923-133827-975254.zip`. The configured target was 6; the home badge is 5.
  SHA-256: `75378aa5caf7b230afddc87e6dce649d0297b646786e244c2cf06d189be65398`.
- `home-6.jpg`: original `images/000506-run-ended-completed.jpg` from MAA-002
  `run-20260923-152519-314941.zip`. The home badge is 6.
  SHA-256: `c0f256026c757f4a1c0b511ca43bb5b2cf52bb43d12ae382caac5850955aad96`.
- `events.json`: **synthetic** evidence emitted by the production lifecycle method in
  `BlackFlowRecoveryReplay.cpp`. Covers target 6 with verified 6, observed 5 and unknown.
  Sequence/timestamp metadata is supplied by the contract test, not a real game.
  SHA-256: `ae3fa900fb8548e8116ae45cac762188b5d6c1523ced11b82ff87c02e3e437e4`.

The screenshots retain their original bytes. The native replay tests the production
home recognizer against both; its evidence output must match the shared fixture.
`BlackFlowDifficultyReplay.cpp.in` executes the production selection/verification
methods with injected UI outcomes. It covers missing confirms and transition frames,
but does not reconstruct the missing intermediate screenshots from the reported run.
