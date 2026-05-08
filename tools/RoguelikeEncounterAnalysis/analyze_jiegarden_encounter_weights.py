#!/usr/bin/env python3
"""Estimate JieGarden encounter weights from collected metadata.

The script implements the workflow described in the prompt:

1. Load ordered encounter nodes from the metadata workbook.
2. Build the per-node candidate pool from layer membership and per-run caps.
3. Emit hard-rule violations before fitting any weights.
4. Fit M0 (equal weights), M1 (global event weights), and M2
   (event-by-layer weights).
5. Emit residuals, repeat-count checks, optional run-level bootstrap CIs, and
   train/test likelihood comparisons.

The default layer configuration is copied from the supplied screenshot for
layers 1 and 2 only. Adjust DEFAULT_EVENT_LAYERS_1_2 or pass
--use-observed-pool for a data-only sensitivity check.
"""

from __future__ import annotations

import argparse
import math
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
import pandas as pd


DEFAULT_INPUT = Path(r"D:\ShadowfaxData\Downloads\encounters整理_完整metadata.xlsx")

# The workbook groups observed rows by named floor. For this data set these are
# the first two layers.
DEFAULT_GROUP_TO_LAYER = {
    "洪陆楼": 1,
    "山水阁": 2,
}

# Screenshot-derived event pool, restricted to layers 1 and 2.
# Events not observed in the workbook are intentionally kept; they become
# boundary/zero-weight diagnostics if the configured pool is too broad.
DEFAULT_EVENT_LAYERS_1_2 = {
    "传讯": [1, 2],
    "来者不拒": [1, 2],
    "饕餮廊": [1, 2],
    "石山": [1, 2],
    "偏安": [1, 2],
    "岔路": [1, 2],
    "集印": [1, 2],
    "护鸭金刚": [1, 2],
    "火祀": [2],
    "烟火漫天": [2],
    "柳儿": [2],
    "鲍老板连锁": [2],
    "移时换物": [2],
    "见钱问柳": [2],
    "鼓上佩洛": [2],
}

# In the supplied data, 偏安 is the only event observed twice in the same run.
DEFAULT_REPEAT_EVENTS = {"偏安"}


@dataclass
class FitResult:
    model: str
    items: list[Any]
    theta: np.ndarray
    nll: float
    success: bool
    iterations: int
    max_projected_grad: float
    params: int
    component_by_item: dict[Any, int]
    boundary_low: set[Any]
    boundary_high: set[Any]
    theta_bound: float

    @property
    def log_likelihood(self) -> float:
        return -self.nll


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Estimate JieGarden encounter event weights from an xlsx workbook.",
    )
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("build/encounter_weight_analysis"),
    )
    parser.add_argument(
        "--layers",
        default="1,2",
        help="Comma-separated numeric layers to analyze. Default: 1,2.",
    )
    parser.add_argument(
        "--run-col",
        default="jsonl_record_id",
        help="Column identifying one run/record. Default: jsonl_record_id.",
    )
    parser.add_argument(
        "--use-observed-pool",
        action="store_true",
        help="Build each layer pool from observed rows instead of the screenshot config.",
    )
    parser.add_argument(
        "--drop-unobserved-config-events",
        action="store_true",
        help="When using the screenshot config, drop configured events never observed.",
    )
    parser.add_argument(
        "--repeat-event",
        action="append",
        default=[],
        help="Additional event allowed to repeat within one run. May be passed multiple times.",
    )
    parser.add_argument(
        "--no-default-repeat-events",
        action="store_true",
        help="Do not treat the built-in DEFAULT_REPEAT_EVENTS as repeatable.",
    )
    parser.add_argument("--test-frac", type=float, default=0.2)
    parser.add_argument("--bootstrap", type=int, default=0)
    parser.add_argument("--seed", type=int, default=20260509)
    parser.add_argument("--max-iter", type=int, default=300)
    parser.add_argument("--theta-bound", type=float, default=20.0)
    parser.add_argument("--tol", type=float, default=1e-7)
    return parser.parse_args()


def parse_layers(text: str) -> set[int]:
    layers = {int(part.strip()) for part in text.split(",") if part.strip()}
    if not layers:
        raise ValueError("--layers must contain at least one layer")
    return layers


def parse_image_timestamp(series: pd.Series) -> pd.Series:
    parsed = pd.to_datetime(series, format="%Y.%m.%d-%H.%M.%S.%f", errors="coerce")
    if parsed.isna().any():
        fallback = pd.to_datetime(series, errors="coerce")
        parsed = parsed.fillna(fallback)
    return parsed


