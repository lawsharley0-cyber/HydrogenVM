"""Native profile framing, menu scope and file-failure preservation checks."""
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from clash_custom_cards import fnv

PREAMBLE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
typedef unsigned int u32;
typedef unsigned char u8;
#define CALL(address,result,args) ((result (*)args)(address))
#define API_puts puts
static void fail(const char *why){fprintf(stderr,"%s",why);exit(4);}
static uintptr_t symbol(const char *name){
#define MAP(n,f) if(!strcmp(name,n))return (uintptr_t)f
    MAP("fopen",fopen); MAP("fread",fread); MAP("fwrite",fwrite);
    MAP("fclose",fclose); MAP("ferror",ferror); MAP("getenv",getenv);
    MAP("fflush",fflush); MAP("fileno",fileno); MAP("fsync",fsync);
    MAP("mkdir",mkdir); MAP("rename",rename);
    fprintf(stderr,"unexpected symbol %s",name);exit(5);
}
static void path_append(char *out,const char *a,const char *b,u32 cap){
    if(snprintf(out,cap,"%s%s",a,b)>=(int)cap)fail("path");
}
static u32 checksum(const u8 *bytes,u32 count){
    u32 hash=2166136261u;while(count--)hash=(hash^*bytes++)*16777619u;return hash;
}
'''


@unittest.skipUnless(shutil.which("clang"), "C compiler required")
class MusicTests(unittest.TestCase):
    def test_native_assignment_reader_and_menu_scope(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            directory = root / "Documents/ClashMusic-v1"
            directory.mkdir(parents=True)
            source = root / "probe.c"
            module = (ROOT / "tools/clash_guest/music.inc").read_text()
            source.write_text(PREAMBLE + module + r'''
int main(void){
    music_initialize();
    printf("selected=%u\n",music_generation);
    const char *valid[]={"/app/res/music/ios/menu_03.m4a","res/music/ios/wizmen04.m4a","/app/res/music/ios/new_menu_01.m4a"};
    for(int i=0;i<3;++i)if(!music_menu_path(valid[i]))return 10;
    const char *invalid[]={"/app/res/music/ios/2min_loop_battle_01.m4a","/app/res/music/ios/scroll_win_02.m4a","/app/other/menu_03.m4a","/app/xres/music/ios/menu_03.m4a","/app/res/music/ios/menu_03.m4a.tmp"};
    for(int i=0;i<5;++i)if(music_menu_path(invalid[i]))return 11;
    return 0;
}
''')
            exe = root / "probe"
            subprocess.run(["clang", "-w", "-fno-builtin", str(source), "-o", str(exe)], check=True)
            def run(expected):
                import os
                result = subprocess.run([str(exe)], env=dict(os.environ, HOME=str(root)), capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"selected={expected}", result.stdout)
            run(0)
            track = b"authored track"
            (directory / "track-00000001.audio").write_bytes(track)
            head = struct.pack("<4I", 0x3143554d, 1, len(track), fnv(track))
            data = head + struct.pack("<I", fnv(head))
            profile = directory / "menu.bin"
            profile.write_bytes(data)
            run(1)
            for bad in (data[:-1], data + b"x", data[:16] + bytes(4)):
                profile.write_bytes(bad)
                run(0)
                self.assertEqual(profile.read_bytes(), bad)
            profile.write_bytes(data)
            (directory / "track-00000001.audio").write_bytes(b"changed")
            run(0)
            self.assertEqual(profile.read_bytes(), data)
