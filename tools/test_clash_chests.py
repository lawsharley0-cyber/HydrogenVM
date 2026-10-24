"""Unit tests for clash_chests.py."""
import unittest
from pathlib import Path
from clash_chests import ClashCsvTables, ChestProgressionEngine

IPA_PATH = Path('/var/tmp/clash-client.ipa')


class TestClashChests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not IPA_PATH.exists():
            raise unittest.SkipTest("Pinned client IPA not found at /var/tmp/clash-client.ipa")
        cls.tables = ClashCsvTables(IPA_PATH)
        cls.engine = ChestProgressionEngine(cls.tables)

    def test_arenas_loaded(self):
        self.assertIn("TrainingCamp", self.tables.arenas)
        self.assertIn("Arena1", self.tables.arenas)
        self.assertIn("Arena6", self.tables.arenas)
        self.assertEqual(self.tables.arenas["TrainingCamp"]["reward_multiplier"], 100)
        self.assertEqual(self.tables.arenas["Arena2"]["reward_multiplier"], 135)

    def test_spells_loaded(self):
        self.assertIn("Knight", self.tables.cards)
        self.assertIn("Archer", self.tables.cards)
        self.assertIn("Fireball", self.tables.cards)
        self.assertIn("Cannon", self.tables.cards)

        knight = self.tables.cards["Knight"]
        self.assertEqual(knight["category"], "Troop")
        self.assertEqual(knight["rarity"], "Common")
        self.assertEqual(knight["global_id"], 26000000)

        fireball = self.tables.cards["Fireball"]
        self.assertEqual(fireball["category"], "Spell")
        self.assertEqual(fireball["rarity"], "Rare")
        self.assertEqual(fireball["global_id"], 28000000)

    def test_chest_order_cycle(self):
        # 6 Newbie chests
        self.assertEqual(len(self.tables.newbie_chests), 6)
        self.assertEqual(self.tables.newbie_chests[0], "Newbie1")
        self.assertEqual(self.tables.newbie_chests[1], "Newbie2")
        self.assertEqual(self.tables.newbie_chests[2], "NewbieRandom")
        self.assertEqual(self.tables.newbie_chests[3], "NewbieFixedEpic")

        # Exactly 240 chests in main cycle
        self.assertEqual(len(self.tables.main_cycle), 240)
        self.assertEqual(self.tables.main_cycle[0], "Silver")
        self.assertEqual(self.tables.main_cycle[1], "Silver")
        self.assertEqual(self.tables.main_cycle[2], "Silver")
        self.assertEqual(self.tables.main_cycle[3], "Gold")

    def test_chest_inheritance(self):
        silver_base = self.tables.chests["Silver"]
        self.assertEqual(silver_base["unlock_seconds"], 3 * 3600)  # 3 hours
        self.assertEqual(silver_base["min_gold"], 19)
        self.assertEqual(silver_base["max_gold"], 29)

        # Silver_Arena1 should inherit unlock_seconds from Silver
        silver_a1 = self.tables.chests["Silver_Arena1"]
        self.assertEqual(silver_a1["unlock_seconds"], 3 * 3600)
        self.assertEqual(silver_a1["min_gold"], 19)
        self.assertEqual(silver_a1["max_gold"], 29)

        # Gold_Arena1 should have 8 hours unlock time
        gold_a1 = self.tables.chests["Gold_Arena1"]
        self.assertEqual(gold_a1["unlock_seconds"], 8 * 3600)

        # Newbie1 should have 15s unlock time
        newbie1 = self.tables.chests["Newbie1"]
        self.assertEqual(newbie1["unlock_seconds"], 15)

    def test_cycle_rotation(self):
        # First 6 chests are Newbie chests
        c0 = self.tables.get_chest_for_cycle(0, arena_level=1)
        self.assertEqual(c0["name"], "Newbie1")

        c1 = self.tables.get_chest_for_cycle(1, arena_level=1)
        self.assertEqual(c1["name"], "Newbie2")

        c5 = self.tables.get_chest_for_cycle(5, arena_level=1)
        self.assertEqual(c5["name"], "NewbieRandom")

        # 7th chest (index 6) starts main cycle in Arena 1
        c6 = self.tables.get_chest_for_cycle(6, arena_level=1)
        self.assertEqual(c6["name"], "Silver_Arena1")

        # 10th chest (index 9) is Gold in Arena 1
        c9 = self.tables.get_chest_for_cycle(9, arena_level=1)
        self.assertEqual(c9["name"], "Gold_Arena1")

    def test_reward_generation(self):
        # Newbie 1 guarantees Knight
        newbie1 = self.tables.chests["Newbie1"]
        rewards = self.engine.calculate_rewards(newbie1, arena_level=0)
        self.assertGreaterEqual(rewards["gold"], 16)
        self.assertLessEqual(rewards["gold"], 16)
        knight_id = self.tables.card_id_by_name["Knight"]
        self.assertIn(knight_id, rewards["cards"])
        self.assertGreaterEqual(rewards["cards"][knight_id], 1)

        # Arena 2 Gold chest applies 135% reward multiplier
        gold_a2 = self.tables.chests["Gold_Arena2"]
        rewards_g2 = self.engine.calculate_rewards(gold_a2, arena_level=2)
        base_gold_min = int(64 * 1.35)
        base_gold_max = int(96 * 1.35)
        self.assertGreaterEqual(rewards_g2["gold"], base_gold_min)
        self.assertLessEqual(rewards_g2["gold"], base_gold_max)
        self.assertGreater(sum(rewards_g2["cards"].values()), 0)

    def test_arena_trophy_progression(self):
        self.assertEqual(self.tables.get_arena_for_trophies(0, 1), 1)
        self.assertEqual(self.tables.get_arena_for_trophies(399, 1), 1)
        self.assertEqual(self.tables.get_arena_for_trophies(400, 1), 2)
        self.assertEqual(self.tables.get_arena_for_trophies(360, 2), 2)
        self.assertEqual(self.tables.get_arena_for_trophies(340, 2), 1)
        self.assertEqual(self.tables.get_arena_for_trophies(800, 1), 3)

    def test_npc_matchmaking(self):
        self.assertEqual(len(self.tables.npcs), 20)
        self.assertEqual(self.tables.npcs["Npc8"]["trophies"], 50)
        self.assertEqual(self.tables.find_best_npc_for_trophies(40), 18000007)
        self.assertEqual(self.tables.find_best_npc_for_trophies(140), 18000008)
        self.assertEqual(self.tables.find_best_npc_for_trophies(310), 18000009)


if __name__ == '__main__':
    unittest.main()