def load_nodes(
    input_path: Path,
    run_col: str,
    group_to_layer: dict[str, int],
    layers: set[int],
) -> tuple[pd.DataFrame, list[dict[str, Any]]]:
    raw = pd.read_excel(input_path, sheet_name="Encounters")
    if run_col not in raw.columns:
        raise KeyError(f"run column {run_col!r} not found in Encounters sheet")

    if "type" in raw.columns:
        raw = raw[raw["type"].fillna("") == "Encounter"].copy()

    violations: list[dict[str, Any]] = []
    unknown_group = raw[~raw["group_name"].isin(group_to_layer)].copy()
    for row in unknown_group.to_dict("records"):
        violations.append(
            {
                "type": "unknown_layer_group",
                "run_id": row.get(run_col),
                "group_name": row.get("group_name"),
                "event": row.get("name"),
                "detail": "group_name is not mapped to a numeric layer",
            }
        )

    df = raw[raw["group_name"].isin(group_to_layer)].copy()
    df["layer"] = df["group_name"].map(group_to_layer).astype(int)
    df = df[df["layer"].isin(layers)].copy()
    df["run_id"] = df[run_col].astype(str)
    df["event"] = df["name"].astype(str).str.strip()
    df["_sort_ts"] = parse_image_timestamp(df["image_timestamp"])
    df["_sort_ts_missing"] = df["_sort_ts"].isna()

    sort_cols = ["run_id", "_sort_ts_missing", "_sort_ts"]
    for col in ("item_index_in_line", "encounter_row_id"):
        if col in df.columns:
            sort_cols.append(col)
    df = df.sort_values(sort_cols, kind="mergesort").reset_index(drop=True)
    df["step"] = df.groupby("run_id").cumcount() + 1
    return df, violations


def build_event_layers_from_observed(df: pd.DataFrame) -> dict[str, list[int]]:
    event_layers: dict[str, list[int]] = {}
    for event, sub in df.groupby("event"):
        event_layers[event] = sorted(int(layer) for layer in sub["layer"].unique())
    return event_layers


def build_layer_pool(
    event_layers: dict[str, list[int]],
    layers: set[int],
    observed_events: set[str],
    drop_unobserved: bool,
) -> dict[int, list[str]]:
    layer_pool: dict[int, list[str]] = {layer: [] for layer in sorted(layers)}
    for event, allowed_layers in event_layers.items():
        if drop_unobserved and event not in observed_events:
            continue
        for layer in allowed_layers:
            if layer in layer_pool:
                layer_pool[layer].append(event)
    for layer in layer_pool:
        layer_pool[layer] = sorted(set(layer_pool[layer]))
    return layer_pool


def build_caps(layer_pool: dict[int, list[str]], repeat_events: set[str]) -> dict[str, float]:
    events = {event for pool in layer_pool.values() for event in pool}
    return {event: (math.inf if event in repeat_events else 1.0) for event in events}


