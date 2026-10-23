"""Legacy static Mach-O inspection shared with the pinned-client probe."""
import struct

CPU_ARM, CPU_ARM64 = 12, 0x0100000C
ARM_SUB = {5: "armv4t", 6: "armv6", 7: "armv5", 9: "armv7", 10: "armv7f", 11: "armv7s", 12: "armv7k", 13: "armv8", 14: "armv6m", 15: "armv7m", 16: "armv7em"}

def macho_info(path):
    """{'archs': [...], 'encrypted': bool, 'error': str|None} for the 32-bit ARM slice HydrogenVM would run."""
    try:
        data = open(path, "rb").read()
    except OSError as e:
        return {"archs": [], "encrypted": False, "error": str(e)}
    slices = []
    magic = struct.unpack(">I", data[:4])[0]
    if magic in (0xCAFEBABE, 0xBEBAFECA):
        n = struct.unpack(">I", data[4:8])[0]
        for i in range(n):
            cpu, sub, off, size, _ = struct.unpack(">5I", data[8 + 20 * i:28 + 20 * i])
            slices.append((cpu, sub & 0xFFFFFF, off, size))
    else:
        cpu, sub = struct.unpack("<II", data[4:12])
        slices.append((cpu, sub & 0xFFFFFF, 0, len(data)))
    archs = []
    for cpu, sub, _, _ in slices:
        archs.append("arm64" if cpu == CPU_ARM64 else ARM_SUB.get(sub, "arm?%d" % sub) if cpu == CPU_ARM else "cpu%d" % cpu)
    arm32 = [s for s in slices if s[0] == CPU_ARM]
    if not arm32:
        return {"archs": archs, "encrypted": False, "error": "no 32-bit ARM slice (64-bit only)"}
    # Prefer armv7 (what HydrogenVM runs), else the first 32-bit slice.
    cpu, sub, off, size = next((s for s in arm32 if s[1] in (9, 11)), arm32[0])
    d = data[off:off + size]
    ncmds = struct.unpack("<I", d[16:20])[0]
    p, encrypted = 28, False
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack("<II", d[p:p + 8])
        if cmd == 0x21:   # LC_ENCRYPTION_INFO
            encrypted = struct.unpack("<I", d[p + 16:p + 20])[0] != 0
        p += cmdsize
    gl = "GLES2 (shaders)" if b"_glCreateShader" in d else "GLES1 (fixed-function)" if b"_glMatrixMode" in d or b"_glVertexPointer" in d else "none"
    engine = "Unity" if b"UnityAppController" in d or b"mono_" in d or b"UnityGetGLViewController" in d else ""
    return {"archs": archs, "encrypted": encrypted, "error": None, "gl": gl, "engine": engine}

