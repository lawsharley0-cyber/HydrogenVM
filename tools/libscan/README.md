# Bundled library scanner

`python3 tools/libscan/scan.py Game.ipa Game.apk Game.app` prints one JSON report
with a match count per input and individual hits per binary. APK scanning selects
`lib/armeabi-v7a/*.so`. IPA and app scanning examines 32-bit ARMv7 Mach-O
and ARMv6 images, including slices of universal binaries. Bare ELF and Mach-O files are
also accepted. An address is the even virtual address from the image, before
any loader slide. The `isa` field identifies ARM or Thumb. A version guess can
include multiple releases whose retained instruction bytes are identical.

The database covers zlib 1.2.7 and 1.2.8, libpng 1.6.16 and 1.6.37, libvorbis
1.3.5 and a pinned Tremor checkout. Clang 19 and GCC 14 compiled the pinned
sources with `-O2` and `-Os` in both instruction sets. The Clang iOS profiles
use an ARMv7 iOS 6 target and the Test Lab stub SDK. Prefixes contain up to
256 bytes. Masks omit PC-relative branch and literal fields and relevant
object relocations. Tremor's short synthesis wrapper also needs a hit for
its branch destination.

For older Mach-O builds, the scanner also checks defined text symbols and
instruction references to zlib and libpng constants. It recognizes ARM and
Thumb PC-relative literal loads and address construction. It ties those
references to the Mach-O function-start table, or to a nearby frame prologue
when the table is unavailable. The zlib fast path also needs a direct call
chain from the identified inflate function. The CRC lookup table is checked
alongside the loop's XOR/shift instruction and evidence of a bundled inflate
function.

A defined-symbol hit is high confidence. Constant-reference hits are medium
confidence and leave the version guess empty. Use those addresses as
candidates for follow-up inspection. The constant-reference path covers
zlib and libpng in Mach-O. ELF, libvorbis and Tremor matching still depends
on the compiled signatures. Compiler revisions, LTO and build options
can change compiled code shapes.

## Rebuild and validate

Use Python 3.12+, Clang, GCC for arm-linux-gnueabihf, LLVM binutils, LLD and
ARM-capable cctools ld64. Generate the stub SDK using
`python3 tools/hvm_testlab.py run --hvm build/hvm --case TL-D` first and give the generator its
`usr/lib` directory. The ARM sysroot argument points at the cross headers
directory's parent, typically `/usr/arm-linux-gnueabihf`.

```sh
python3 tools/libscan/build_signatures.py \
  --work /path/to/scratch/libscan \
  --sysroot /usr/arm-linux-gnueabihf \
  --ld64 "$HVM_LD64" \
  --ios-libs build/ios/_work/TestLab/sdk/usr/lib
python3 -m unittest discover -s tools/tests -p test_libscan.py
```

The generator checks each archive against its pinned SHA-256, builds and links
fixtures outside the repository, and compares matches in stripped copies to
the original linker symbols. It also checks a deliberately unrelated binary
with misleading symbol names. Only the derived signature database is checked
into the repository.
