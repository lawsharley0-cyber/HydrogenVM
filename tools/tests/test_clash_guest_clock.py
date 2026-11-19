"""Independent wall-time/native-tick boundaries; no proprietary data required."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class GuestClockTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang'), 'C compiler unavailable')
    def test_measured_time_and_native_accounting(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory/'clock.c'
            source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
typedef unsigned int u32;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
#include "clock_math.h"
int main(int argc,char **argv) {
    if(argc>1) {
        switch(argv[1][0]) {
            case 'b':guest_accounted_home_epoch(100,20,19);break;
            case 'o':guest_accounted_home_epoch(0xffffffffu,0,20);break;
            default:guest_accounted_home_epoch(0,0,0);break;
        }
        return 1;
    }
    printf("%u %u %u %u %u\n",guest_elapsed_home_seconds(0,100),
        guest_elapsed_home_seconds(100,99),guest_elapsed_home_seconds(100,100),
        guest_elapsed_home_seconds(100,141),guest_elapsed_home_seconds(1,0xffffffffu));
    printf("%u %u %u\n",guest_accounted_home_epoch(1000,40,59),
        guest_accounted_home_epoch(1000,40,60),guest_accounted_home_epoch(1000,40,1240));
    /* 60 seconds are already in the native timer; only the remaining
       41 seconds of a 101-second wall interval may be credited. */
    printf("%u\n",guest_elapsed_home_seconds(guest_accounted_home_epoch(1000,40,1240),1101));
    const u32 ticks[]={0,1,19,20,21,39,40,2147483647u,0xffffffffu};
    for(unsigned i=0;i<sizeof ticks/sizeof *ticks;++i)printf("%u ",guest_seconds_from_ticks(ticks[i]));
    puts("");
    return 0;
}
''')
            exe=directory/'clock'
            result=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),
                                   str(source),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(result.stdout.splitlines(),['0 0 0 41 2592000','1000 1001 1060','41',
                                                         '0 0 0 1 1 1 2 107374182 214748364 '])
            for case,reason in [('backwards','backwards'),('overflow','overflow'),('unstamped','backwards')]:
                result=subprocess.run([str(exe),case],capture_output=True,text=True)
                self.assertEqual(result.returncode,2)
                self.assertIn(reason,result.stderr)


if __name__=='__main__':unittest.main()
