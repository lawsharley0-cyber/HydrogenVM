"""Clash Royale v2.1306.1 CSV chest parser and reward engine.

Reads treasure_chests.csv, chest_order.csv, arenas.csv, and spell CSVs
from the client bundle and implements chest cycle progression,
unlock time calculation, and drop generation.
"""
import csv
import io
import math
import random
import zipfile
from pathlib import Path


class ClashCsvTables:
    def __init__(self, ipa_or_res_path):
        self.chests = {}            # name -> dict of chest attributes
        self.chest_by_id = {}       # global_id (19000000 + i) -> chest dict
        self.chest_id_by_name = {}  # name -> global_id
        self.arenas = {}            # name -> dict of arena attributes
        self.arena_by_index = {}    # int -> arena name
        self.cards = {}             # name -> dict of card attributes
        self.card_by_id = {}        # global_id -> card dict
        self.card_id_by_name = {}   # name -> global_id
        self.npcs = {}              # name -> dict of npc attributes
        self.npc_by_id = {}         # global_id -> npc dict
        self.newbie_chests = []     # 6 newbie chest names
        self.main_cycle = []        # 240 main cycle chest base names

        self._load(ipa_or_res_path)

    def _read_csv(self, source, filename, allow_empty_name=False):
        if isinstance(source, zipfile.ZipFile):
            prefix = "Payload/Clash Royale.app/res/csv_logic/"
            data = source.read(prefix + filename).decode("utf-8-sig")
        else:
            path = Path(source) / "csv_logic" / filename
            data = path.read_text(encoding="utf-8-sig")
        reader = csv.reader(io.StringIO(data))
        rows = list(reader)
        if len(rows) < 3:
            return []
        header = rows[0]
        records = []
        last_name = ""
        for r in rows[2:]:
            if not r:
                continue
            name = r[0].strip() if len(r) > 0 else ""
            if not name:
                if not allow_empty_name:
                    continue
                name = last_name
            else:
                last_name = name
            rec = {header[i]: r[i] for i in range(len(header)) if i < len(r)}
            rec["_RowName"] = r[0].strip() if len(r) > 0 else ""
            records.append(rec)
        return records

    def _load(self, path):
        path_obj = Path(path)
        archive = None
        if path_obj.suffix.lower() == ".ipa" or path_obj.is_file():
            archive = zipfile.ZipFile(path)
            source = archive
        else:
            source = path_obj

        try:
            # 1. Load arenas
            arena_rows = self._read_csv(source, "arenas.csv")
            for idx, r in enumerate(arena_rows):
                name = r["Name"]
                mult = int(r.get("ChestRewardMultiplier") or 100)
                arena_num = int(r.get("Arena") or idx)
                trophy_limit = int(r.get("TrophyLimit") or 0)
                demote_limit = int(r.get("DemoteTrophyLimit") or 0)
                pvp_loc = r.get("PvpLocation") or ""
                self.arenas[name] = {
                    "name": name,
                    "arena_number": arena_num,
                    "reward_multiplier": mult,
                    "trophy_limit": trophy_limit,
                    "demote_trophy_limit": demote_limit,
                    "pvp_location": pvp_loc,
                    "global_id": 54000000 + idx,
                }
                self.arena_by_index[arena_num] = name

            # 1b. Load NPCs
            npc_rows = self._read_csv(source, "npcs.csv")
            for idx, r in enumerate(npc_rows):
                name = r["Name"]
                gid = 18000000 + idx
                npc_data = {
                    "name": name,
                    "global_id": gid,
                    "trophies": int(r.get("Trophies") or 0),
                    "exp_level": int(r.get("ExpLevel") or 1),
                    "location": r.get("Location") or "",
                    "predefined_deck": r.get("PredefinedDeck") or "",
                    "tid": r.get("TID") or "",
                }
                self.npcs[name] = npc_data
                self.npc_by_id[gid] = npc_data

            # 2. Load spells (troops, buildings, spells)
            spell_files = [
                ("spells_characters.csv", 26000000, "Troop"),
                ("spells_buildings.csv", 27000000, "Building"),
                ("spells_other.csv", 28000000, "Spell"),
            ]
            for fn, base_id, category in spell_files:
                rows = self._read_csv(source, fn)
                for idx, r in enumerate(rows):
                    name = r["Name"]
                    gid = base_id + idx
                    card_data = {
                        "name": name,
                        "global_id": gid,
                        "category": category,
                        "rarity": r.get("Rarity", "Common"),
                        "unlock_arena": r.get("UnlockArena", "TrainingCamp"),
                        "mana_cost": int(r.get("ManaCost") or 0),
                    }
                    self.cards[name] = card_data
                    self.card_by_id[gid] = card_data
                    self.card_id_by_name[name] = gid

            # 3. Load treasure_chests.csv with inheritance
            chest_rows = self._read_csv(source, "treasure_chests.csv", allow_empty_name=False)
            raw_chests = {}
            for idx, r in enumerate(chest_rows):
                name = r["Name"]
                gid = 19000000 + idx
                raw_chests[name] = (idx, gid, r)
                self.chest_id_by_name[name] = gid

            # Resolve inheritance from BaseChest
            for name, (idx, gid, r) in raw_chests.items():
                base_name = r.get("BaseChest", "")
                merged = {}
                if base_name and base_name in raw_chests:
                    for k, v in raw_chests[base_name][2].items():
                        if v != "":
                            merged[k] = v
                for k, v in r.items():
                    if v != "" or k not in merged:
                        merged[k] = v

                # Calculate unlock duration in seconds
                days = int(merged.get("TimeTakenDays") or 0)
                hours = int(merged.get("TimeTakenHours") or 0)
                mins = int(merged.get("TimeTakenMinutes") or 0)
                secs = int(merged.get("TimeTakenSeconds") or 0)
                unlock_seconds = days * 86400 + hours * 3600 + mins * 60 + secs

                guaranteed = [s.strip() for s in merged.get("GuaranteedSpells", "").split(";") if s.strip()]

                chest_info = {
                    "name": name,
                    "global_id": gid,
                    "index": idx,
                    "base_chest": base_name,
                    "arena": merged.get("Arena", "TrainingCamp"),
                    "unlock_seconds": unlock_seconds,
                    "min_gold": int(merged.get("MinGold") or 0),
                    "max_gold": int(merged.get("MaxGold") or 0),
                    "random_spells": int(merged.get("RandomSpells") or 0),
                    "different_spells": int(merged.get("DifferentSpells") or 0),
                    "rare_chance": int(merged.get("RareChance") or 0),
                    "epic_chance": int(merged.get("EpicChance") or 0),
                    "guaranteed_spells": guaranteed,
                }
                self.chests[name] = chest_info
                self.chest_by_id[gid] = chest_info

            # 4. Load chest_order.csv (handles continuation rows with empty names)
            order_rows = self._read_csv(source, "chest_order.csv", allow_empty_name=True)
            for r in order_rows:
                chest_type = r.get("Chest", "").strip()
                if not chest_type:
                    continue
                if r["Name"].startswith("Newbie"):
                    self.newbie_chests.append(chest_type)
                else:
                    self.main_cycle.append(chest_type)
        finally:
            if archive:
                archive.close()

    def get_chest_for_cycle(self, cycle_index, arena_level=1):
        """Returns the chest dict according to player's cycle index and arena level."""
        if cycle_index < len(self.newbie_chests):
            chest_name = self.newbie_chests[cycle_index]
        else:
            cycle_pos = (cycle_index - len(self.newbie_chests)) % len(self.main_cycle)
            base_type = self.main_cycle[cycle_pos]
            if arena_level <= 0:
                chest_name = base_type
            else:
                chest_name = f"{base_type}_Arena{arena_level}"
                if chest_name not in self.chests:
                    chest_name = base_type

        return self.chests.get(chest_name)

    def get_arena_for_trophies(self, trophies, current_arena=1):
        if current_arena <= 0:
            current_arena = 1
        cur_name = self.arena_by_index.get(current_arena)
        if cur_name:
            cur = self.arenas[cur_name]
            if cur["demote_trophy_limit"] > 0 and trophies < cur["demote_trophy_limit"] and current_arena > 1:
                return current_arena - 1
        next_arena = current_arena + 1
        while True:
            nxt_name = self.arena_by_index.get(next_arena)
            if not nxt_name:
                break
            nxt = self.arenas[nxt_name]
            if nxt["trophy_limit"] <= 0:
                break
            if trophies >= nxt["trophy_limit"]:
                current_arena = next_arena
                next_arena += 1
            else:
                break
        return current_arena

    def find_best_npc_for_trophies(self, trophies):
        pvp_npcs = [npc for npc in self.npcs.values() if npc["global_id"] >= 18000007]
        if not pvp_npcs:
            return 18000007
        pvp_npcs.sort(key=lambda n: abs(n["trophies"] - trophies))
        return pvp_npcs[0]["global_id"]


