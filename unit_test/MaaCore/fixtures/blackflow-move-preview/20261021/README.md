# M16 destination conversion confirmation

`m16-confirm.png` is the unmodified 1280×720 MAA-001 v1.1.23 failure frame,
`debug/interface/2026.10.21-13.27.11.492_raw.png`, from
`run-20261021-122404-391701`. It shows 坎诺特特快 changing 不期而遇 to 诡意行商.

`BlackFlow@Roguelike@MoveConversionConfirm.png` is its exact, unscaled
`[260,303,750,34]` text crop. Recognition uses the full conversion sentence;
the fixed click rectangle lies inside the right confirmation button.
The old cross-region confirmation retains its separate template and ROI.

Native tests replay the real task graph before acceptance, observation and
dialog reappearance. The title-masked counterexample is explicitly synthetic;
the generic button alone must not establish conversion dialog identity.
