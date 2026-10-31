import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import agent_queue as q  # noqa: E402

CFG = {
    "wip_limit": 2,
    "agents": {
        "a": {"can_run_games": True, "weekly_budget": 2, "prefers": ["T1", "T3"]},
        "b": {"can_run_games": False, "weekly_budget": 1, "prefers": ["T3"]},
    },
}


def card(n, tid, *labs, body=""):
    return {"number": n, "title": f"{tid} something", "body": body, "labels": [{"name": l} for l in ("card", *labs)], "state": "open", "html_url": f"u/{n}"}


class PickTests(unittest.TestCase):
    def test_card_id_keeps_sub_card_letter(self):
        self.assertEqual(q.card_id({"number": 1, "title": "T5b GL model"}), "T5b")
        self.assertEqual(q.card_id({"number": 1, "title": "T5 umbrella"}), "T5")
        self.assertEqual(q.card_id({"number": 9, "title": "misc"}), "#9")

    def test_deps_parse(self):
        self.assertEqual(q.deps({"body": "x\nDepends-on: #3, #4\n"}), [3, 4])
        self.assertEqual(q.deps({"body": "nothing"}), [])

    def test_prefers_listed_card_then_lowest_number(self):
        cards = [card(5, "T9", "status:ready"), card(6, "T3", "status:ready"), card(7, "T1", "status:ready")]
        got, _ = q.pick("a", cards, set(), {}, CFG)
        self.assertEqual(got["number"], 7)

    def test_open_dependency_blocks_card(self):
        cards = [card(1, "T1", "status:in-progress"), card(2, "T2", "status:ready", body="Depends-on: #1")]
        got, _ = q.pick("a", cards, set(), {}, CFG)
        self.assertIsNone(got)

    def test_closed_dependency_unblocks_card(self):
        cards = [card(2, "T2", "status:ready", body="Depends-on: #1")]
        got, _ = q.pick("a", cards, {1}, {}, CFG)
        self.assertEqual(got["number"], 2)

    def test_game_run_card_skips_agent_without_capability(self):
        cards = [card(1, "T1", "status:ready", "needs:game-run"), card(2, "T3", "status:ready")]
        got, _ = q.pick("b", cards, set(), {}, CFG)
        self.assertEqual(got["number"], 2)

    def test_hotspot_card_waits_while_another_hotspot_is_in_progress(self):
        cards = [card(1, "T1", "status:in-progress", "hotspot"), card(2, "T2", "status:ready", "hotspot"), card(3, "T4", "status:ready")]
        got, _ = q.pick("a", cards, set(), {}, CFG)
        self.assertEqual(got["number"], 3)

    def test_work_limit(self):
        cards = [card(1, "T1", "status:in-progress"), card(2, "T2", "status:in-progress"), card(3, "T3", "status:ready")]
        got, why = q.pick("a", cards, set(), {}, CFG)
        self.assertIsNone(got)
        self.assertIn("work limit", why)

    def test_weekly_budget_hands_work_to_others(self):
        cards = [card(3, "T3", "status:ready")]
        got, why = q.pick("b", cards, set(), {"b": 1}, CFG)
        self.assertIsNone(got)
        self.assertIn("weekly budget", why)
        got, _ = q.pick("a", cards, set(), {"b": 1}, CFG)
        self.assertEqual(got["number"], 3)

    def test_umbrella_cards_do_not_count_or_get_picked(self):
        cards = [card(1, "T5", "status:in-progress", "umbrella"), card(2, "T5", "status:ready", "umbrella"),
                 card(3, "T5a", "status:in-progress"), card(4, "T3", "status:ready")]
        got, _ = q.pick("a", cards, set(), {}, CFG)   # limit 2: only #3 counts, #1 is an umbrella
        self.assertEqual(got["number"], 4)

    def test_unknown_agent(self):
        got, why = q.pick("zzz", [], set(), {}, CFG)
        self.assertIsNone(got)
        self.assertIn("unknown agent", why)

    def test_unknown_agent_uses_default_profile(self):
        cfg = dict(CFG, default={"can_run_games": False, "weekly_budget": 2, "prefers": []})
        cards = [card(1, "T1", "status:ready", "needs:game-run"), card(2, "T3", "status:ready")]
        got, _ = q.pick("newagent", cards, set(), {}, cfg)
        self.assertEqual(got["number"], 2)

    def test_hotspot_areas_conflict_only_within_the_same_area(self):
        cards = [card(1, "T1", "status:in-progress", "hotspot:process"),
                 card(2, "T2a", "status:ready", "hotspot:process"),
                 card(3, "T2b", "status:ready", "hotspot:objc-runtime")]
        got, _ = q.pick("a", cards, set(), {}, CFG)
        self.assertEqual(got["number"], 3)

    def test_card_ids_for_game_and_probe_cards(self):
        self.assertEqual(q.card_id({"number": 1, "title": "G3 Goat crash"}), "G3")
        self.assertEqual(q.card_id({"number": 1, "title": "P1 probe sweep"}), "P1")

    def test_cards_not_ready_are_ignored(self):
        cards = [card(1, "T1", "status:blocked"), card(2, "T2", "status:review")]
        got, _ = q.pick("a", cards, set(), {}, CFG)
        self.assertIsNone(got)


if __name__ == "__main__":
    unittest.main()