class ChestProgressionEngine:
    def __init__(self, tables: ClashCsvTables):
        self.tables = tables

    def get_eligible_cards(self, max_arena_level, rarity=None):
        """Returns cards unlocked at or below max_arena_level, optionally filtered by rarity."""
        eligible = []
        for card in self.tables.cards.values():
            if rarity and card["rarity"].lower() != rarity.lower():
                continue
            unlock_arena_name = card["unlock_arena"]
            arena_info = self.tables.arenas.get(unlock_arena_name, {"arena_number": 0})
            if arena_info["arena_number"] <= max_arena_level:
                eligible.append(card)
        return eligible

    def calculate_rewards(self, chest_data, current_inventory=None, arena_level=1, rng=None):
        """Calculates gold and card drops for an opened chest."""
        if rng is None:
            rng = random.Random()
        if current_inventory is None:
            current_inventory = {}

        arena_name = self.tables.arena_by_index.get(arena_level, "Arena1")
        arena_info = self.tables.arenas.get(arena_name, {"reward_multiplier": 100})
        multiplier = arena_info["reward_multiplier"] / 100.0

        min_g = int(chest_data["min_gold"] * multiplier)
        max_g = int(chest_data["max_gold"] * multiplier)
        gold = rng.randint(min_g, max(min_g, max_g))

        total_cards = int(chest_data["random_spells"] * multiplier)
        cards_awarded = {}

        # 1. Guaranteed cards (e.g. Newbie chests)
        for spell_name in chest_data.get("guaranteed_spells", []):
            if spell_name in self.tables.cards:
                cid = self.tables.card_id_by_name[spell_name]
                cards_awarded[cid] = cards_awarded.get(cid, 0) + 1
                total_cards = max(0, total_cards - 1)

        # 2. Epics
        epic_chance = chest_data.get("epic_chance", 0)
        guaranteed_epics = 0
        if epic_chance > 0 and epic_chance <= total_cards:
            guaranteed_epics = total_cards // epic_chance
        elif epic_chance > 0:
            if rng.randint(1, epic_chance) == 1:
                guaranteed_epics = 1
        guaranteed_epics = min(guaranteed_epics, total_cards)

        if guaranteed_epics > 0:
            eligible_epics = self.get_eligible_cards(arena_level, "Epic")
            if eligible_epics:
                for _ in range(guaranteed_epics):
                    chosen = rng.choice(eligible_epics)
                    cid = chosen["global_id"]
                    cards_awarded[cid] = cards_awarded.get(cid, 0) + 1
                    total_cards -= 1

        # 3. Rares
        rare_chance = chest_data.get("rare_chance", 0)
        guaranteed_rares = 0
        if rare_chance > 0 and rare_chance <= total_cards:
            guaranteed_rares = total_cards // rare_chance
        elif rare_chance > 0:
            if rng.randint(1, rare_chance) == 1:
                guaranteed_rares = 1
        guaranteed_rares = min(guaranteed_rares, total_cards)

        if guaranteed_rares > 0:
            eligible_rares = self.get_eligible_cards(arena_level, "Rare")
            if eligible_rares:
                for _ in range(guaranteed_rares):
                    chosen = rng.choice(eligible_rares)
                    cid = chosen["global_id"]
                    cards_awarded[cid] = cards_awarded.get(cid, 0) + 1
                    total_cards -= 1

        # 4. Commons (inverse weight based on current player inventory)
        if total_cards > 0:
            eligible_commons = self.get_eligible_cards(arena_level, "Common")
            if not eligible_commons:
                eligible_commons = self.get_eligible_cards(arena_level)
            if eligible_commons:
                weights = [1.0 / (current_inventory.get(c["global_id"], 0) + 1) for c in eligible_commons]
                chosen_commons = rng.choices(eligible_commons, weights=weights, k=total_cards)
                for c in chosen_commons:
                    cid = c["global_id"]
                    cards_awarded[cid] = cards_awarded.get(cid, 0) + 1

        return {
            "gold": gold,
            "cards": cards_awarded,
            "chest_name": chest_data["name"],
            "global_id": chest_data["global_id"],
        }
