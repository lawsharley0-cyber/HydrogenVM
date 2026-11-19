"""Compile guest code against authored placeholders, without proprietary assets."""
import shutil
import subprocess
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class GuestCompileTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'),'host C++ compiler unavailable')
    def test_fixture_exporter_builds_without_game_data(self):
        with tempfile.TemporaryDirectory() as directory:
            result=subprocess.run(['clang++','-std=c++20','-O2','-I'+str(ROOT/'src'),
                                   str(ROOT/'tools/clash_guest/fixture_export.cpp'),
                                   str(ROOT/'src/runtime/offline/ClashState.cpp'),'-lz',
                                   '-o',str(Path(directory)/'fixture-export')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)

    @unittest.skipUnless(shutil.which('clang'),'ARM cross compiler unavailable')
    def test_freestanding_armv7_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory)
            header='\n'.join('#define API_'+name+' 0x1000u' for name in ['puts','printf','malloc','abort','dlsym','sinf','cosf','memcpy','memset'])
            header+='\n#define INITIAL_AVATAR_OFFSET 0u\n'
            header+='static const unsigned char initial_home[16]={0},initial_login[16]={0};\n'
            header+='#define GUEST_MAX_TOWER_LEVEL 12\nstruct GuestTowerPatch {unsigned part,offset,width;int values[12];};\n'
            header+='struct GuestSector {const unsigned char *parts[4];unsigned sizes[4];const struct GuestTowerPatch *patches;unsigned patchCount,lowBits,nameOffset,nameSize;};\n'
            header+='static const struct GuestSector npc_sectors[7]={0};\n'
            header+='#define GUEST_ARENA_PROFILE_COUNT 18\nstatic const struct GuestSector arena1_sectors[18]={0};\nstatic const int arena1_npc_trophies[18]={0},arena1_missions[18]={0},arena_mana_rates[3]={28000,14000,14000};\nstatic const unsigned arena1_deck_ids[18]={0};\n'
            header+='static const int arena_profile_arenas[18]={0};\n'
            header+='#define GUEST_MOD_UNIT_COUNT 1\n#define GUEST_MOD_SETTINGS_VTABLE 0\nstruct ModUnitDef {const char *name,*projectile;unsigned cardId;};\nstatic const struct ModUnitDef mod_unit_defs[1]={{"Knight","",26000000}};\n'
            header+='struct ForgeBlueprint {unsigned id;const char *rows[6];};\nstatic const struct ForgeBlueprint forge_blueprints[1]={{26000000,{"","","","","",""}}};\n'
            header+='static const char *forge_table_paths[6]={"a","b","c","d","e","f"};\nstatic const unsigned forge_source_sizes[6]={1,1,1,1,1,1},forge_source_hashes[6]={0},forge_group_counts[2]={1,1};\n'
            header+='#define GUEST_MECH_CARD_COUNT 1\n#define GUEST_MECH_TROOP_COUNT 1\n#define GUEST_MECH_SHOT_COUNT 1\n'
            header+='struct MechCardDef {unsigned id;const char *name;};\nstatic const struct MechCardDef mech_cards[]={{26000000,"Test troop"}};\n'
            header+='static const char *mech_troop_names[]={"Test troop"},*mech_shot_names[]={"Test shot"};\n'
            header+='#define GUEST_ART_SOURCE_TOKEN 1u\nstatic const char *forge_art_templates[1][4][4]={0};\n'
            (directory/'generated.h').write_text(header)
            command=['clang','-target','armv7-apple-ios6.0','-mthumb','-Oz','-ffreestanding',
                     '-fno-builtin','-fno-stack-protector','-fno-pic','-fno-unwind-tables',
                     '-fno-asynchronous-unwind-tables','-Werror','-I'+str(directory),'-c',
                     str(ROOT/'tools/clash_guest/transport.c'),'-o',str(directory/'guest.o')]
            import struct
            expected_undefined={
                '_guest_original_battle_ended','_guest_original_reward_execute',
                '_guest_original_settings_click','_guest_original_mail_execute','_guest_original_request_execute',
                '_guest_original_challenge_execute','_guest_original_mod_health','_guest_original_mod_damage','_guest_original_mod_shot_damage','_guest_original_mod_string'}
            variants=[[],['-DGUEST_PROFILE_SAVES=1'],['-DGUEST_PROBE_DONATION_ACTORS=1'],
                      ['-DGUEST_PROFILE_SAVES=1','-DGUEST_PROBE_DONATION_ACTORS=1']]
            for flags in variants:
                result=subprocess.run(command+flags,capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                # Every variant is a real freestanding ARMv7 Mach-O object.
                self.assertEqual(struct.unpack_from('<III',(directory/'guest.o').read_bytes()),
                                 (0xfeedface,12,9))
                self.assertEqual(b'DonationActorProbe:' in (directory/'guest.o').read_bytes(),
                                 '-DGUEST_PROBE_DONATION_ACTORS=1' in flags)
                if shutil.which('llvm-nm'):
                    result=subprocess.run(['llvm-nm','--undefined-only',str(directory/'guest.o')],
                                          capture_output=True,text=True)
                    self.assertEqual(result.returncode,0,result.stderr)
                    self.assertEqual(set(result.stdout.split()),expected_undefined,flags)


if __name__=='__main__':unittest.main()
