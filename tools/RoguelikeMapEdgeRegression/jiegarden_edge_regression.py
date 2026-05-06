#!/usr/bin/env python3

from __future__ import annotations

import argparse
import itertools
import json
import math
from dataclasses import dataclass, field
from pathlib import Path

import cv2
import numpy as np


NODE_WIDTH = 138
NODE_HEIGHT = 47
DIRECTION_THRESHOLD = 7

SAMPLE_STEP_X = 2
MAX_STEP_Y_PER_SAMPLE = 8
EDGE_SCORE_THRESHOLD = 0.97
BRIGHT_GATE = 245
ROI_LEFT_RATIO = 0.15
ROI_RIGHT_RATIO = 0.85


@dataclass(frozen=True)
class Node:
    index: int
    column: int
    x: int
    y: int


@dataclass
class EdgeDebug:
    roi: tuple[int, int, int, int]
    start_range: tuple[int, int]
    end_range: tuple[int, int]
    sample_xs: list[int]
    path: list[int]
    support_values: list[float]
    avg_support: float
    total_step: float
    roi_image: np.ndarray = field(repr=False)
    gray: np.ndarray = field(repr=False)
    bright_mask: np.ndarray = field(repr=False)
    profile: np.ndarray = field(repr=False)


@dataclass
class EdgeScore:
    source: int
    target: int
    score: float = 0.0
    support_ratio: float = 0.0
    endpoint_score: float = 0.0
    continuity: float = 0.0
    end_error: float = 0.0
    path_length: int = 0
    accepted: bool = False
    reject_reason: str = ""
    debug: EdgeDebug | None = field(default=None, repr=False)


NODES = {
    1: Node(1, 1, 321, 239),
    2: Node(2, 1, 321, 360),
    3: Node(3, 2, 771, 119),
    4: Node(4, 2, 771, 239),
    5: Node(5, 2, 771, 361),
    6: Node(6, 2, 771, 482),
    7: Node(7, 3, 1221, 119),
    8: Node(8, 3, 1221, 240),
    9: Node(9, 3, 1221, 361),
    10: Node(10, 3, 1221, 482),
    11: Node(11, 4, 1671, 179),
    12: Node(12, 4, 1671, 300),
    13: Node(13, 4, 1671, 421),
    14: Node(14, 5, 2121, 240),
    15: Node(15, 5, 2121, 361),
}


EXPECTED_ACCEPTED = {(3, 9)}
EXPECTED_REJECTED = {(4, 8)}


