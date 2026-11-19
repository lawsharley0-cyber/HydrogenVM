"""Independent inventory conservation/format vectors; no proprietary data."""
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


@unittest.skipUnless(shutil.which('clang'), 'C compiler unavailable')
class GuestInventoryTests(unittest.TestCase):
    def test_conservation_and_checkpoint(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source, exe = directory/'inventory.c', directory/'inventory'
            source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef unsigned int u32; typedef unsigned char u8;
#define CALL(address,result,args) ((result (*)args)(address))
static uintptr_t s_read=(uintptr_t)&fread,s_write=(uintptr_t)&fwrite;
static u8 donor_home[]={1,2,3};static u32 donor_size=3;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
static u32 checksum(const u8 *p,u32 n) {u32 h=2166136261;while(n--)h=(h^*p++)*16777619;return h;}
#include "donor_inventory_state.inc"
int main(int argc,char **argv) {
    if(argc==1) {
        u32 sent,received,stock;
        while(scanf("%u%u",&sent,&received)==2) {
            struct DonorInventoryRow row={26000000,sent,received};
            if(donor_inventory_stock(&row,&stock))printf("%u\n",stock);else puts("invalid");
        }
        return 0;
    }
    u32 tag=0;if(fread(&tag,1,4,stdin)!=4 || tag!=0x31494e47u)return 2;
    read_donor_inventory(stdin,0x12345678);
    if(fgetc(stdin)!=EOF || donor_inventory_body_hash!=checksum(donor_home,donor_size))return 2;
    write_donor_inventory(stdout,0x12345678);
}
''')
            compiled = subprocess.run(['clang', '-std=c11', '-O2',
                '-I'+str(ROOT/'tools/clash_guest'), str(source), '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            vectors = [(0,0),(1,0),(1,1),(2,1),(0,1),(1000000000,1000000000),
                       (1,999999999),(0,1000000000),(2,0),(0xffffffff,0),
                       (0,0xffffffff),(1000000001,1000000000)]
            result = subprocess.run([str(exe)], input='\n'.join(f'{s} {r}' for s,r in vectors),
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout,
                '1\n0\n1\n0\n2\n1\n999999999\ninvalid\ninvalid\ninvalid\ninvalid\ninvalid\n')

            # One sent-and-replenished card retains its sent history at stock1.
            ids = [26000000,26000001,26000003,26000013,28000000,28000001]
            rows = b''.join(struct.pack('<3I', card, int(i==5), int(i==5))
                            for i,card in enumerate(ids))
            def pack(payload=rows, **fields):
                header = [0x31494e47,1,6,0x12345678,fnv(b'\1\2\3'),fnv(payload)]
                for index,value in fields.items():
                    header[int(index)] = value
                prefix = struct.pack('<6I', *header)
                return prefix+struct.pack('<I', fnv(prefix))+payload
            valid = pack()
            def run(data):
                return subprocess.run([str(exe),'read'], input=data, capture_output=True)
            accepted = run(valid)
            self.assertEqual((accepted.returncode,accepted.stdout), (0,valid), accepted.stderr)
            for size in range(len(valid)):
                with self.subTest(truncated=size):
                    self.assertEqual(run(valid[:size]).returncode, 2)
            for offset in range(len(valid)):
                damaged = bytearray(valid); damaged[offset] ^= 1
                with self.subTest(bitflip=offset):
                    self.assertEqual(run(damaged).returncode, 2)
            for index,value in [(1,0),(1,2),(2,5),(2,7),(3,0),(4,0)]:
                with self.subTest(header=(index,value)):
                    self.assertEqual(run(pack(**{str(index):value})).returncode, 2)
            # Recomputed checksums do not excuse wrong identities, negative
            # stock, unbounded transfers, or overflowing aggregate donations.
            for offset,value in [(0,26000001),(4,2),(8,0xffffffff),(12,26000000),
                                 (4,1000000000),(16,1000000000)]:
                payload = bytearray(rows); struct.pack_into('<I',payload,offset,value)
                with self.subTest(row=(offset,value)):
                    self.assertEqual(run(pack(payload)).returncode, 2)
            self.assertEqual(run(valid+b'\0').returncode, 2)


if __name__ == '__main__':
    unittest.main()
