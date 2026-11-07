"""Authored six-arena unlock, original NPC range and append-only profile checks."""
from pathlib import Path
import sys,unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from clash_bot_policy import arena_deck_names,load_policy,export_header

class ProgressionTests(unittest.TestCase):
    def fixture(self):
        pools=arena_deck_names();unlocks={}
        for arena,decks in pools.items():
            for cards in decks.values():
                for name in cards:unlocks.setdefault(name,arena)
        rows=[dict(Name=name,UnlockArena=f'Arena{a}',Rarity='Common',ManaCost='5') for name,a in unlocks.items()]
        tables={'spells_characters':rows,'spells_buildings':[],'spells_other':[],
            'arenas':[dict(Name='TrainingCamp',Arena='0',TrophyLimit='0')]+[
                dict(Name=f'Arena{i+1}',Arena=str(i+1),TrophyLimit=str(n)) for i,n in enumerate([0,400,800,1000,1200,1400])],
            'npcs':[dict(Name=f'Trainer{i}',Trophies='0',ExpLevel='1') for i in range(7)]+[
                dict(Name=f'Bot{i}',Trophies=str(score),ExpLevel=str(level)) for i,(score,level) in enumerate(
                    [(50,2),(150,2),(300,2),(450,3),(600,3),(700,3),(800,4),(900,4),(1000,5),(1100,5),(1200,6),(1300,6),(1400,7)])],
            'globals':[dict(Name=k,NumberValue=str(v)) for k,v in [('MAX_MANA',10),('START_MANA',5),('MANA_REGEN_MS',28000),('MANA_REGEN_MS_END',14000),('MANA_REGEN_MS_OVERTIME',14000)]]}
        return tables

    def test_six_arenas_keep_old_indices_and_original_npc_levels(self):
        data=self.fixture()
        with patch('clash_bot_policy.read_table',side_effect=lambda _,name:data[name]):
            p=load_policy('.',include_all_arenas=True)
            self.assertEqual([sum(x['arena']==a for x in p['profiles']) for a in range(1,7)],[18,18,12,12,12,6])
            for i,profile in enumerate(p['profiles'][:36]):
                self.assertEqual((profile['mission'],profile['arena'],profile['deck']),
                                 (18000007+i//6,1 if i<18 else 2,i%6))
            self.assertEqual([p['profiles'][i]['towerLevel'] for i in [0,18,36,48,60,72]],[2,3,4,5,6,7])
            for profile in p['profiles']:
                self.assertTrue(all(c['arena']<=profile['arena'] and c['cost']<=6 for c in profile['cards']))
                self.assertEqual(len({c['id'] for c in profile['cards']}),8)
            self.assertIn('#define GUEST_PROFILE_EXPORT_COUNT 78',export_header(p))

    def test_future_unlock_missing_arena_and_invalid_tower_fail(self):
        for fault in ['unlock','arena','tower']:
            data=self.fixture()
            if fault=='unlock':next(c for c in data['spells_characters'] if c['Name']=='Barbarians')['UnlockArena']='Arena6'
            elif fault=='arena':data['arenas'].pop()
            else:data['npcs'][-1]['ExpLevel']='13'
            with patch('clash_bot_policy.read_table',side_effect=lambda _,name:data[name]):
                with self.assertRaises(ValueError):load_policy('.',include_all_arenas=True)

if __name__=='__main__':unittest.main()
