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
            header+='struct GuestSector {const unsigned char *parts[4];unsigned sizes[4];const struct GuestTowerPatch *patches;unsigned patchCount,lowBits;};\n'
            header+='static const struct GuestSector npc_sectors[7]={0};\n'
            header+='static const struct GuestSector arena1_sectors[13]={0};\nstatic const int arena1_npc_trophies[13]={0};\n'
            (directory/'generated.h').write_text(header)
            command=['clang','-target','armv7-apple-ios6.0','-mthumb','-Oz','-ffreestanding',
                     '-fno-builtin','-fno-stack-protector','-fno-pic','-fno-unwind-tables',
                     '-fno-asynchronous-unwind-tables','-Werror','-I'+str(directory),'-c',
                     str(ROOT/'tools/clash_guest/transport.c'),'-o',str(directory/'guest.o')]
            result=subprocess.run(command,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            # A real ARMv7 Mach-O object, not a host-only build of the C code.
            import struct
            self.assertEqual(struct.unpack_from('<III',(directory/'guest.o').read_bytes()),
                             (0xfeedface,12,9))
            # Profiling must remain freestanding too; no extra native runtime
            # dependency may creep in through timing/counter arithmetic.
            result=subprocess.run(command+['-DGUEST_PROFILE_SAVES=1'],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            if shutil.which('llvm-nm'):
                # Compiling succeeds even when ARMv7 emits a compiler-runtime
                # division helper. A freestanding IPA cannot resolve one.
                result=subprocess.run(['llvm-nm','--undefined-only',str(directory/'guest.o')],
                                      capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual(set(result.stdout.split()),{
                    '_guest_original_battle_ended','_guest_original_reward_execute',
                    '_guest_original_settings_click'})


if __name__=='__main__':unittest.main()
