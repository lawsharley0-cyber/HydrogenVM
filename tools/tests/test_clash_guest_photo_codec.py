"""Independent liblzma decode oracle for on-device literal encoding."""
from pathlib import Path
import ctypes,lzma,random,shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
class PhotoCodecTests(unittest.TestCase):
 @unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
 def test_literal_encoder_known_size_boundaries_and_random_bytes(self):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'codec.c').write_text('typedef unsigned int u32;typedef unsigned char u8;\n#include "photo_lzma.inc"\nu32 encode(const u8 *in,u32 n,u8 *out,u32 cap){return photo_lzma(in,n,out,cap);}\n')
   lib=p/'codec.so';r=subprocess.run(['clang','-shared','-fPIC','-O2','-I'+str(ROOT/'tools/clash_guest'),str(p/'codec.c'),'-o',str(lib)],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr)
   fn=ctypes.CDLL(str(lib)).encode;fn.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_void_p,ctypes.c_uint];fn.restype=ctypes.c_uint
   rng=random.Random(734)
   for raw in [b'',b'a',b'a'*8192,bytes(range(256))*20,rng.randbytes(131072)]:
    out=ctypes.create_string_buffer(len(raw)*2+256);n=fn(raw,len(raw),out,len(out));self.assertGreater(n,0)
    encoded=out.raw[:n];decoded=lzma.decompress(encoded[:9]+bytes(4)+encoded[9:],format=lzma.FORMAT_ALONE);self.assertEqual(decoded,raw)
   small=ctypes.create_string_buffer(16);self.assertEqual(fn(bytes(range(256)),256,small,16),0)
if __name__=='__main__':unittest.main()