def score_horizontal_edge(image: np.ndarray, source: Node, target: Node) -> EdgeScore:
    result = EdgeScore(source.index, target.index)

    raw_roi_x = source.x + NODE_WIDTH
    raw_roi_right = target.x
    raw_roi_width = raw_roi_right - raw_roi_x
    roi_x = raw_roi_x + round(raw_roi_width * ROI_LEFT_RATIO)
    roi_right = raw_roi_x + round(raw_roi_width * ROI_RIGHT_RATIO)
    roi_y = min(source.y, target.y)
    roi_bottom = max(source.y + NODE_HEIGHT, target.y + NODE_HEIGHT)
    roi_width = roi_right - roi_x
    roi_height = roi_bottom - roi_y
    if (
        roi_width <= SAMPLE_STEP_X
        or roi_height <= 2
        or roi_x < 0
        or roi_y < 0
        or roi_x + roi_width > image.shape[1]
        or roi_y + roi_height > image.shape[0]
    ):
        result.reject_reason = "invalid_roi"
        return result

    start_begin_y = int(np.clip(source.y - roi_y, 0, roi_height - 1))
    start_end_y = int(np.clip(source.y + NODE_HEIGHT - 1 - roi_y, 0, roi_height - 1))
    target_begin_y = int(np.clip(target.y - roi_y, 0, roi_height - 1))
    target_end_y = int(np.clip(target.y + NODE_HEIGHT - 1 - roi_y, 0, roi_height - 1))
    cropped = image[roi_y:roi_bottom, roi_x:roi_right]
    gray = cv2.cvtColor(cropped, cv2.COLOR_BGR2GRAY)

    _, bright_mask = cv2.threshold(gray, BRIGHT_GATE, 255, cv2.THRESH_BINARY)
    profile = bright_mask.astype(np.float32) / 255.0

    sample_xs = list(range(0, roi_width, SAMPLE_STEP_X))
    if not sample_xs or sample_xs[-1] != roi_width - 1:
        sample_xs.append(roi_width - 1)

    sample_count = len(sample_xs)
    result.path_length = sample_count
    profiles = np.zeros((sample_count, roi_height), dtype=np.float32)
    for sample, x in enumerate(sample_xs):
        x_begin = max(0, x - SAMPLE_STEP_X // 2)
        x_end = min(roi_width - 1, x + SAMPLE_STEP_X // 2)
        profiles[sample] = profile[:, x_begin : x_end + 1].max(axis=1)

    neg_inf = -1.0e9
    prev_dp = np.full(roi_height, neg_inf, dtype=np.float64)
    backtrace = np.full((sample_count, roi_height), -1, dtype=np.int32)
    for y in range(start_begin_y, start_end_y + 1):
        prev_dp[y] = profiles[0, y]

    for sample in range(1, sample_count):
        curr_dp = np.full(roi_height, neg_inf, dtype=np.float64)
        for y in range(roi_height):
            prev_begin = max(0, y - MAX_STEP_Y_PER_SAMPLE)
            prev_end = min(roi_height - 1, y + MAX_STEP_Y_PER_SAMPLE)
            for prev_y in range(prev_begin, prev_end + 1):
                value = prev_dp[prev_y] + profiles[sample, y]
                if value > curr_dp[y]:
                    curr_dp[y] = value
                    backtrace[sample, y] = prev_y
        prev_dp = curr_dp

    best_y = target_begin_y
    best_value = neg_inf
    for y in range(target_begin_y, target_end_y + 1):
        if prev_dp[y] > best_value:
            best_value = prev_dp[y]
            best_y = y
    if best_value <= neg_inf / 2:
        result.reject_reason = "endpoint_unreachable"
        return result

    path = [best_y] * sample_count
    for sample in range(sample_count - 1, 0, -1):
        prev_y = int(backtrace[sample, path[sample]])
        if prev_y < 0:
            break
        path[sample - 1] = prev_y

    support_values = np.array([profiles[sample, path[sample]] for sample in range(sample_count)], dtype=np.float64)
    support_sum = float(support_values.sum())
    support_count = int((support_values >= 0.25).sum())
    total_step = sum(abs(path[sample] - path[sample - 1]) for sample in range(1, sample_count))

    avg_support = support_sum / sample_count
    result.support_ratio = support_count / sample_count
    result.endpoint_score = 1.0
    result.continuity = 1.0 - min(1.0, total_step / max(1, sample_count - 1) / MAX_STEP_Y_PER_SAMPLE)
    result.end_error = 0.0
    result.score = result.support_ratio
    result.score = float(np.clip(result.score, 0.0, 1.0))

    if result.score < EDGE_SCORE_THRESHOLD:
        result.reject_reason = "below_threshold"

    result.debug = EdgeDebug(
        roi=(roi_x, roi_y, roi_width, roi_height),
        start_range=(start_begin_y, start_end_y),
        end_range=(target_begin_y, target_end_y),
        sample_xs=sample_xs,
        path=path,
        support_values=support_values.tolist(),
        avg_support=avg_support,
        total_step=float(total_step),
        roi_image=cropped.copy(),
        gray=gray.copy(),
        bright_mask=bright_mask.copy(),
        profile=profile.copy(),
    )

    return result


def edge_crosses(lhs: EdgeScore, rhs: EdgeScore) -> bool:
    lhs_source_y = NODES[lhs.source].y
    rhs_source_y = NODES[rhs.source].y
    lhs_target_y = NODES[lhs.target].y
    rhs_target_y = NODES[rhs.target].y
    return (
        lhs_source_y < rhs_source_y and lhs_target_y > rhs_target_y + DIRECTION_THRESHOLD
    ) or (
        lhs_source_y > rhs_source_y and lhs_target_y + DIRECTION_THRESHOLD < rhs_target_y
    )


def select_column_edges(scores: list[EdgeScore]) -> None:
    candidates = [i for i, score in enumerate(scores) if not score.reject_reason]
    selected_indices: list[int] = []

    if len(candidates) <= 20:
        best_score = -1.0
        best_endpoint_score = -1.0
        best_path_length = 1 << 30
        for mask in range(1, 1 << len(candidates)):
            selected: list[int] = []
            total_score = 0.0
            total_endpoint_score = 0.0
            total_path_length = 0
            valid = True
            for bit, score_index in enumerate(candidates):
                if (mask & (1 << bit)) == 0:
                    continue
                if any(edge_crosses(scores[score_index], scores[selected_index]) for selected_index in selected):
                    valid = False
                    break
                selected.append(score_index)
                total_score += scores[score_index].score
                total_endpoint_score += scores[score_index].endpoint_score
                total_path_length += scores[score_index].path_length
            if not valid:
                continue
            if (
                total_score > best_score
                or (
                    math.isclose(total_score, best_score, abs_tol=1e-6)
                    and (
                        total_endpoint_score > best_endpoint_score
                        or (
                            math.isclose(total_endpoint_score, best_endpoint_score, abs_tol=1e-6)
                            and total_path_length < best_path_length
                        )
                    )
                )
            ):
                best_score = total_score
                best_endpoint_score = total_endpoint_score
                best_path_length = total_path_length
                selected_indices = selected
    else:
        for score_index in sorted(
            candidates,
            key=lambda index: (-scores[index].score, -scores[index].endpoint_score, scores[index].path_length),
        ):
            if not any(edge_crosses(scores[score_index], scores[selected_index]) for selected_index in selected_indices):
                selected_indices.append(score_index)

    for selected_index in selected_indices:
        scores[selected_index].accepted = True
    for score in scores:
        if score.accepted:
            score.reject_reason = ""
        elif not score.reject_reason:
            score.reject_reason = "crossing_rejected"


def normalize_u8(image: np.ndarray) -> np.ndarray:
    if image.dtype == np.uint8:
        return image
    min_value = float(np.min(image))
    max_value = float(np.max(image))
    if max_value <= min_value:
        return np.zeros(image.shape, dtype=np.uint8)
    normalized = (image - min_value) / (max_value - min_value) * 255.0
    return np.clip(normalized, 0, 255).astype(np.uint8)


def to_bgr(image: np.ndarray) -> np.ndarray:
    image_u8 = normalize_u8(image)
    if image_u8.ndim == 2:
        return cv2.cvtColor(image_u8, cv2.COLOR_GRAY2BGR)
    return image_u8.copy()


def draw_label(image: np.ndarray, text: str, origin: tuple[int, int]) -> None:
    x, y = origin
    lines = text.splitlines()
    line_height = 18
    width = max(cv2.getTextSize(line, cv2.FONT_HERSHEY_SIMPLEX, 0.48, 1)[0][0] for line in lines) + 10
    height = line_height * len(lines) + 8
    cv2.rectangle(image, (x, y - height + 4), (x + width, y + 4), (0, 0, 0), -1)
    for i, line in enumerate(lines):
        cv2.putText(
            image,
            line,
            (x + 5, y - height + line_height * (i + 1)),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.48,
            (255, 255, 255),
            1,
            cv2.LINE_AA,
        )


def draw_edge_path(score: EdgeScore, base: np.ndarray, on_profile: bool = False) -> np.ndarray:
    debug = score.debug
    if debug is None:
        return base.copy()

    image = to_bgr(base)
    points = np.array([(x, y) for x, y in zip(debug.sample_xs, debug.path)], dtype=np.int32)
    if len(points) >= 2:
        cv2.polylines(image, [points], False, (0, 255, 255), 2, cv2.LINE_AA)
    start_begin, start_end = debug.start_range
    end_begin, end_end = debug.end_range
    cv2.rectangle(image, (0, start_begin), (image.shape[1] - 1, start_end), (0, 255, 0), 1, cv2.LINE_AA)
    cv2.rectangle(image, (0, end_begin), (image.shape[1] - 1, end_end), (0, 0, 255), 1, cv2.LINE_AA)
    cv2.circle(image, (0, debug.path[0]), 4, (0, 255, 0), -1, cv2.LINE_AA)
    cv2.circle(image, (image.shape[1] - 1, debug.path[-1]), 4, (0, 0, 255), -1, cv2.LINE_AA)
    if on_profile:
        draw_label(image, "yellow: DP path\ngreen: source y-range\nred: target y-range", (8, image.shape[0] - 8))
    return image


def resize_to_width(image: np.ndarray, width: int) -> np.ndarray:
    if image.shape[1] == width:
        return image
    scale = width / image.shape[1]
    height = max(1, int(round(image.shape[0] * scale)))
    return cv2.resize(image, (width, height), interpolation=cv2.INTER_AREA)


def write_labeled_image(path: Path, title: str, image: np.ndarray) -> None:
    image = to_bgr(image)
    label_height = 28
    canvas = np.zeros((image.shape[0] + label_height, image.shape[1], 3), dtype=np.uint8)
    canvas[label_height:] = image
    cv2.putText(canvas, title, (8, 19), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 1, cv2.LINE_AA)
    cv2.imwrite(str(path), canvas)


def write_edge_debug(score: EdgeScore, output_dir: Path) -> None:
    debug = score.debug
    if debug is None:
        return

    edge_dir = output_dir / f"edge_{score.source}_to_{score.target}"
    edge_dir.mkdir(parents=True, exist_ok=True)

    roi_with_path = draw_edge_path(score, debug.roi_image)
    profile_u8 = normalize_u8(debug.profile * 255.0)
    profile_color = cv2.applyColorMap(profile_u8, cv2.COLORMAP_TURBO)
    profile_with_path = draw_edge_path(score, profile_color, on_profile=True)

    write_labeled_image(edge_dir / "00_roi_path.png", "ROI + DP path", roi_with_path)
    write_labeled_image(edge_dir / "01_gray.png", "gray", debug.gray)
    write_labeled_image(edge_dir / "02_bright_mask.png", f"bright mask >= {BRIGHT_GATE}", debug.bright_mask)
    write_labeled_image(edge_dir / "03_gate_profile_path.png", "gate profile + DP path", profile_with_path)

    panel_width = 360
    panel_images = [
        resize_to_width(roi_with_path, panel_width),
        resize_to_width(to_bgr(debug.gray), panel_width),
        resize_to_width(to_bgr(debug.bright_mask), panel_width),
        resize_to_width(profile_with_path, panel_width),
    ]
    panel_height = max(image.shape[0] for image in panel_images)
    padded = []
    for image in panel_images:
        canvas = np.zeros((panel_height, panel_width, 3), dtype=np.uint8)
        canvas[: image.shape[0], : image.shape[1]] = image
        padded.append(canvas)
    combined = np.hstack(padded)
    draw_label(
        combined,
        (
            f"{score.source}->{score.target} "
            f"score={score.score:.3f} state={'accepted' if score.accepted else score.reject_reason}\n"
            f"support={score.support_ratio:.3f} endpoint={score.endpoint_score:.3f} "
            f"continuity={score.continuity:.3f} end_error={score.end_error:.1f}"
        ),
        (8, combined.shape[0] - 8),
    )
    cv2.imwrite(str(edge_dir / "summary.png"), combined)

    metrics = {
        "source": score.source,
        "target": score.target,
        "roi": debug.roi,
        "start_y_range": debug.start_range,
        "end_y_range": debug.end_range,
        "score": score.score,
        "avg_support": debug.avg_support,
        "support_ratio": score.support_ratio,
        "endpoint_score": score.endpoint_score,
        "continuity": score.continuity,
        "end_error": score.end_error,
        "total_step": debug.total_step,
        "path_length": score.path_length,
        "accepted": score.accepted,
        "reject_reason": score.reject_reason,
        "sample_xs": debug.sample_xs,
        "path": debug.path,
        "support_values": debug.support_values,
        "formula": "score = support_ratio",
        "constants": {
            "SampleStepX": SAMPLE_STEP_X,
            "MaxStepYPerSample": MAX_STEP_Y_PER_SAMPLE,
            "EdgeScoreThreshold": EDGE_SCORE_THRESHOLD,
            "BrightGate": BRIGHT_GATE,
            "RoiLeftRatio": ROI_LEFT_RATIO,
            "RoiRightRatio": ROI_RIGHT_RATIO,
        },
    }
    (edge_dir / "metrics.json").write_text(json.dumps(metrics, ensure_ascii=False, indent=2), encoding="utf-8")


def write_score_matrix(scores: list[EdgeScore], source_nodes: list[Node], target_nodes: list[Node], output_dir: Path) -> None:
    cell_width = 140
    cell_height = 78
    margin_left = 72
    margin_top = 52
    matrix_image = np.full(
        (margin_top + len(source_nodes) * cell_height, margin_left + len(target_nodes) * cell_width, 3),
        35,
        dtype=np.uint8,
    )

    score_by_edge = {(score.source, score.target): score for score in scores}
    for col, node in enumerate(target_nodes):
        cv2.putText(
            matrix_image,
            f"to {node.index}",
            (margin_left + col * cell_width + 34, 32),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.65,
            (255, 255, 255),
            1,
            cv2.LINE_AA,
        )
    for row, node in enumerate(source_nodes):
        cv2.putText(
            matrix_image,
            f"{node.index}",
            (24, margin_top + row * cell_height + 45),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.75,
            (255, 255, 255),
            1,
            cv2.LINE_AA,
        )

    for row, source in enumerate(source_nodes):
        for col, target in enumerate(target_nodes):
            score = score_by_edge[(source.index, target.index)]
            x = margin_left + col * cell_width
            y = margin_top + row * cell_height
            heat = int(np.clip(score.score, 0, 1) * 255)
            color = cv2.applyColorMap(np.array([[heat]], dtype=np.uint8), cv2.COLORMAP_TURBO)[0, 0].tolist()
            cv2.rectangle(matrix_image, (x, y), (x + cell_width - 2, y + cell_height - 2), color, -1)
            border = (0, 255, 0) if score.accepted else (0, 0, 255)
            cv2.rectangle(matrix_image, (x, y), (x + cell_width - 2, y + cell_height - 2), border, 2)
            state = "A" if score.accepted else score.reject_reason[:10]
            cv2.putText(
                matrix_image,
                f"{source.index}->{target.index}",
                (x + 8, y + 22),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.55,
                (0, 0, 0),
                2,
                cv2.LINE_AA,
            )
            cv2.putText(
                matrix_image,
                f"{score.score:.3f}",
                (x + 8, y + 45),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.58,
                (0, 0, 0),
                2,
                cv2.LINE_AA,
            )
            cv2.putText(
                matrix_image,
                state,
                (x + 8, y + 66),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.46,
                (0, 0, 0),
                1,
                cv2.LINE_AA,
            )

    cv2.imwrite(str(output_dir / "score_matrix.png"), matrix_image)


def write_overview(image: np.ndarray, scores: list[EdgeScore], output_dir: Path) -> None:
    overview = image.copy()
    overlay = overview.copy()
    for score in scores:
        source = NODES[score.source]
        target = NODES[score.target]
        start = (source.x + NODE_WIDTH, source.y + NODE_HEIGHT // 2)
        end = (target.x, target.y + NODE_HEIGHT // 2)
        if score.accepted:
            color = (0, 255, 0)
            thickness = 4
        elif score.reject_reason == "crossing_rejected":
            color = (0, 128, 255)
            thickness = 2
        else:
            color = (80, 80, 255)
            thickness = 1
        cv2.line(overlay, start, end, color, thickness, cv2.LINE_AA)
        midpoint = ((start[0] + end[0]) // 2, (start[1] + end[1]) // 2)
        cv2.putText(
            overlay,
            f"{score.source}->{score.target} {score.score:.2f}",
            midpoint,
            cv2.FONT_HERSHEY_SIMPLEX,
            0.48,
            color,
            1,
            cv2.LINE_AA,
        )
    cv2.addWeighted(overlay, 0.75, overview, 0.25, 0, overview)

    for node in NODES.values():
        if node.column not in (2, 3):
            continue
        center = (node.x + NODE_WIDTH // 2, node.y + NODE_HEIGHT // 2)
        cv2.circle(overview, center, 34, (255, 255, 0), 3, cv2.LINE_AA)
        cv2.putText(
            overview,
            str(node.index),
            (center[0] - 10, center[1] + 8),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.8,
            (255, 255, 255),
            2,
            cv2.LINE_AA,
        )
    draw_label(overview, "green: accepted\norange: crossing_rejected\nred: threshold/support rejected", (16, 90))
    cv2.imwrite(str(output_dir / "overview_edges.png"), overview)


def write_debug_outputs(
    image: np.ndarray,
    scores: list[EdgeScore],
    source_nodes: list[Node],
    target_nodes: list[Node],
    output_dir: Path,
) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    write_overview(image, scores, output_dir)
    write_score_matrix(scores, source_nodes, target_nodes, output_dir)
    for score in scores:
        write_edge_debug(score, output_dir)

    summary = []
    for score in scores:
        summary.append(
            {
                "source": score.source,
                "target": score.target,
                "score": score.score,
                "support_ratio": score.support_ratio,
                "endpoint_score": score.endpoint_score,
                "continuity": score.continuity,
                "end_error": score.end_error,
                "path_length": score.path_length,
                "accepted": score.accepted,
                "reject_reason": score.reject_reason,
            }
        )
    (output_dir / "edge_scores.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")


def main() -> int:
    default_image = Path(__file__).parent / "fixtures" / "jiegarden_2026.05.06-04.19.29.782_full_map.png"
    parser = argparse.ArgumentParser(description="Regression test for JieGarden data-collection map edge scoring.")
    parser.add_argument("--image", type=Path, default=default_image)
    parser.add_argument("--debug-dir", type=Path, help="Write preprocessing, DP path, and score visualizations.")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    image = cv2.imread(str(args.image), cv2.IMREAD_COLOR)
    if image is None:
        raise RuntimeError(f"failed to read image: {args.image}")

    scores: list[EdgeScore] = []
    source_nodes = [node for node in NODES.values() if node.column == 2]
    target_nodes = [node for node in NODES.values() if node.column == 3]
    for source, target in itertools.product(source_nodes, target_nodes):
        scores.append(score_horizontal_edge(image, source, target))
    select_column_edges(scores)

    if args.debug_dir:
        write_debug_outputs(image, scores, source_nodes, target_nodes, args.debug_dir)
        print(f"debug outputs: {args.debug_dir}")

    score_by_edge = {(score.source, score.target): score for score in scores}
    if args.verbose:
        for score in scores:
            state = "accepted" if score.accepted else score.reject_reason
            print(
                f"{score.source}->{score.target}: score={score.score:.3f} "
                f"support={score.support_ratio:.3f} endpoint={score.endpoint_score:.3f} "
                f"continuity={score.continuity:.3f} state={state}"
            )

    failures: list[str] = []
    for edge in EXPECTED_ACCEPTED:
        score = score_by_edge[edge]
        if not score.accepted:
            failures.append(f"expected {edge[0]}->{edge[1]} to be accepted, got {score.reject_reason}")
    for edge in EXPECTED_REJECTED:
        score = score_by_edge[edge]
        if score.accepted:
            failures.append(f"expected {edge[0]}->{edge[1]} to be rejected, score={score.score:.3f}")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1

    print("PASS: 3->9 accepted and 4->8 rejected on JieGarden regression map")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
