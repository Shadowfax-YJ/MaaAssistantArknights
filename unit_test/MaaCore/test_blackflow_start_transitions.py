"""Replay the BlackFlow startup transitions that stalled on 2026-09-11."""

import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
BASE = json.loads((ROOT / "resource/tasks/Roguelike/base.json").read_text(encoding="utf-8"))
BLACKFLOW = json.loads((ROOT / "resource/tasks/Roguelike/BlackFlow.json").read_text(encoding="utf-8"))
PREFIX = "BlackFlow@"


def task(name):
    inherited = BASE.get(name.removeprefix(PREFIX), {})
    return inherited | BLACKFLOW.get(name, {})


def next_tasks(name):
    for candidate in task(name).get("next", []):
        if not candidate.startswith(PREFIX):
            candidate = PREFIX + candidate
        if candidate.endswith("#next"):
            yield from next_tasks(candidate.removesuffix("#next"))
        else:
            yield candidate


def next_match(name, visible):
    # ProcessTask selects the first recognized candidate; JustReturn always wins.
    return next((candidate for candidate in next_tasks(name)
                 if candidate in visible or task(candidate).get("algorithm") == "JustReturn"), None)


class StartupTransitions(unittest.TestCase):
    def test_selected_reward_is_confirmed_before_selecting_the_card_again(self):
        entry = PREFIX + "Roguelike@LastReward-EnterPoint"
        confirm = PREFIX + "Roguelike@LastRewardConfirm"
        # 06:01: both the selected card's title and its confirmation remain visible.
        self.assertEqual(next_match(entry, {entry, confirm}), confirm)

    def test_another_reward_can_be_selected_after_the_previous_one_is_claimed(self):
        entry = PREFIX + "Roguelike@LastReward-EnterPoint"
        self.assertEqual(next_match(entry, {entry}), entry)
        roles = PREFIX + "Roguelike@RolesDefault"
        self.assertEqual(next_match(entry, {roles}), roles)

    def test_give_up_dialog_is_confirmed_without_waiting_for_recruitment_animation(self):
        choose = PREFIX + "StartExplore@Roguelike@ChooseOper"
        confirm = PREFIX + "StartExplore@Roguelike@ChooseOperConfirmToGiveUp"
        # 06:04:25: the recruit plugin clicks GiveUp; no recruitment animation follows.
        self.assertEqual(next_match(choose, {confirm}), confirm)
        voucher = PREFIX + "StartExplore@Roguelike@RecruitOther"
        self.assertEqual(next_match(confirm, {voucher}), voucher)
        enter = PREFIX + "Roguelike@EnterAfterRecruit"
        self.assertEqual(next_match(confirm, {enter}), enter)

    def test_late_give_up_dialog_interrupts_animation_wait(self):
        wait = PREFIX + "StartExplore@Roguelike@RecruitAnimationWait"
        confirm = PREFIX + "StartExplore@Roguelike@ChooseOperConfirmToGiveUp"
        self.assertEqual(next_match(wait, {confirm}), confirm)


if __name__ == "__main__":
    unittest.main()
