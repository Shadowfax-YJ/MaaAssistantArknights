# Codex Project Notes

This repository uses `AGENTS.md` as the Codex-facing project memory entry point. Keep durable investigation notes in `.agents/project-memory.md` and update them when a local pitfall is confirmed.

## MAA resource and WPF checks

- After editing `resource/tasks/**/*.json`, verify with a real Core resource load against `build/bin/Debug`; JSON parsing and Core compilation alone are not enough.
- For new roguelike modes, audit the complete `ProcessTask` chain from WPF mode submission through `StrategyChange`, `{theme}@Roguelike@Stages`, plugin trigger nodes, and terminal abandon/restart nodes. A task matched from a `next` list continues with that matched task's own `next`, not the sibling tasks that followed it in the parent list.
- For `src/MaaWpfGui` `net9.0-windows10.0.17763.0` builds, make sure the output directory does not contain assemblies that reference `System.Runtime, Version=10.0.0.0`.
- When validating the new roguelike collectible-pool test mode, check `build/bin/Debug/debug/asst.log` and confirm WPF submitted Roguelike mode `8`, not the existing deep exploration mode `7`.

See `.agents/project-memory.md` for concrete commands and known failure modes.
