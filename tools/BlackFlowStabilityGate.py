#!/usr/bin/env python3
"""Live stability gate for BlackFlow automation collection.

The gate tails the RelWithDebInfo logs, captures emulator screenshots at review
boundaries, tracks diagnostic run directories, and stops MAA through its global
link-start hotkey when a hard anomaly is observed.  A run is counted only after
its terminal state is followed by the next run's first diagnostic map; this
proves that long-running restart behaviour also worked.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import time
from collections import Counter, deque
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any


LOG_TIMESTAMP = re.compile(r"^\[(?P<time>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\]")
FLOOR_MOVE = re.compile(
    r"第\s*(?P<floor>\d+)\s*层｜行动力\s*(?P<before>\d+)→(?P<after>\d+)｜"
    r"(?P<movement>.+?)移动至\s*(?P<target>.+?)｜安全余量\s*(?P<safety>-?\d+)"
)
FLOOR_START = re.compile(r"BlackFlow current floor recognized floor (?P<floor>\d+) area (?P<area>.+)$")
SHOP_OFFER = re.compile(
    r"BlackFlow automation (?P<shop>诡意行商|秘境行商)商品 (?P<name>.+?) "
    r"(?:货架 (?P<shelf>上半|下半) )?价格 "
    r"(?P<price>\d+|未识别) 源石锭 (?P<wallet>\d+|未识别) 卡片可购买 (?P<buyable>是|否|未确认)"
)
SHOP_SELECTED = re.compile(
    r"BlackFlow automation (?P<shop>诡意行商|秘境行商) purchase selected (?P<name>.+?)"
    r"(?: 货架 (?P<shelf>上半|下半))?$"
)

EXPECTED_RESTART_OUTCOMES = {
    "automation_collection_pursuit_unsupported",
    "roguelike_settlement_defeat",
    "roguelike_settlement_success",
}
HARD_FAILURE_OUTCOMES = {
    "map_rebuild_failed",
    "page_recovery_failed",
    "wisadel_status_sync_failed",
    "floor_recognition_failed",
    "movement_inventory_refresh_failed",
    "movement_selection_failed",
}
SCREENSHOT_PATTERNS = {
    "task-start": ("TaskChainStart",),
    "floor": ("BlackFlow current floor recognized",),
    "battle": ("开始战斗:",),
    "shop-offer": ("BlackFlow automation 诡意行商商品", "BlackFlow automation 秘境行商商品"),
    "shop-selected": ("purchase selected",),
    "recruit": ("ChooseOperConfirm", "RecruitOther", "RecruitSkip"),
    "terminal": ('"what":"BlackFlowStrategyResult"',),
    "abandon": ("AbandonConfirm",),
    "error": ("TaskChainError", "terminated without a strategy result"),
}


def settlement_terminal(details: dict[str, Any]) -> dict[str, str] | None:
    """Translate the generic roguelike settlement callback into a run terminal.

    BlackFlow does not get another routing turn after a battle defeat, so that
    path cannot emit BlackFlowStrategyResult.  The settlement callback is the
    authoritative terminal evidence in that case.
    """
    game_pass = details.get("game_pass")
    if not isinstance(game_pass, bool):
        return None
    outcome = "roguelike_settlement_success" if game_pass else "roguelike_settlement_defeat"
    reason = "肉鸽结算：通关" if game_pass else "肉鸽结算：战败"
    return {"outcome": outcome, "reason": reason, "next_action": "restart_current_run"}


def now_stamp() -> str:
    return datetime.now().strftime("%Y%m%d-%H%M%S-%f")


def safe_name(value: str) -> str:
    return re.sub(r"[^0-9A-Za-z\u4e00-\u9fff_-]+", "-", value).strip("-")[:48] or "evidence"


def atomic_json(path: Path, value: Any) -> None:
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temp, path)


class TailFile:
    def __init__(self, path: Path, archive: Path) -> None:
        self.path = path
        self.archive = archive
        self.offset = path.stat().st_size if path.exists() else 0
        self.pending = ""

    def read_lines(self) -> list[str]:
        if not self.path.exists():
            return []
        size = self.path.stat().st_size
        if size < self.offset:
            self.offset = 0
            self.pending = ""
        if size == self.offset:
            return []
        with self.path.open("rb") as stream:
            stream.seek(self.offset)
            data = stream.read()
            self.offset = stream.tell()
        if not data:
            return []
        with self.archive.open("ab") as stream:
            stream.write(data)
        text = self.pending + data.decode("utf-8", errors="replace")
        parts = text.splitlines(keepends=True)
        if parts and not parts[-1].endswith(("\n", "\r")):
            self.pending = parts.pop()
        else:
            self.pending = ""
        return [part.rstrip("\r\n") for part in parts]


@dataclass
class RunRecord:
    directory: str
    discovered_at: str
    first_floor: int | None = None
    started_from_floor_one: bool = False
    terminal_at: str | None = None
    outcome: str | None = None
    reason: str | None = None
    next_action: str | None = None
    expected_terminal: bool = False
    restart_confirmed_at: str | None = None
    counted: bool = False
    actions: int = 0
    floors: dict[str, dict[str, Any]] = field(default_factory=dict)
    shop_selected: list[dict[str, Any]] = field(default_factory=list)
    revealed_nodes: dict[str, list[int]] = field(default_factory=dict)
    settlement: dict[str, Any] = field(default_factory=dict)
    metrics: dict[str, Any] = field(default_factory=dict)


class StabilityGate:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.bin_dir = args.bin_dir.resolve()
        self.debug_dir = self.bin_dir / "debug"
        self.blackflow_dir = self.debug_dir / "BlackFlow"
        self.session_dir = self.blackflow_dir / f"stability-{now_stamp()}"
        self.screens_dir = self.session_dir / "screenshots"
        self.session_dir.mkdir(parents=True, exist_ok=False)
        self.screens_dir.mkdir()
        self.events_path = self.session_dir / "events.jsonl"
        self.key_log_path = self.session_dir / "key-events.log"
        self.state_path = self.session_dir / "state.json"
        self.asst_tail = TailFile(self.debug_dir / "asst.log", self.session_dir / "asst.incremental.log")
        self.gui_tail = TailFile(self.debug_dir / "gui.log", self.session_dir / "gui.incremental.log")
        self.known_run_dirs = {
            path.name for path in self.blackflow_dir.glob("run-*") if path.is_dir()
        }
        self.runs: list[RunRecord] = []
        self.current_run: RunRecord | None = None
        self.consecutive = 0
        self.anomaly: dict[str, Any] | None = None
        self.started_at = datetime.now()
        self.last_log_activity = time.monotonic()
        self.last_progress = time.monotonic()
        self.last_state_write = 0.0
        self.last_screenshot: dict[str, float] = {}
        self.screenshot_sequence = 0
        self.recent_tasks: deque[tuple[float, str]] = deque()
        self.pending_terminal: RunRecord | None = None
        self.orphan_terminal: dict[str, Any] | None = None
        self.task_active = False
        self.maa_idle: bool | None = None
        self.maa_stopping = False
        self.stop_hotkey_sent = False
        self.shop_cycle: dict[str, Any] = {"shop": None, "offers": [], "wallet": None}
        self.current_floor: int | None = None
        self.metadata()
        if self.args.adopt_latest_run:
            candidates = sorted(
                (path for path in self.blackflow_dir.glob("run-*") if path.is_dir()),
                key=lambda path: path.stat().st_mtime_ns,
            )
            if candidates:
                self.known_run_dirs.discard(candidates[-1].name)
                self.task_active = True
                self.poll_run_directories()

    def metadata(self) -> None:
        config = self.bin_dir / "config" / "gui.new.json"
        if config.exists():
            shutil.copy2(config, self.session_dir / "gui.new.json")
        metadata = {
            "started_at": self.started_at.isoformat(),
            "target_runs": self.args.target_runs,
            "bin_dir": str(self.bin_dir),
            "asst_log_start_offset": self.asst_tail.offset,
            "gui_log_start_offset": self.gui_tail.offset,
            "baseline_run_directories": sorted(self.known_run_dirs),
            "adb": str(self.args.adb),
            "device": self.args.device,
            "stall_seconds": self.args.stall_seconds,
        }
        atomic_json(self.session_dir / "metadata.json", metadata)

    def emit(self, kind: str, **details: Any) -> None:
        event = {"time": datetime.now().isoformat(), "kind": kind, **details}
        with self.events_path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(event, ensure_ascii=False) + "\n")
        print(json.dumps(event, ensure_ascii=False), flush=True)

    def capture(self, reason: str, force: bool = False) -> None:
        now = time.monotonic()
        cooldown = 1.5 if reason.startswith("shop") else 4.0
        if not force and now - self.last_screenshot.get(reason, 0.0) < cooldown:
            return
        self.last_screenshot[reason] = now
        self.screenshot_sequence += 1
        name = f"{self.screenshot_sequence:04d}-{now_stamp()}-{safe_name(reason)}.png"
        local = self.screens_dir / name
        remote = f"/sdcard/{name}"
        try:
            subprocess.run(
                [str(self.args.adb), "-s", self.args.device, "shell", "screencap", "-p", remote],
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                timeout=15,
            )
            subprocess.run(
                [str(self.args.adb), "-s", self.args.device, "pull", remote, str(local)],
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                timeout=15,
            )
            self.emit("screenshot", reason=reason, file=str(local.relative_to(self.session_dir)))
        except Exception as exc:  # evidence failure is itself important, but should not stop gameplay
            self.emit("screenshot-error", reason=reason, error=str(exc))

    def stop_maa(self) -> None:
        if self.stop_hotkey_sent:
            self.emit("stop-hotkey-skipped", reason="already-sent")
            return
        if self.maa_stopping or self.maa_idle is True or not self.task_active:
            self.emit(
                "stop-hotkey-skipped",
                reason="already-idle-or-stopping",
                maa_idle=self.maa_idle,
                maa_stopping=self.maa_stopping,
                task_active=self.task_active,
            )
            return
        # MAA registers Ctrl+Shift+Alt+L as a global LinkStart toggle.
        user32 = ctypes.windll.user32
        key_up = 0x0002
        keys = [0x11, 0x10, 0x12, 0x4C]  # CTRL, SHIFT, ALT, L
        for key in keys:
            user32.keybd_event(key, 0, 0, 0)
        for key in reversed(keys):
            user32.keybd_event(key, 0, key_up, 0)
        self.stop_hotkey_sent = True
        self.emit("stop-hotkey-sent")

    def fail(self, code: str, reason: str, line: str | None = None) -> None:
        if self.anomaly is not None:
            return
        self.anomaly = {"code": code, "reason": reason, "line": line, "time": datetime.now().isoformat()}
        self.emit("anomaly", **self.anomaly)
        self.capture(f"anomaly-{code}", force=True)
        if not self.args.no_stop:
            self.stop_maa()

    def inspect_run_floor(self, directory: Path) -> int | None:
        floor_dirs = sorted(directory.glob("floor-*"))
        for floor_dir in floor_dirs:
            match = re.fullmatch(r"floor-(\d+)", floor_dir.name)
            if match and any(floor_dir.glob("*.json")):
                return int(match.group(1))
        return None

    def poll_run_directories(self) -> None:
        directories = sorted(
            (path for path in self.blackflow_dir.glob("run-*") if path.is_dir()),
            key=lambda path: path.stat().st_mtime_ns,
        )
        for directory in directories:
            if directory.name in self.known_run_dirs:
                continue
            first_floor = self.inspect_run_floor(directory)
            if first_floor is None:
                continue
            self.known_run_dirs.add(directory.name)
            record = RunRecord(
                directory=str(directory),
                discovered_at=datetime.now().isoformat(),
                first_floor=first_floor,
                started_from_floor_one=first_floor == 1,
            )
            record.floors[str(first_floor)] = {
                "entered_at": record.discovered_at,
                "area": None,
                "first_action_at": None,
                "last_action_at": None,
                "actions": 0,
                "targets": [],
            }
            previous = self.current_run
            self.runs.append(record)
            self.current_run = record
            self.current_floor = first_floor
            self.emit("run-diagnostic-start", directory=directory.name, first_floor=first_floor)
            self.capture(f"run-start-floor-{first_floor}", force=True)
            self.last_progress = time.monotonic()

            if self.orphan_terminal is not None:
                orphan = self.orphan_terminal
                self.orphan_terminal = None
                self.apply_strategy_result(record, orphan)

            if self.pending_terminal is not None and self.pending_terminal is previous:
                previous.restart_confirmed_at = datetime.now().isoformat()
                if previous.started_from_floor_one and previous.expected_terminal:
                    previous.counted = True
                    self.consecutive += 1
                    self.emit(
                        "run-counted",
                        consecutive=self.consecutive,
                        directory=Path(previous.directory).name,
                        outcome=previous.outcome,
                    )
                self.pending_terminal = None
            elif previous is not None and previous.started_from_floor_one and previous.terminal_at is None:
                self.fail(
                    "unreported-run-restart",
                    f"new diagnostic run {directory.name} appeared before the previous run reported a terminal state",
                )

    def apply_strategy_result(self, record: RunRecord, result: dict[str, Any]) -> None:
        outcome = str(result.get("outcome", ""))
        reason = str(result.get("reason", ""))
        next_action = str(result.get("next_action", ""))
        line = str(result.get("line", ""))
        expected = outcome in EXPECTED_RESTART_OUTCOMES and next_action == "restart_current_run"
        record.terminal_at = datetime.now().isoformat()
        record.outcome = outcome
        record.reason = reason
        record.next_action = next_action
        record.expected_terminal = expected
        self.finalize_run_metrics(record)
        self.pending_terminal = record if expected else None
        self.emit(
            "strategy-result",
            directory=Path(record.directory).name,
            outcome=outcome,
            reason=reason,
            next_action=next_action,
            expected=expected,
        )
        self.last_progress = time.monotonic()
        if outcome in HARD_FAILURE_OUTCOMES or not expected:
            self.fail("unexpected-strategy-result", f"{outcome}: {reason}; next={next_action}", line)

    def finalize_run_metrics(self, record: RunRecord) -> None:
        terminal = datetime.fromisoformat(record.terminal_at) if record.terminal_at else datetime.now()
        started = datetime.fromisoformat(record.discovered_at)
        duration_seconds = max(0.0, (terminal - started).total_seconds())
        floor_durations: dict[str, float] = {}
        ordered = sorted(
            (
                (floor, stats.get("entered_at") or stats.get("first_action_at"))
                for floor, stats in record.floors.items()
                if stats.get("entered_at") or stats.get("first_action_at")
            ),
            key=lambda item: item[1],
        )
        for index, (floor, entered_at) in enumerate(ordered):
            left_at = ordered[index + 1][1] if index + 1 < len(ordered) else record.terminal_at
            if left_at:
                floor_durations[floor] = max(
                    0.0,
                    (datetime.fromisoformat(left_at) - datetime.fromisoformat(entered_at)).total_seconds(),
                )
        revealed_count = sum(len(nodes) for nodes in record.revealed_nodes.values())
        record.metrics = {
            "duration_seconds": round(duration_seconds, 3),
            "revealed_node_count": revealed_count,
            "revealed_nodes_per_minute": round(revealed_count * 60 / duration_seconds, 3)
            if duration_seconds > 0
            else 0.0,
            "floor_duration_seconds": {floor: round(value, 3) for floor, value in floor_durations.items()},
            "actions": record.actions,
        }
        self.emit("run-metrics", directory=Path(record.directory).name, **record.metrics)

    def process_strategy_result(self, payload: dict[str, Any], line: str) -> None:
        details = payload.get("details", {})
        outcome = str(details.get("outcome", ""))
        reason = str(details.get("termination_reason", ""))
        next_action = str(details.get("next_action", ""))
        record = self.current_run
        if record is None:
            self.emit("terminal-without-diagnostic-run", outcome=outcome, reason=reason)
            self.orphan_terminal = {
                "outcome": outcome,
                "reason": reason,
                "next_action": next_action,
                "line": line,
            }
            return
        self.apply_strategy_result(
            record,
            {"outcome": outcome, "reason": reason, "next_action": next_action, "line": line},
        )

    def process_settlement(self, payload: dict[str, Any], line: str) -> None:
        details = payload.get("details", {})
        if not isinstance(details, dict):
            return
        terminal = settlement_terminal(details)
        if terminal is None:
            return
        record = self.current_run
        self.emit(
            "roguelike-settlement",
            game_pass=details.get("game_pass"),
            floor=details.get("floor"),
            step=details.get("step"),
            score=details.get("score"),
        )
        if record is None:
            self.orphan_terminal = {**terminal, "line": line}
            return
        record.settlement = dict(details)
        if record.terminal_at is not None:
            self.emit(
                "settlement-after-terminal",
                directory=Path(record.directory).name,
                outcome=record.outcome,
            )
            return
        self.apply_strategy_result(record, {**terminal, "line": line})

    def record_move(self, match: re.Match[str], line_time: str | None) -> None:
        record = self.current_run
        if record is None:
            return
        floor = match.group("floor")
        stats = record.floors.setdefault(
            floor,
            {"first_action_at": line_time, "last_action_at": line_time, "actions": 0, "targets": []},
        )
        stats["last_action_at"] = line_time
        stats["actions"] += 1
        stats["targets"].append(match.group("target"))
        record.actions += 1
        self.last_progress = time.monotonic()
        self.emit(
            "move",
            floor=int(floor),
            movement=match.group("movement"),
            target=match.group("target"),
            action_points_before=int(match.group("before")),
            action_points_after=int(match.group("after")),
            safety_margin=int(match.group("safety")),
        )

    def record_floor_start(self, match: re.Match[str], line_time: str | None) -> None:
        record = self.current_run
        if record is None or line_time is None:
            return
        floor = int(match.group("floor"))
        if self.current_floor == floor and str(floor) in record.floors:
            return
        self.current_floor = floor
        stats = record.floors.setdefault(
            str(floor),
            {"first_action_at": None, "last_action_at": None, "actions": 0, "targets": []},
        )
        stats.setdefault("entered_at", datetime.strptime(line_time, "%Y-%m-%d %H:%M:%S.%f").isoformat())
        stats["area"] = match.group("area")
        self.emit("floor-start", floor=floor, area=match.group("area"))

    def record_routing_reveal(self, payload: dict[str, Any]) -> None:
        record = self.current_run
        if record is None:
            return
        consistency = payload.get("details", {}).get("previous_move_reveal_consistency", {})
        floor = consistency.get("floor")
        nodes = consistency.get("observed_revealed_nodes", [])
        if not isinstance(floor, int) or not isinstance(nodes, list):
            return
        recorded = record.revealed_nodes.setdefault(str(floor), [])
        added = []
        for node in nodes:
            if isinstance(node, int) and node not in recorded:
                recorded.append(node)
                added.append(node)
        if added:
            self.emit("revealed-nodes", floor=floor, count=len(added), nodes=added)

    def record_shop_offer(self, match: re.Match[str]) -> None:
        shop = match.group("shop")
        if self.shop_cycle["shop"] != shop:
            self.shop_cycle = {"shop": shop, "offers": [], "wallet": None}
        price = None if match.group("price") == "未识别" else int(match.group("price"))
        wallet = None if match.group("wallet") == "未识别" else int(match.group("wallet"))
        self.shop_cycle["wallet"] = wallet
        self.shop_cycle["offers"].append(
            {
                "name": match.group("name"),
                "shelf": match.group("shelf"),
                "price": price,
                "buyable": match.group("buyable"),
            }
        )

    def process_task_loop(self, task: str, line: str) -> None:
        now = time.monotonic()
        self.recent_tasks.append((now, task))
        while self.recent_tasks and now - self.recent_tasks[0][0] > 120:
            self.recent_tasks.popleft()
        excluded = {
            "BattleCancelSelection",
            "MapCaptureStabilityWait",
            "Routing",
            "RoutingAction",
            "MapPrepare",
            "MapPrepare-Ready",
            "Stages",
        }
        if task in excluded:
            return
        count = sum(1 for _, recent in self.recent_tasks if recent == task)
        if count >= 30:
            self.fail("repeated-task-loop", f"task {task} started {count} times within 120 seconds", line)

    def process_line(self, source: str, line: str) -> None:
        if not line:
            return
        self.last_log_activity = time.monotonic()
        time_match = LOG_TIMESTAMP.match(line)
        line_time = time_match.group("time") if time_match else None

        interesting = any(
            token in line
            for token in (
                "BlackFlow",
                "RoguelikeSettlement",
                "TaskChain",
                "任务出错",
                "任务已停止",
                "开始战斗:",
                "战斗完成",
                "第 ",
            )
        )
        if interesting:
            with self.key_log_path.open("a", encoding="utf-8") as stream:
                stream.write(f"[{source}] {line}\n")

        move_match = FLOOR_MOVE.search(line)
        if move_match:
            self.record_move(move_match, line_time)

        floor_match = FLOOR_START.search(line)
        if floor_match:
            self.record_floor_start(floor_match, line_time)

        offer_match = SHOP_OFFER.search(line)
        if offer_match:
            self.record_shop_offer(offer_match)

        selected_match = SHOP_SELECTED.search(line)
        if selected_match:
            selected = {
                "shop": selected_match.group("shop"),
                "name": selected_match.group("name"),
                "shelf": selected_match.group("shelf"),
                "time": line_time,
            }
            if self.current_run is not None:
                self.current_run.shop_selected.append(selected)
            self.emit("shop-purchase-selected", **selected)
            self.last_progress = time.monotonic()

        if "Assistant::append_callback | SubTaskStart " in line:
            json_start = line.find("{", line.find("SubTaskStart"))
            if json_start >= 0:
                try:
                    payload = json.loads(line[json_start:])
                    task = str(payload.get("details", {}).get("task", ""))
                    if task:
                        self.process_task_loop(task, line)
                except json.JSONDecodeError:
                    pass

        if '"what":"BlackFlowStrategyResult"' in line:
            json_start = line.find("{", line.find("SubTaskExtraInfo"))
            if json_start >= 0:
                try:
                    self.process_strategy_result(json.loads(line[json_start:]), line)
                except json.JSONDecodeError as exc:
                    self.fail("strategy-result-json", str(exc), line)

        if '"what":"RoguelikeSettlement"' in line:
            json_start = line.find("{", line.find("SubTaskExtraInfo"))
            if json_start >= 0:
                try:
                    self.process_settlement(json.loads(line[json_start:]), line)
                except json.JSONDecodeError as exc:
                    self.fail("settlement-json", str(exc), line)

        if '"what":"BlackFlowRoutingDecision"' in line:
            json_start = line.find("{", line.find("SubTaskExtraInfo"))
            if json_start >= 0:
                try:
                    self.record_routing_reveal(json.loads(line[json_start:]))
                except json.JSONDecodeError as exc:
                    self.fail("routing-decision-json", str(exc), line)

        if "TaskChainStart" in line and '"taskchain":"Roguelike"' in line:
            self.task_active = True
            self.stop_hotkey_sent = False
            self.emit("task-chain-start")
            self.last_progress = time.monotonic()
        if "TaskChainCompleted" in line and '"taskchain":"Roguelike"' in line:
            self.task_active = False
            if self.consecutive < self.args.target_runs:
                self.fail("task-chain-ended-early", "Roguelike task chain completed before the stability target", line)
        if "TaskChainError" in line and '"taskchain":"Roguelike"' in line:
            # Error is terminal for this task chain.  Do not press the global LinkStart toggle:
            # by the time the hotkey is delivered the GUI may already be idle, which would start
            # a new run instead of stopping the failed one.
            self.task_active = False
            self.fail("task-chain-error", "Roguelike task chain reported an error", line)
        if "terminated without a strategy result" in line:
            self.fail("unreported-termination", "BlackFlow terminated without a strategy result", line)
        if source == "gui" and ("任务出错" in line or "任务失败" in line):
            # The GUI emits this immediately before switching Idle false -> true.  Treat it as
            # already terminal so stop_maa() cannot turn an error into an accidental restart.
            self.maa_idle = True
            self.maa_stopping = False
            self.task_active = False
            self.fail("gui-task-error", "GUI reported task failure", line)

        if source == "gui":
            if "Idle: true to false" in line:
                self.maa_idle = False
                self.maa_stopping = False
                self.stop_hotkey_sent = False
            elif "Idle: false to true" in line:
                self.maa_idle = True
                self.maa_stopping = False
                self.task_active = False
            if "Stopping: false to true" in line:
                self.maa_stopping = True
            elif "Stopping: true to false" in line:
                self.maa_stopping = False

        for reason, patterns in SCREENSHOT_PATTERNS.items():
            if any(pattern in line for pattern in patterns):
                self.capture(reason)
                break

    def write_state(self) -> None:
        state = {
            "started_at": self.started_at.isoformat(),
            "updated_at": datetime.now().isoformat(),
            "target_runs": self.args.target_runs,
            "consecutive_runs": self.consecutive,
            "task_active": self.task_active,
            "maa_idle": self.maa_idle,
            "maa_stopping": self.maa_stopping,
            "anomaly": self.anomaly,
            "current_run": None if self.current_run is None else Path(self.current_run.directory).name,
            "runs": [record.__dict__ for record in self.runs],
            "session_dir": str(self.session_dir),
        }
        atomic_json(self.state_path, state)

    def run(self) -> int:
        self.emit("monitor-start", session_dir=str(self.session_dir), target_runs=self.args.target_runs)
        self.capture("monitor-start", force=True)
        while True:
            self.poll_run_directories()
            for line in self.gui_tail.read_lines():
                self.process_line("gui", line)
            for line in self.asst_tail.read_lines():
                self.process_line("asst", line)
            self.poll_run_directories()

            now = time.monotonic()
            if self.task_active and now - self.last_log_activity > self.args.stall_seconds:
                self.fail(
                    "log-stall",
                    f"no new MAA log lines for {self.args.stall_seconds} seconds while the task was active",
                )
            if self.pending_terminal is not None and now - self.last_progress > self.args.restart_seconds:
                self.fail(
                    "restart-timeout",
                    f"no next diagnostic run appeared within {self.args.restart_seconds} seconds after terminal state",
                )

            if now - self.last_state_write >= 2:
                self.write_state()
                self.last_state_write = now

            if self.anomaly is not None:
                self.write_state()
                self.emit("monitor-stop", status="anomaly", consecutive=self.consecutive)
                return 2
            if self.consecutive >= self.args.target_runs:
                self.capture("stability-target-met", force=True)
                self.write_state()
                self.emit("monitor-stop", status="passed", consecutive=self.consecutive)
                return 0
            if (self.session_dir / "stop.request").exists():
                self.write_state()
                self.emit("monitor-stop", status="requested", consecutive=self.consecutive)
                return 3
            time.sleep(self.args.poll_ms / 1000)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--bin-dir",
        type=Path,
        default=Path("build/bin/RelWithDebInfo"),
        help="MAA RelWithDebInfo output directory",
    )
    parser.add_argument("--target-runs", type=int, default=10)
    parser.add_argument("--adb", type=Path, default=Path(r"E:\Programs\MuMuPlayer\nx_main\adb.exe"))
    parser.add_argument("--device", default="emulator-5558")
    parser.add_argument("--poll-ms", type=int, default=250)
    parser.add_argument("--stall-seconds", type=int, default=420)
    parser.add_argument("--restart-seconds", type=int, default=180)
    parser.add_argument(
        "--adopt-latest-run",
        action="store_true",
        help="treat the newest existing diagnostic run as the current run (for monitor restarts)",
    )
    parser.add_argument("--no-stop", action="store_true", help="do not send the MAA stop hotkey on anomaly")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if sys.platform != "win32":
        print("BlackFlowStabilityGate requires Windows for MAA hotkey control", file=sys.stderr)
        return 64
    if not args.adb.exists():
        print(f"adb does not exist: {args.adb}", file=sys.stderr)
        return 66
    return StabilityGate(args).run()


if __name__ == "__main__":
    raise SystemExit(main())
