# 002 Cargo from the Mouth decline ending

Original JPEG bytes from MAA-002 v1.1.29, `run-20261024-165636-848029`:

- `options.jpg`: `collection-popups/floor-4-tree-hole/node-1688849893818372/003541-node-event-capture-cc-try.jpg`, SHA-256 `da26ab6ff7f55d8ea7681f8ecf705856aa9ede399525ed2eaca57d3431829bf3`.
- `evidence-frame.jpg`: `images/003546-task-finished-completed.jpg`, SHA-256 `1afd765afffa67e2cffab1b6786b3f4f817f4a37ab294d5f7dec535938f07a12`.

At 17:44:04 (UTC+8), the first three offers were disabled and option four,
没什么想买的, was selected. The original confirm_cargo_choice omitted this
ending page and stopped with cargo_choice_transition_not_confirmed at 17:44:14.
The existing ending template matches the check; ordinary options and a masked
check do not match it. No threshold or resource action is changed.

`choice-payloads.json` is exported by the actual selection/confirmation methods
with injected input and recognizer results. It retains all four candidates and
the decline index/text; it does not assert a purchase or acquired reward.
The timing replay covers two consecutive ending frames, recognition gaps,
loading/data-sync overlays, storage failure and isolation from purchase choices.

`producer-events.json` is exported through the real Session, task port and run
logger with the original pre-click/ending pictures. Its floor-two Session is
controlled contract context, not the historical floor-four tree-hole scope.
The native test keeps that node running until the original ending dispatcher
handles exit. Map return is a separate older captured response frame; no missing
post-click image of this stopped run is fabricated. Shared consumer and signed
v1 archive checks use these same events and their phase-specific original images.
