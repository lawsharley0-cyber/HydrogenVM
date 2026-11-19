"""Original base-first 526 wire and complete-turn-before-mutation regression."""
from pathlib import Path
import shutil,struct,subprocess,tempfile,unittest

ROOT=Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
class GuestMemberTests(unittest.TestCase):
    def test_kick_only_commits_after_validating_entire_turn(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory=Path(temporary);source=directory/'members.c';exe=directory/'members'
            source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef unsigned int u32;typedef unsigned char u8;
static u8 input[4096];static u32 length;static int commits;
static u32 pending_size,reward_applied,pending_kind,pending_id,completed_kind,completed_id;
static int requested_kind,requested_id;static u32 clan_chat_sequence=1;
static void fail(const char *why) {fprintf(stderr,"%s commits=%d\n",why,commits);exit(2);}
static u32 native_length(void *p) {return length;}
static void *native_ui(void) {fail("unexpected UI capture before dispatch");return 0;}
static int native_diagnostic(const char *format,int a,int b,u32 c) {return fprintf(stderr,format,a,b,c);}
static uintptr_t lookup(u32 address) {
    if(address==0x198d19)return (uintptr_t)&native_length;
    if(address==0x91f7d)return (uintptr_t)&native_ui;
    if(address==2)return (uintptr_t)&native_diagnostic;
    fail("unexpected native call");return 0;
}
#define API_malloc 1
#define API_printf 2
#define CALL(address,result,args) ((result (*)args)lookup(address))
#define FIELD(p,offset) ((uintptr_t)input)
static void capture_current_home(void *p,int force) {fail("unexpected capture");}
#include "home_turn.inc"
struct SectorWriter {u8 *data;u32 size,capacity;};
static void sector_byte(struct SectorWriter *w,u8 b) {if(w->size>=w->capacity)fail("capacity");w->data[w->size++]=b;}
static void sector_bytes(struct SectorWriter *w,const u8 *p,u32 n) {while(n--)sector_byte(w,*p++);}
static void sector_vint(struct SectorWriter *w,int value) {fail("unexpected chat encoder");}
#include "clan_chat_wire.inc"
#include "clan_member_wire.inc"
static void name_echo(struct GuestReader *r) {fail("unexpected name");}
static void clan_echo(struct GuestReader *r,int k) {fail("unexpected clan");}
static void mail_echo(struct GuestReader *r) {fail("unexpected mail");}
static void request_echo(struct GuestReader *r) {fail("unexpected request");}
static void friendly_echo(struct GuestReader *r) {fail("unexpected friendly");}
static void kick_from_turn(const u8 *p,u32 n) {++commits;fwrite(p,1,n,stdout);}
int main(void) {length=fread(input,1,sizeof input,stdin);return consume_home_turn((void*)1);}
''')
            result=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),str(source),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            # Literal type526 (0x20e -> VInt8e08), base tick1/sequence2 and
            # executor(0,1), String, fixed BE target(3,1), four-byte -1 trailer.
            target=struct.pack('>II',3,1);reason=struct.pack('>I',6)+b'Reason'
            command=b'\x8e\x08\x01\x02\0\x01'+reason+target
            valid=b'\x01\0\x01'+command+b'\xff'*4
            def run(data):return subprocess.run([str(exe)],input=data,capture_output=True)
            result=run(valid)
            self.assertEqual((result.returncode,result.stdout),(0,target+reason),result.stderr)
            for optional in [b'\0'*4,b'\xff'*4]:
                result=run(b'\x01\0\x01'+command[:6]+optional+target+b'\xff'*4)
                self.assertEqual((result.returncode,result.stdout),(0,target+optional),result.stderr)
            invalid=[valid[:n] for n in range(len(valid))]+[
                valid[:-1]+b'\0',valid+b'\0',
                b'\x01\0\x02'+command+command+b'\xff'*4,
                b'\x01\0\x01'+command[:-1]+b'\x02'+b'\xff'*4,
                b'\x01\0\x01'+command[:4]+b'\0\x02'+command[6:]+b'\xff'*4,
                b'\x01\0\x01'+command[:6]+struct.pack('>I',1025)+b'x'*1025+target+b'\xff'*4,
                # Valid kick plus matchmaking537 in one bundle must also be
                # rejected before dispatch, not partially applied.
                b'\x01\0\x02'+command+b'\x99\x08\x01\x03\0\x01\0\0'+b'\xff'*4]
            for data in invalid:
                result=run(data)
                with self.subTest(vector=data.hex()[:80]):
                    self.assertEqual(result.returncode,2,result.stderr)
                    self.assertEqual(result.stdout,b'')
                    self.assertIn(b'commits=0',result.stderr)


if __name__=='__main__':unittest.main()