def build_trials(
    nodes: pd.DataFrame,
    layer_pool: dict[int, list[str]],
    caps: dict[str, float],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    trials: list[dict[str, Any]] = []
    violations: list[dict[str, Any]] = []
    current_run: str | None = None
    appeared: defaultdict[str, int] = defaultdict(int)

    for row in nodes.sort_values(["run_id", "step"], kind="mergesort").to_dict("records"):
        run_id = str(row["run_id"])
        if run_id != current_run:
            current_run = run_id
            appeared = defaultdict(int)

        layer = int(row["layer"])
        observed = str(row["event"]).strip()
        pool = layer_pool.get(layer, [])
        candidate_set = [
            event for event in pool if appeared[event] < caps.get(event, math.inf)
        ]

        if not candidate_set:
            violations.append(
                make_violation(
                    "empty_candidate_set",
                    row,
                    candidate_set,
                    "candidate pool is empty before observing the row",
                )
            )
        if observed not in pool:
            violations.append(
                make_violation(
                    "observed_in_wrong_layer",
                    row,
                    candidate_set,
                    "observed event is not configured for this layer",
                )
            )
        if appeared[observed] >= caps.get(observed, math.inf):
            violations.append(
                make_violation(
                    "cap_exceeded_before_observation",
                    row,
                    candidate_set,
                    "observed event already reached its per-run cap",
                )
            )
        if observed not in candidate_set:
            violations.append(
                make_violation(
                    "observed_not_in_candidate_set",
                    row,
                    candidate_set,
                    "observed event is absent from the reconstructed candidate set",
                )
            )

        trials.append(
            {
                "run_id": run_id,
                "step": int(row["step"]),
                "layer": layer,
                "group_name": row.get("group_name"),
                "observed": observed,
                "candidate_set": candidate_set,
                "candidate_size": len(candidate_set),
                "is_valid": bool(candidate_set) and observed in candidate_set,
                "image_timestamp": row.get("image_timestamp"),
                "encounter_row_id": row.get("encounter_row_id"),
            }
        )
        appeared[observed] += 1

    return trials, violations


def make_violation(
    violation_type: str,
    row: dict[str, Any],
    candidate_set: list[str],
    detail: str,
) -> dict[str, Any]:
    return {
        "type": violation_type,
        "run_id": row.get("run_id"),
        "step": row.get("step"),
        "layer": row.get("layer"),
        "group_name": row.get("group_name"),
        "event": row.get("event"),
        "candidate_set": "|".join(candidate_set),
        "detail": detail,
    }


def item_sort_key(item: Any) -> tuple[Any, ...]:
    if isinstance(item, tuple):
        event, layer = item
        return (int(layer), str(event))
    return (str(item),)


def make_choice_records(
    trials: list[dict[str, Any]],
    mode: str,
) -> list[tuple[Any, list[Any]]]:
    records: list[tuple[Any, list[Any]]] = []
    for trial in trials:
        if not trial["is_valid"]:
            continue
        if mode == "m1":
            y = trial["observed"]
            candidates = list(trial["candidate_set"])
        elif mode == "m2":
            layer = int(trial["layer"])
            y = (trial["observed"], layer)
            candidates = [(event, layer) for event in trial["candidate_set"]]
        else:
            raise ValueError(f"unknown mode: {mode}")
        records.append((y, candidates))
    return records


class DisjointSet:
    def __init__(self, items: list[Any]) -> None:
        self.parent = {item: item for item in items}

    def find(self, item: Any) -> Any:
        parent = self.parent[item]
        if parent != item:
            self.parent[item] = self.find(parent)
        return self.parent[item]

    def union(self, left: Any, right: Any) -> None:
        root_left = self.find(left)
        root_right = self.find(right)
        if root_left != root_right:
            self.parent[root_right] = root_left


def find_components(
    items: list[Any],
    records: list[tuple[Any, list[Any]]],
) -> dict[Any, int]:
    dsu = DisjointSet(items)
    for _, candidates in records:
        if len(candidates) < 2:
            continue
        first = candidates[0]
        for item in candidates[1:]:
            dsu.union(first, item)

    roots = sorted({dsu.find(item) for item in items}, key=item_sort_key)
    root_to_component = {root: idx for idx, root in enumerate(roots, start=1)}
    return {item: root_to_component[dsu.find(item)] for item in items}


def logsumexp(values: np.ndarray) -> float:
    max_value = float(np.max(values))
    return max_value + math.log(float(np.sum(np.exp(values - max_value))))


def softmax(values: np.ndarray) -> np.ndarray:
    max_value = float(np.max(values))
    exp_values = np.exp(values - max_value)
    return exp_values / np.sum(exp_values)


def fit_choice_model(
    records: list[tuple[Any, list[Any]]],
    model: str,
    max_iter: int,
    theta_bound: float,
    tol: float,
) -> FitResult:
    items = sorted({item for _, candidates in records for item in candidates}, key=item_sort_key)
    if not items:
        return FitResult(model, [], np.array([]), math.nan, False, 0, math.nan, 0, {}, set(), set(), theta_bound)

    item_to_idx = {item: idx for idx, item in enumerate(items)}
    indexed_records = [
        (item_to_idx[y], np.array([item_to_idx[item] for item in candidates], dtype=np.int64))
        for y, candidates in records
    ]

    component_by_item = find_components(items, records)
    anchors = set()
    for component in sorted(set(component_by_item.values())):
        component_items = [item for item in items if component_by_item[item] == component]
        anchors.add(item_to_idx[component_items[0]])

    free_indices = [idx for idx in range(len(items)) if idx not in anchors]
    free_pos = {idx: pos for pos, idx in enumerate(free_indices)}
    params = len(free_indices)

    def unpack(x: np.ndarray) -> np.ndarray:
        theta = np.zeros(len(items), dtype=np.float64)
        if params:
            theta[free_indices] = x
        return theta

    def nll_grad_hess(
        x: np.ndarray,
        need_hess: bool,
    ) -> tuple[float, np.ndarray, np.ndarray | None]:
        theta = unpack(x)
        nll = 0.0
        grad = np.zeros(params, dtype=np.float64)
        hess = np.zeros((params, params), dtype=np.float64) if need_hess else None

        for y_idx, candidate_idx in indexed_records:
            values = theta[candidate_idx]
            probs = softmax(values)
            nll += logsumexp(values) - theta[y_idx]

            local_free_positions: list[int] = []
            local_free_probs: list[float] = []
            for local_idx, global_idx in enumerate(candidate_idx):
                pos = free_pos.get(int(global_idx))
                if pos is None:
                    continue
                prob = float(probs[local_idx])
                grad[pos] += prob
                local_free_positions.append(pos)
                local_free_probs.append(prob)

            y_pos = free_pos.get(int(y_idx))
            if y_pos is not None:
                grad[y_pos] -= 1.0

            if need_hess and local_free_positions:
                p = np.array(local_free_probs, dtype=np.float64)
                cov = np.diag(p) - np.outer(p, p)
                idx = np.ix_(local_free_positions, local_free_positions)
                assert hess is not None
                hess[idx] += cov

        return nll, grad, hess

    if params == 0:
        nll, grad, _ = nll_grad_hess(np.array([]), False)
        return FitResult(model, items, np.zeros(len(items)), nll, True, 0, 0.0, 0, component_by_item, set(), set(), theta_bound)

    x = np.zeros(params, dtype=np.float64)
    success = False
    last_pg = math.inf
    last_iter = 0

    for iteration in range(1, max_iter + 1):
        nll, grad, hess = nll_grad_hess(x, True)
        projected = grad.copy()
        at_low = x <= -theta_bound + 1e-8
        at_high = x >= theta_bound - 1e-8
        projected[at_low & (grad > 0)] = 0.0
        projected[at_high & (grad < 0)] = 0.0
        last_pg = float(np.max(np.abs(projected))) if len(projected) else 0.0
        last_iter = iteration
        if last_pg < tol:
            success = True
            break

        assert hess is not None
        ridge = 1e-8
        try:
            direction = np.linalg.solve(hess + ridge * np.eye(params), grad)
        except np.linalg.LinAlgError:
            direction = grad
        if not np.all(np.isfinite(direction)):
            direction = grad

        accepted = False
        for step in (1.0, 0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625):
            candidate_x = np.clip(x - step * direction, -theta_bound, theta_bound)
            candidate_nll, _, _ = nll_grad_hess(candidate_x, False)
            if candidate_nll <= nll - 1e-10:
                x = candidate_x
                accepted = True
                if abs(nll - candidate_nll) < tol:
                    success = True
                break

        if success:
            break

        if not accepted:
            grad_step = min(0.1, 1.0 / (np.linalg.norm(projected) + 1e-12))
            candidate_x = np.clip(x - grad_step * projected, -theta_bound, theta_bound)
            candidate_nll, _, _ = nll_grad_hess(candidate_x, False)
            if candidate_nll <= nll - 1e-10:
                x = candidate_x
            else:
                break

    final_nll, final_grad, _ = nll_grad_hess(x, False)
    projected = final_grad.copy()
    projected[(x <= -theta_bound + 1e-8) & (final_grad > 0)] = 0.0
    projected[(x >= theta_bound - 1e-8) & (final_grad < 0)] = 0.0
    last_pg = float(np.max(np.abs(projected))) if len(projected) else 0.0
    success = success or last_pg < max(tol, 1e-5)

    theta = unpack(x)
    boundary_low = {items[idx] for idx, value in enumerate(theta) if value <= -theta_bound + 1e-6}
    boundary_high = {items[idx] for idx, value in enumerate(theta) if value >= theta_bound - 1e-6}
    return FitResult(
        model=model,
        items=items,
        theta=theta,
        nll=final_nll,
        success=success,
        iterations=last_iter,
        max_projected_grad=last_pg,
        params=params,
        component_by_item=component_by_item,
        boundary_low=boundary_low,
        boundary_high=boundary_high,
        theta_bound=theta_bound,
    )


def evaluate_nll(fit: FitResult, records: list[tuple[Any, list[Any]]]) -> float:
    if not records:
        return math.nan
    item_to_idx = {item: idx for idx, item in enumerate(fit.items)}
    nll = 0.0
    for y, candidates in records:
        if y not in item_to_idx or any(item not in item_to_idx for item in candidates):
            return math.nan
        idx = np.array([item_to_idx[item] for item in candidates], dtype=np.int64)
        nll += logsumexp(fit.theta[idx]) - fit.theta[item_to_idx[y]]
    return float(nll)


def equal_weight_nll(trials: list[dict[str, Any]]) -> float:
    return float(
        sum(math.log(trial["candidate_size"]) for trial in trials if trial["is_valid"])
    )


def relative_weights(
    fit: FitResult,
    observed_counts: Counter[Any],
) -> dict[Any, float]:
    weights = np.exp(fit.theta)
    result: dict[Any, float] = {}
    for component in sorted(set(fit.component_by_item.values())):
        idxs = [
            idx
            for idx, item in enumerate(fit.items)
            if fit.component_by_item[item] == component
        ]
        denominator_candidates = [
            weights[idx]
            for idx in idxs
            if observed_counts.get(fit.items[idx], 0) > 0 and fit.items[idx] not in fit.boundary_low
        ]
        if not denominator_candidates:
            denominator_candidates = [weights[idx] for idx in idxs]
        denominator = min(denominator_candidates)
        for idx in idxs:
            result[fit.items[idx]] = float(weights[idx] / denominator)
    return result


def predict_probabilities_m1(
    fit: FitResult,
    candidate_set: list[str],
) -> dict[str, float]:
    item_to_idx = {item: idx for idx, item in enumerate(fit.items)}
    idx = np.array([item_to_idx[event] for event in candidate_set], dtype=np.int64)
    probs = softmax(fit.theta[idx])
    return {event: float(prob) for event, prob in zip(candidate_set, probs)}


def split_trials_by_run(
    trials: list[dict[str, Any]],
    test_frac: float,
    seed: int,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    valid_trials = [trial for trial in trials if trial["is_valid"]]
    runs = sorted({trial["run_id"] for trial in valid_trials})
    rng = np.random.default_rng(seed)
    shuffled = np.array(runs, dtype=object)
    rng.shuffle(shuffled)
    test_size = max(1, int(round(len(shuffled) * test_frac))) if len(shuffled) > 1 else 0
    test_runs = set(str(run) for run in shuffled[:test_size])
    train = [trial for trial in valid_trials if trial["run_id"] not in test_runs]
    test = [trial for trial in valid_trials if trial["run_id"] in test_runs]
    return train, test


def reliability_label(observed_count: int) -> str:
    if observed_count <= 5:
        return "very_unstable"
    if observed_count <= 20:
        return "unstable"
    if observed_count <= 50:
        return "usable"
    if observed_count <= 100:
        return "fair"
    return "strong"


def item_to_text(item: Any) -> str:
    if isinstance(item, tuple):
        event, layer = item
        return f"{event}@L{layer}"
    return str(item)


def build_event_summary(
    trials: list[dict[str, Any]],
    layer_pool: dict[int, list[str]],
    caps: dict[str, float],
    fit_m1: FitResult,
    ci: dict[str, tuple[float, float]] | None,
) -> pd.DataFrame:
    candidate_counts: Counter[str] = Counter()
    observed_counts: Counter[str] = Counter()
    expected_counts: Counter[str] = Counter()

    for trial in trials:
        if not trial["is_valid"]:
            continue
        observed_counts[trial["observed"]] += 1
        for event in trial["candidate_set"]:
            candidate_counts[event] += 1
        for event, prob in predict_probabilities_m1(fit_m1, trial["candidate_set"]).items():
            expected_counts[event] += prob

    allowed_layers: defaultdict[str, list[int]] = defaultdict(list)
    for layer, pool in layer_pool.items():
        for event in pool:
            allowed_layers[event].append(layer)

    rel = relative_weights(fit_m1, Counter(observed_counts))
    rows = []
    for event in sorted(allowed_layers):
        observed = observed_counts[event]
        candidate = candidate_counts[event]
        cap = caps.get(event, math.inf)
        ci_low, ci_high = (math.nan, math.nan)
        if ci and event in ci:
            ci_low, ci_high = ci[event]
        diagnostics = []
        if event in fit_m1.boundary_low:
            diagnostics.append("low")
        if event in fit_m1.boundary_high:
            diagnostics.append("high")
        if candidate > 0 and observed == 0:
            diagnostics.append("zero_observed")
        rows.append(
            {
                "event": event,
                "allowed_layers": ",".join(str(layer) for layer in sorted(allowed_layers[event])),
                "cap": "inf" if math.isinf(cap) else int(cap),
                "candidate_count": candidate,
                "observed_count": observed,
                "expected_count_m1": float(expected_counts[event]),
                "rough_rate": observed / candidate if candidate else math.nan,
                "component": fit_m1.component_by_item.get(event),
                "theta_m1": fit_m1.theta[fit_m1.items.index(event)] if event in fit_m1.items else math.nan,
                "relative_weight_m1": rel.get(event, math.nan),
                "ci95_low": ci_low,
                "ci95_high": ci_high,
                "diagnostic": ";".join(diagnostics),
                "reliability": reliability_label(observed),
            }
        )
    return pd.DataFrame(rows)


def build_event_layer_summary(
    trials: list[dict[str, Any]],
    layer_pool: dict[int, list[str]],
    fit_m1: FitResult,
    fit_m2: FitResult,
) -> pd.DataFrame:
    candidate_counts: Counter[tuple[str, int]] = Counter()
    observed_counts: Counter[tuple[str, int]] = Counter()
    expected_m1: Counter[tuple[str, int]] = Counter()

    for trial in trials:
        if not trial["is_valid"]:
            continue
        layer = int(trial["layer"])
        observed_counts[(trial["observed"], layer)] += 1
        for event in trial["candidate_set"]:
            candidate_counts[(event, layer)] += 1
        for event, prob in predict_probabilities_m1(fit_m1, trial["candidate_set"]).items():
            expected_m1[(event, layer)] += prob

    observed_pair_counts = Counter(
        {(event, layer): count for (event, layer), count in observed_counts.items()}
    )
    rel_m2 = relative_weights(fit_m2, observed_pair_counts)

    rows = []
    for layer in sorted(layer_pool):
        for event in sorted(layer_pool[layer]):
            key = (event, layer)
            candidate = candidate_counts[key]
            observed = observed_counts[key]
            expected = float(expected_m1[key])
            diagnostics = []
            if key in fit_m2.boundary_low:
                diagnostics.append("low")
            if key in fit_m2.boundary_high:
                diagnostics.append("high")
            if candidate > 0 and observed == 0:
                diagnostics.append("zero_observed")
            rows.append(
                {
                    "layer": layer,
                    "event": event,
                    "candidate_count": candidate,
                    "observed_count": observed,
                    "rough_rate": observed / candidate if candidate else math.nan,
                    "expected_count_m1": expected,
                    "residual_m1": observed - expected,
                    "relative_weight_m2": rel_m2.get(key, math.nan),
                    "m2_diagnostic": ";".join(diagnostics),
                }
            )
    return pd.DataFrame(rows)


def build_repeat_distribution(
    nodes: pd.DataFrame,
    layer_pool: dict[int, list[str]],
    caps: dict[str, float],
) -> pd.DataFrame:
    runs = sorted(nodes["run_id"].unique())
    events = sorted({event for pool in layer_pool.values() for event in pool})
    grouped = nodes.groupby(["run_id", "event"]).size()
    rows = []
    for event in events:
        counts = [int(grouped.get((run_id, event), 0)) for run_id in runs]
        cap = caps.get(event, math.inf)
        ge2 = sum(1 for value in counts if value >= 2)
        max_count = max(counts) if counts else 0
        if not math.isinf(cap) and max_count > cap:
            judgment = "cap_violation"
        elif math.isinf(cap) and ge2 == 0:
            judgment = "repeat_config_but_no_repeat_seen"
        elif math.isinf(cap) and ge2 > 0:
            judgment = "repeat_seen"
        else:
            judgment = "no_repeat_seen"
        rows.append(
            {
                "event": event,
                "configured_cap": "inf" if math.isinf(cap) else int(cap),
                "runs_0": sum(1 for value in counts if value == 0),
                "runs_1": sum(1 for value in counts if value == 1),
                "runs_2": sum(1 for value in counts if value == 2),
                "runs_3_plus": sum(1 for value in counts if value >= 3),
                "max_count_in_one_run": max_count,
                "judgment": judgment,
            }
        )
    return pd.DataFrame(rows)


def model_comparison_rows(
    trials: list[dict[str, Any]],
    fit_m1: FitResult,
    fit_m2: FitResult,
    seed: int,
    test_frac: float,
    max_iter: int,
    theta_bound: float,
    tol: float,
) -> pd.DataFrame:
    valid_trials = [trial for trial in trials if trial["is_valid"]]
    n = len(valid_trials)
    m0_nll = equal_weight_nll(valid_trials)

    train_trials, test_trials = split_trials_by_run(valid_trials, test_frac, seed)
    train_m1 = fit_choice_model(make_choice_records(train_trials, "m1"), "M1_train", max_iter, theta_bound, tol)
    train_m2 = fit_choice_model(make_choice_records(train_trials, "m2"), "M2_train", max_iter, theta_bound, tol)

    test_m0_nll = equal_weight_nll(test_trials)
    test_m1_nll = evaluate_nll(train_m1, make_choice_records(test_trials, "m1"))
    test_m2_nll = evaluate_nll(train_m2, make_choice_records(test_trials, "m2"))

    rows = [
        {
            "model": "M0",
            "description": "equal weights inside reconstructed candidate set",
            "nll": m0_nll,
            "log_likelihood": -m0_nll,
            "params": 0,
            "aic": 2 * m0_nll,
            "bic": 2 * m0_nll,
            "test_nll": test_m0_nll,
            "test_log_likelihood": -test_m0_nll,
            "success": True,
            "iterations": 0,
            "max_projected_grad": 0.0,
        },
        fit_row(fit_m1, "global event weights", n, test_m1_nll),
        fit_row(fit_m2, "event-by-layer weights", n, test_m2_nll),
    ]

    df = pd.DataFrame(rows)
    ll_m1 = float(df.loc[df["model"] == "M1", "log_likelihood"].iloc[0])
    ll_m2 = float(df.loc[df["model"] == "M2", "log_likelihood"].iloc[0])
    k_m1 = int(df.loc[df["model"] == "M1", "params"].iloc[0])
    k_m2 = int(df.loc[df["model"] == "M2", "params"].iloc[0])
    lr = 2.0 * (ll_m2 - ll_m1)
    df["lr_vs_m1"] = np.nan
    df["lr_df_vs_m1"] = np.nan
    df.loc[df["model"] == "M2", "lr_vs_m1"] = lr
    df.loc[df["model"] == "M2", "lr_df_vs_m1"] = k_m2 - k_m1
    df["delta_aic_vs_best"] = df["aic"] - df["aic"].min()
    df["delta_bic_vs_best"] = df["bic"] - df["bic"].min()
    return df


def fit_row(fit: FitResult, description: str, n: int, test_nll: float) -> dict[str, Any]:
    return {
        "model": fit.model,
        "description": description,
        "nll": fit.nll,
        "log_likelihood": fit.log_likelihood,
        "params": fit.params,
        "aic": 2 * fit.params + 2 * fit.nll,
        "bic": math.log(max(n, 1)) * fit.params + 2 * fit.nll,
        "test_nll": test_nll,
        "test_log_likelihood": -test_nll if math.isfinite(test_nll) else math.nan,
        "success": fit.success,
        "iterations": fit.iterations,
        "max_projected_grad": fit.max_projected_grad,
    }


def bootstrap_m1_ci(
    nodes: pd.DataFrame,
    layer_pool: dict[int, list[str]],
    caps: dict[str, float],
    iterations: int,
    seed: int,
    max_iter: int,
    theta_bound: float,
    tol: float,
) -> dict[str, tuple[float, float]]:
    if iterations <= 0:
        return {}

    rng = np.random.default_rng(seed)
    run_ids = np.array(sorted(nodes["run_id"].unique()), dtype=object)
    target_events = sorted({event for pool in layer_pool.values() for event in pool})
    values: dict[str, list[float]] = {event: [] for event in target_events}

    for _ in range(iterations):
        sampled = rng.choice(run_ids, size=len(run_ids), replace=True)
        parts = []
        for idx, run_id in enumerate(sampled):
            part = nodes[nodes["run_id"] == run_id].copy()
            part["run_id"] = f"{run_id}#boot{idx}"
            parts.append(part)
        sampled_nodes = pd.concat(parts, ignore_index=True)
        sampled_trials, _ = build_trials(sampled_nodes, layer_pool, caps)
        records = make_choice_records(sampled_trials, "m1")
        fit = fit_choice_model(records, "M1", max_iter, theta_bound, tol)
        observed_counts = Counter(trial["observed"] for trial in sampled_trials if trial["is_valid"])
        rel = relative_weights(fit, observed_counts)
        for event in target_events:
            values[event].append(rel.get(event, math.nan))

    ci: dict[str, tuple[float, float]] = {}
    for event, event_values in values.items():
        arr = np.array(event_values, dtype=np.float64)
        arr = arr[np.isfinite(arr)]
        if len(arr):
            ci[event] = (float(np.percentile(arr, 2.5)), float(np.percentile(arr, 97.5)))
    return ci


def write_csv(path: Path, df: pd.DataFrame) -> None:
    df.to_csv(path, index=False, encoding="utf-8-sig")


def markdown_table(df: pd.DataFrame) -> str:
    if df.empty:
        return "_empty_"

    def cell(value: Any) -> str:
        if pd.isna(value):
            return ""
        if isinstance(value, float):
            return f"{value:.6g}"
        return str(value)

    columns = list(df.columns)
    lines = [
        "| " + " | ".join(columns) + " |",
        "| " + " | ".join("---" for _ in columns) + " |",
    ]
    for row in df.itertuples(index=False, name=None):
        lines.append("| " + " | ".join(cell(value) for value in row) + " |")
    return "\n".join(lines)


def write_outputs(
    output_dir: Path,
    nodes: pd.DataFrame,
    trials: list[dict[str, Any]],
    violations: list[dict[str, Any]],
    layer_pool: dict[int, list[str]],
    caps: dict[str, float],
    fit_m1: FitResult,
    fit_m2: FitResult,
    model_df: pd.DataFrame,
    event_summary: pd.DataFrame,
    event_layer_summary: pd.DataFrame,
    repeat_distribution: pd.DataFrame,
    args: argparse.Namespace,
) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)

    trial_rows = []
    for trial in trials:
        row = dict(trial)
        row["candidate_set"] = "|".join(trial["candidate_set"])
        trial_rows.append(row)
    write_csv(output_dir / "trials.csv", pd.DataFrame(trial_rows))

    violation_df = pd.DataFrame(violations)
    if violation_df.empty:
        violation_df = pd.DataFrame(
            columns=["type", "run_id", "step", "layer", "group_name", "event", "candidate_set", "detail"]
        )
    write_csv(output_dir / "violations.csv", violation_df)

    hard_summary = (
        violation_df.groupby("type", dropna=False)
        .size()
        .reset_index(name="count")
        .sort_values("type")
    )
    write_csv(output_dir / "hard_check_summary.csv", hard_summary)
    write_csv(output_dir / "model_comparison.csv", model_df)
    write_csv(output_dir / "event_summary_m1.csv", event_summary)
    write_csv(output_dir / "event_layer_summary.csv", event_layer_summary)
    write_csv(output_dir / "repeat_distribution.csv", repeat_distribution)

    config_rows = []
    for layer, pool in layer_pool.items():
        for event in pool:
            cap = caps.get(event, math.inf)
            config_rows.append(
                {
                    "layer": layer,
                    "event": event,
                    "cap": "inf" if math.isinf(cap) else int(cap),
                }
            )
    write_csv(output_dir / "candidate_pool_config.csv", pd.DataFrame(config_rows))

    summary = build_markdown_summary(
        nodes,
        trials,
        violations,
        model_df,
        event_summary,
        event_layer_summary,
        repeat_distribution,
        fit_m1,
        fit_m2,
        args,
    )
    (output_dir / "analysis_summary.md").write_text(summary, encoding="utf-8")


def build_markdown_summary(
    nodes: pd.DataFrame,
    trials: list[dict[str, Any]],
    violations: list[dict[str, Any]],
    model_df: pd.DataFrame,
    event_summary: pd.DataFrame,
    event_layer_summary: pd.DataFrame,
    repeat_distribution: pd.DataFrame,
    fit_m1: FitResult,
    fit_m2: FitResult,
    args: argparse.Namespace,
) -> str:
    valid_count = sum(1 for trial in trials if trial["is_valid"])
    invalid_count = len(trials) - valid_count
    violation_counts = Counter(v["type"] for v in violations)
    best_aic = model_df.sort_values("aic").iloc[0]["model"]
    best_test = model_df.sort_values("test_nll").iloc[0]["model"]
    m2_lr = model_df.loc[model_df["model"] == "M2", "lr_vs_m1"].iloc[0]
    m2_df = model_df.loc[model_df["model"] == "M2", "lr_df_vs_m1"].iloc[0]

    top_weights = (
        event_summary[event_summary["observed_count"] > 0]
        .sort_values("relative_weight_m1", ascending=False)
        .head(10)
    )
    largest_residuals = (
        event_layer_summary.assign(abs_residual=lambda df: df["residual_m1"].abs())
        .sort_values("abs_residual", ascending=False)
        .head(10)
    )
    repeat_flags = repeat_distribution[
        repeat_distribution["judgment"].isin(["cap_violation", "repeat_seen"])
    ]

    lines = [
        "# JieGarden encounter weight analysis",
        "",
        f"- Input: `{args.input}`",
        f"- Runs: {nodes['run_id'].nunique()}",
        f"- Nodes: {len(nodes)}",
        f"- Valid reconstructed trials: {valid_count}",
        f"- Invalid reconstructed trials: {invalid_count}",
        f"- M1 converged: {fit_m1.success} (iterations={fit_m1.iterations}, projected_grad={fit_m1.max_projected_grad:.3g})",
        f"- M2 converged: {fit_m2.success} (iterations={fit_m2.iterations}, projected_grad={fit_m2.max_projected_grad:.3g})",
        "",
        "## Hard checks",
    ]
    if violation_counts:
        for key, value in sorted(violation_counts.items()):
            lines.append(f"- {key}: {value}")
    else:
        lines.append("- No hard violations.")

    lines.extend(
        [
            "",
            "## Model comparison",
            "",
            markdown_table(
                model_df[
                    [
                        "model",
                        "nll",
                        "params",
                        "aic",
                        "bic",
                        "test_nll",
                        "delta_aic_vs_best",
                        "delta_bic_vs_best",
                    ]
                ]
            ),
            "",
            f"- Best AIC model: {best_aic}",
            f"- Best held-out test NLL model: {best_test}",
            f"- M2 vs M1 LR statistic: {m2_lr:.3f} on {m2_df:.0f} extra parameters",
            "",
            "## Largest M1 weights",
            "",
            markdown_table(
                top_weights[
                    ["event", "allowed_layers", "observed_count", "candidate_count", "relative_weight_m1", "diagnostic"]
                ]
            ),
            "",
            "## Largest layer residuals under M1",
            "",
            markdown_table(
                largest_residuals[
                    ["layer", "event", "observed_count", "expected_count_m1", "residual_m1"]
                ]
            ),
            "",
            "## Repeat diagnostics",
            "",
        ]
    )
    if repeat_flags.empty:
        lines.append("- No repeat flags.")
    else:
        lines.append(
            markdown_table(
                repeat_flags[
                    ["event", "configured_cap", "runs_2", "runs_3_plus", "max_count_in_one_run", "judgment"]
                ]
            )
        )
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    layers = parse_layers(args.layers)
    repeat_events = set() if args.no_default_repeat_events else set(DEFAULT_REPEAT_EVENTS)
    repeat_events.update(event.strip() for event in args.repeat_event if event.strip())

    nodes, load_violations = load_nodes(
        args.input,
        args.run_col,
        DEFAULT_GROUP_TO_LAYER,
        layers,
    )
    observed_events = set(nodes["event"].unique())
    if args.use_observed_pool:
        event_layers = build_event_layers_from_observed(nodes)
    else:
        event_layers = {
            event: [layer for layer in allowed_layers if layer in layers]
            for event, allowed_layers in DEFAULT_EVENT_LAYERS_1_2.items()
            if any(layer in layers for layer in allowed_layers)
        }

    layer_pool = build_layer_pool(
        event_layers,
        layers,
        observed_events,
        args.drop_unobserved_config_events,
    )
    caps = build_caps(layer_pool, repeat_events)
    trials, trial_violations = build_trials(nodes, layer_pool, caps)
    violations = load_violations + trial_violations
    valid_trials = [trial for trial in trials if trial["is_valid"]]

    if not valid_trials:
        raise RuntimeError("No valid trials after candidate reconstruction; inspect violations.csv")

    fit_m1 = fit_choice_model(
        make_choice_records(valid_trials, "m1"),
        "M1",
        args.max_iter,
        args.theta_bound,
        args.tol,
    )
    fit_m2 = fit_choice_model(
        make_choice_records(valid_trials, "m2"),
        "M2",
        args.max_iter,
        args.theta_bound,
        args.tol,
    )

    ci = bootstrap_m1_ci(
        nodes,
        layer_pool,
        caps,
        args.bootstrap,
        args.seed,
        args.max_iter,
        args.theta_bound,
        args.tol,
    )
    event_summary = build_event_summary(trials, layer_pool, caps, fit_m1, ci)
    event_layer_summary = build_event_layer_summary(trials, layer_pool, fit_m1, fit_m2)
    repeat_distribution = build_repeat_distribution(nodes, layer_pool, caps)
    model_df = model_comparison_rows(
        trials,
        fit_m1,
        fit_m2,
        args.seed,
        args.test_frac,
        args.max_iter,
        args.theta_bound,
        args.tol,
    )

    write_outputs(
        args.output_dir,
        nodes,
        trials,
        violations,
        layer_pool,
        caps,
        fit_m1,
        fit_m2,
        model_df,
        event_summary,
        event_layer_summary,
        repeat_distribution,
        args,
    )

    print(f"Wrote analysis outputs to {args.output_dir.resolve()}")
    print(model_df[["model", "nll", "params", "aic", "bic", "test_nll"]].to_string(index=False))
    if violations:
        print("Hard-check violations:")
        for key, value in sorted(Counter(v["type"] for v in violations).items()):
            print(f"  {key}: {value}")
    else:
        print("Hard-check violations: 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
