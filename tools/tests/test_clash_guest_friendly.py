"""Authored Friendly Challenge wire, escrow, and atomic-storage fault vectors."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def vint(value):
    # Independent non-negative test encoder; all identities/ages here are small.
    assert 0 <= value < (1 << 31)
    first = value & 63
    value >>= 6
    out = bytearray([first | (128 if value else 0)])
    while value:
        byte = value & 127
        value >>= 7
        out.append(byte | (128 if value else 0))
    return bytes(out)


def string(value):
    return struct.pack('>I', len(value)) + value


def entry(closed=False, optional=False):
    base = (vint(10) + vint(0) + vint(7) + vint(0) + vint(1) + vint(0) + vint(1) +
            string(b'Ada') + vint(3) + vint(2) + vint(0) + b'\0')
    return base + string(b'Local bout') + bytes([closed | (optional << 1)]) + (string(b'note') if optional else b'')


def result():
    return (vint(11) + vint(0) + vint(7) + vint(0) + vint(1) + vint(0) + vint(1) +
            string(b'Ada') + vint(3) + vint(2) + vint(0) + b'\0' +
            string(b'{"0":{"stars":1},"1":{"stars":2}}') +
            struct.pack('>IIII', 2, 0, 1306, 0) + b'\0')


@unittest.skipUnless(shutil.which('clang'), 'C compiler unavailable')
class GuestFriendlyTests(unittest.TestCase):
    def compile(self, directory, name, source):
        path = directory/(name+'.c')
        path.write_text(source)
        exe = directory/name
        completed = subprocess.run(['clang', '-std=c11', '-O2', '-I'+str(ROOT/'tools/clash_guest'),
                                    str(path), '-o', str(exe)], capture_output=True, text=True)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        return exe

    def test_native_challenge_and_result_boundaries(self):
        with tempfile.TemporaryDirectory() as temporary:
            exe = self.compile(Path(temporary), 'wire', r'''
#include <stdio.h>
#include <stdlib.h>
typedef unsigned int u32;typedef unsigned char u8;
static u32 friendly_sequence=7,friendly_state=1,friendly_namespace,friendly_result[32768],friendly_echo_crc;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
static u32 checksum(const u8 *p,u32 n) {u32 h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h;}
#include "reader.inc"
static void clan_skip_string(struct GuestReader *r,u32 maximum,int required) {
    u32 size=turn_int(r);if(size==0xffffffffu && !required)return;
    if((required && !size) || size>maximum || size>r->size-r->at)fail("string");r->at+=size;
}
struct SectorWriter {u8 *data;u32 size,capacity;};
static void sector_byte(struct SectorWriter *w,u8 b) {if(w->size>=w->capacity)fail("capacity");w->data[w->size++]=b;}
static void sector_bytes(struct SectorWriter *w,const u8 *p,u32 n) {while(n--)sector_byte(w,*p++);}
static void sector_vint(struct SectorWriter *w,int value) {
    u32 v=(u32)value;u8 first=v&63;v>>=6;sector_byte(w,first|(v?128:0));
    while(v){u8 b=v&127;v>>=7;sector_byte(w,b|(v?128:0));}
}
#include "friendly_wire.inc"
int main(int argc,char **argv) {
    friendly_state=(u32)atoi(argv[1]);u8 data[4096],out[4100];
    friendly_namespace=argc>2?(u32)atoi(argv[2]):0;
    u32 size=(u32)fread(data,1,sizeof data,stdin);struct SectorWriter w={out,0,sizeof out};
    friendly_entry_wire(&w,data,size,100,225);fwrite(out,1,w.size,stdout);
}
''')
            def run(data, state=1, namespace=0):
                return subprocess.run([str(exe), str(state),str(namespace)], input=data, capture_output=True)
            for raw, state in [(entry(), 1), (entry(optional=True), 2), (entry(closed=True), 4),
                               (entry(closed=True), 5), (result(), 3)]:
                age_at = raw.index(b'\0\0\0', 10)  # only used to derive expected below independently
                # Base ends immediately before the payload string. Recompute age at
                # the fixed final three VInts in the common StreamEntry header.
                prefix_length = 7 + 4 + 3 + 1 + 1
                original = raw[:prefix_length] + b'\0' + raw[prefix_length+1:]
                expected = raw[:prefix_length] + vint(125) + raw[prefix_length+1:]
                completed = run(raw, state)
                self.assertEqual((completed.returncode, completed.stdout), (0, expected), (state, raw.hex()))
                distinct = raw[:1]+b'\x02'+raw[2:]
                distinct_expected = expected[:1]+b'\x02'+expected[2:]
                completed = run(distinct,state,2)
                self.assertEqual((completed.returncode,completed.stdout),(0,distinct_expected))
                self.assertEqual(run(distinct,state,0).returncode,2)
                self.assertEqual(run(raw,state,2).returncode,2)
                for n in range(len(raw)):
                    self.assertEqual(run(raw[:n], state).returncode, 2)
                self.assertEqual(run(raw+b'\0', state).returncode, 2)
            self.assertEqual(run(entry(), 4).returncode, 2)
            self.assertEqual(run(entry(closed=True), 1).returncode, 2)
            self.assertEqual(run(result(), 2).returncode, 2)
            self.assertEqual(run(b'\x0b'+entry()[1:], 1).returncode, 2)
            self.assertEqual(run(b'\x09'+entry()[1:], 1).returncode, 2)
            self.assertEqual(run(entry()[:3]+b'\x08'+entry()[4:], 1).returncode, 2)

    def test_challenge_echo_uses_logic_command_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            exe = self.compile(Path(temporary), 'echo', r'''
#include <stdio.h>
#include <stdlib.h>
typedef unsigned int u32;typedef unsigned char u8;
static u32 friendly_sequence=7,friendly_state=1,friendly_namespace,friendly_result[32768],friendly_echo_crc;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
static u32 checksum(const u8 *p,u32 n) {u32 h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h;}
#include "reader.inc"
static void clan_skip_string(struct GuestReader *r,u32 maximum,int required) {
    u32 size=turn_int(r);if(size==0xffffffffu && !required)return;
    if((required && !size) || size>maximum || size>r->size-r->at)fail("string");r->at+=size;
}
struct SectorWriter {u8 *data;u32 size,capacity;};
static void sector_byte(struct SectorWriter *w,u8 b) {if(w->size>=w->capacity)fail("capacity");w->data[w->size++]=b;}
static void sector_bytes(struct SectorWriter *w,const u8 *p,u32 n) {while(n--)sector_byte(w,*p++);}
static void sector_vint(struct SectorWriter *w,int value) {
    u32 v=(u32)value;u8 first=v&63;v>>=6;sector_byte(w,first|(v?128:0));
    while(v){u8 b=v&127;v>>=7;sector_byte(w,b|(v?128:0));}
}
#include "friendly_wire.inc"
int main(int argc,char **argv) {
    friendly_echo_crc=(u32)strtoul(argv[1],0,0);u8 data[4096];
    u32 size=(u32)fread(data,1,sizeof data,stdin);struct GuestReader r={data,size,0,0,0};
    friendly_echo(&r);if(r.at!=size)fail("trailing echo");return 0;
}
''')
            message = b'Can you beat me?'
            payload = b'\1' + string(message)
            valid = payload + b'\x7f\x7f\x00\x01'  # tick, sequence, LogicLong high/low
            def run(data, crc=fnv(payload)):
                return subprocess.run([str(exe), str(crc)], input=data, capture_output=True)
            self.assertEqual(run(valid).returncode, 0)
            self.assertEqual(run(valid + b'\0').returncode, 2)
            self.assertEqual(run(valid[:-1]).returncode, 2)
            self.assertEqual(run(valid, fnv(b'other')).returncode, 2)
            self.assertEqual(run(payload + b'\x7f\x7f\x01\x01').returncode, 2)
            for n in range(len(valid)):
                self.assertEqual(run(valid[:n]).returncode, 2)

    def test_atomic_friendly_extension_and_escrow_faults(self):
        with tempfile.TemporaryDirectory() as temporary:
            exe = self.compile(Path(temporary), 'state', r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef unsigned int u32;typedef unsigned char u8;
#define CALL(address,result,args) ((result (*)args)(address))
static u32 rd(void *p,u32 s,u32 n,void *f) {return (u32)fread(p,s,n,f);}
static u32 wr(const void *p,u32 s,u32 n,void *f) {return (u32)fwrite(p,s,n,f);}
static uintptr_t s_read=(uintptr_t)&rd,s_write=(uintptr_t)&wr;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
static u32 checksum(const u8 *p,u32 n) {u32 h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h;}
#include "friendly_state.inc"
int main(void) {
    u32 tag;if(fread(&tag,1,4,stdin)!=4 || tag!=0x31524647u)return 2;
    read_friendly(stdin,0x12345678u);write_friendly(stdout,0x12345678u);
}
''')
            challenge, battle_result = entry(), result()
            header = [0x31524647, 1, 7, 3, 100, len(challenge), len(battle_result),
                      100, 95, 80, 80, 5, 0, 0x12345678, 0xfedcba98,
                      fnv(challenge), fnv(battle_result), 110, 0]
            def encode(fields, first=challenge, second=battle_result):
                raw = struct.pack('<19I', *fields)
                return raw + struct.pack('<I', fnv(raw)) + first + second
            expected = encode(header)
            def run(data):
                return subprocess.run([str(exe)], input=data, capture_output=True)
            completed = run(expected)
            self.assertEqual((completed.returncode, completed.stdout), (0, expected), completed.stderr)
            distinct_challenge=challenge[:1]+b'\x02'+challenge[2:]
            distinct_result=battle_result[:1]+b'\x02'+battle_result[2:]
            distinct_header=header.copy();distinct_header[1]=2;distinct_header[18]=2
            distinct_header[15]=fnv(distinct_challenge);distinct_header[16]=fnv(distinct_result)
            distinct=encode(distinct_header,distinct_challenge,distinct_result)
            completed=run(distinct)
            self.assertEqual((completed.returncode,completed.stdout),(0,distinct))
            for n in range(len(expected)):
                self.assertEqual(run(expected[:n]).returncode, 2)
            for at in range(len(expected)):
                broken = bytearray(expected);broken[at] ^= 1
                self.assertEqual(run(broken).returncode, 2)
            invalid = [(1,2),(2,0),(2,1000000001),(3,0),(3,6),(4,0),(5,19),(5,2049),
                       (6,32769),(6,0),(7,2000000001),(8,96),(9,101),(10,96),(11,4),
                       (12,1),(13,0),(17,0),(18,1)]
            for index, value in invalid:
                fields = header.copy();fields[index] = value
                self.assertEqual(run(encode(fields)).returncode, 2, (index, value))
            cancelled = header.copy();cancelled[3] = 4;cancelled[6] = 0
            cancelled[12] = 5;cancelled[16] = fnv(b'');cancelled[17] = 0
            self.assertEqual(run(encode(cancelled, challenge, b'')).returncode, 0)
            pending = header.copy();pending[3] = 1;pending[6] = 0;pending[12] = 5
            pending[16] = fnv(b'');pending[17] = 0
            self.assertEqual(run(encode(pending, challenge, b'')).returncode, 0)

    def test_legacy_identity_migration_preserves_text_and_result(self):
        with tempfile.TemporaryDirectory() as temporary:
            exe = self.compile(Path(temporary),'identity',r'''
#include <stdio.h>
#include <stdlib.h>
typedef unsigned int u32;typedef unsigned char u8;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
#include "friendly_identity.inc"
int main(int argc,char **argv) {
    u8 data[4096];u32 n=(u32)fread(data,1,sizeof data,stdin),split=(u32)atoi(argv[1]);
    u32 space=argc>2?(u32)atoi(argv[2]):0;
    if(split>n)return 3;
    int changed=friendly_identity_migration(data,split,data+split,n-split,&space);
    printf("%u %d\n",space,changed);fwrite(data,1,n,stdout);
}
''')
            first,second=entry(),result()
            for tail in (b'',second):
                expected=first[:1]+b'\x02'+first[2:]
                if tail:expected+=tail[:1]+b'\x02'+tail[2:]
                run=subprocess.run([str(exe),str(len(first))],input=first+tail,capture_output=True)
                self.assertEqual((run.returncode,run.stdout),(0,b'2 1\n'+expected))
                run=subprocess.run([str(exe),str(len(first)),'2'],input=expected,capture_output=True)
                self.assertEqual((run.returncode,run.stdout),(0,b'2 0\n'+expected))
            for namespace,bad in [('1',first+second),('0',b'\x09'+first[1:]+second),
                                  ('0',first+b'\x09'+second[1:]),('0',first[:1]+b'\x01'+first[2:]+second)]:
                run=subprocess.run([str(exe),str(len(first)),namespace],input=bad,capture_output=True)
                self.assertEqual(run.returncode,2)

    def test_terminal_friendly_allows_later_native_economy(self):
        with tempfile.TemporaryDirectory() as temporary:
            exe = self.compile(Path(temporary), 'wallet', r'''
#include <stdlib.h>
typedef unsigned int u32;
#include "friendly_wallet.inc"
int main(int argc,char **argv) {
    if(argc==1)return !(friendly_wallet_valid(1,2,95,80,100,95,80,80) &&
                       !friendly_wallet_valid(1,2,95,79,100,95,80,79));
    if(argc!=5)return 3;
    return friendly_wallet_valid((u32)atoi(argv[1]),atoi(argv[2]),atoi(argv[3]),atoi(argv[4]),
                                 3000,2995,3000,2995)?0:2;
}
''')
            self.assertEqual(subprocess.run([str(exe)]).returncode,0)
            # Terminal records describe a historical debit/refund. Subsequent
            # real upgrades, rewards and entry costs must survive relaunch.
            for state in (3,4,5):
                for clan in (2,3,4):
                    for gold in (0,2975,2994,3000,3023):
                        result = subprocess.run([str(exe),str(state),str(clan),str(gold),str(gold)])
                        self.assertEqual(result.returncode,0,(state,clan,gold))
            # An active paid intent still requires its exact protected wallet.
            for state in (1,2):
                self.assertEqual(subprocess.run([str(exe),str(state),'2','2995','2995']).returncode,0)
                for clan,gold,free in [(2,2994,2994),(2,2996,2995),(2,2995,2994),(3,2995,2995),(4,2995,2995)]:
                    self.assertEqual(subprocess.run([str(exe),str(state),str(clan),str(gold),str(free)]).returncode,2)
            for state,clan,gold,free in [(0,2,2995,2995),(6,2,2995,2995),(3,0,2995,2995),
                                        (3,1,2995,2995),(4,2,-1,0),(5,2,50,51),
                                        (3,2,2000000001,0),(3,2,50,-1)]:
                self.assertEqual(subprocess.run([str(exe),str(state),str(clan),str(gold),str(free)]).returncode,2)

    def test_sector_preserves_accounts_and_changes_only_battle_type(self):
        with tempfile.TemporaryDirectory() as temporary:
            exe = self.compile(Path(temporary), 'sector', r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned int u32;typedef unsigned char u8;
static u32 friendly_sequence=7,friendly_state=1,friendly_namespace,friendly_result[32768],friendly_echo_crc;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
static u32 checksum(const u8 *p,u32 n) {u32 h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h;}
#include "reader.inc"
static void clan_skip_string(struct GuestReader *r,u32 maximum,int required) {
    u32 size=turn_int(r);if(size==0xffffffffu && !required)return;
    if((required && !size) || size>maximum || size>r->size-r->at)fail("string");r->at+=size;
}
struct SectorWriter {u8 *data;u32 size,capacity;};
static void sector_byte(struct SectorWriter *w,u8 b) {if(w->size>=w->capacity)fail("capacity");w->data[w->size++]=b;}
static void sector_bytes(struct SectorWriter *w,const u8 *p,u32 n) {while(n--)sector_byte(w,*p++);}
static void sector_vint(struct SectorWriter *w,int value) {
    u32 v=(u32)value;u8 first=v&63;v>>=6;sector_byte(w,first|(v?128:0));
    while(v){u8 b=v&127;v>>=7;sector_byte(w,b|(v?128:0));}
}
#include "friendly_wire.inc"
int main(void) {
    u8 data[4096],original[4096];u32 size=(u32)fread(data,1,sizeof data,stdin);
    memcpy(original,data,size);u32 offset=friendly_sector_type(data,size);
    if(offset>=size || data[offset]!=0)return 2;
    for(u32 i=0;i<size;++i)if(i!=offset && data[i]!=original[i])return 2;
    data[offset]=2;fwrite(data,1,size,stdout);return 0;
}
''')
            prefix = (vint(11) + vint(22) + vint(33) + vint(44) +
                      struct.pack('>I', 0x12345678) + vint(55) +
                      vint(540) + vint(1) + vint(0) + vint(12) + vint(34))
            accounts = b'\x7f' + vint(0) + vint(0) + vint(1)
            account_at = len(prefix)
            raw = prefix + accounts + vint(0) + b'\xaa\xbb'
            type_at = account_at + len(accounts)
            expected = raw[:type_at] + vint(2) + raw[type_at+1:]
            completed = subprocess.run([str(exe)], input=raw, capture_output=True)
            self.assertEqual((completed.returncode, completed.stdout), (0, expected), completed.stderr)
            self.assertEqual(completed.stdout[account_at:type_at], accounts)
            self.assertEqual(sum(a != b for a, b in zip(raw, completed.stdout)), 1)
            for n in range(type_at+1):
                broken = subprocess.run([str(exe)], input=raw[:n], capture_output=True)
                self.assertEqual(broken.returncode, 2, n)
            for at in range(account_at, type_at+1):
                broken = bytearray(raw);broken[at] = 2
                rejected = subprocess.run([str(exe)], input=broken, capture_output=True)
                self.assertEqual(rejected.returncode, 2, at)


if __name__ == '__main__':
    unittest.main()
