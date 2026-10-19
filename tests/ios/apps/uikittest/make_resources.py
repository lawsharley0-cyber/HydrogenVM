#!/usr/bin/env python3
"""Generate the UIKit test app's resources: PNGs (normal, @2x, Xcode-crushed
CgBI) and the same nib in both compiled formats (NIBArchive and keyed plist).

usage: make_resources.py OUTDIR
"""
import os, plistlib, struct, sys, zlib

def png(w, h, rgba_fn, cgbi=False):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            r, g, b, a = rgba_fn(x, y)
            if cgbi:   # BGRA, premultiplied
                r, g, b = r * a // 255, g * a // 255, b * a // 255
                raw += bytes([b, g, r, a])
            else:
                raw += bytes([r, g, b, a])
    def chunk(t, body):
        c = struct.pack(">I", len(body)) + t + body
        return c + struct.pack(">I", zlib.crc32(t + body) & 0xFFFFFFFF)
    out = b"\x89PNG\r\n\x1a\n"
    if cgbi:
        out += chunk(b"CgBI", struct.pack(">I", 0x50002002))
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    comp = zlib.compressobj(9, zlib.DEFLATED, -15 if cgbi else 15)
    data = comp.compress(bytes(raw)) + comp.flush()
    out += chunk(b"IDAT", data) + chunk(b"IEND", b"")
    return out

# ----------------------------------------------------------------- nibs ---
# A tiny object model shared by both encoders.
class Obj:
    def __init__(self, cls, **values):
        self.cls, self.values = cls, list(values.items())

def build_panel():
    owner_id = Obj("NSString", s="IBFilesOwner")
    owner = Obj("UIProxyObject", UIProxiedObjectIdentifier=owner_id)
    title = Obj("UIButtonContent", UITitle=Obj("NSString", s="Nib Button"))
    content = Obj("NSDictionary", pairs=[(Obj("NSNumber", i=0), title)])
    label = Obj("UILabel", UIBounds=("rect", 0, 0, 180, 30), UICenter=("point", 100, 25), UIText=Obj("NSString", s="From nib"),
                UITextAlignment=1, UITag=7)
    button = Obj("UIButton", UIBounds=("rect", 0, 0, 120, 37), UICenter=("point", 100, 70), UIButtonType=1,
                 UIButtonStatefulContent=content)
    color = Obj("UIColor", UIRed=("float", 0.2), UIGreen=("float", 0.4), UIBlue=("float", 0.8), UIAlpha=("float", 1.0))
    view = Obj("UIView", UIBounds=("rect", 0, 0, 200, 100), UICenter=("point", 160, 120), UIBackgroundColor=color,
               UISubviews=Obj("NSArray", items=[label, button]), UITag=42)
    outlet = Obj("UIRuntimeOutletConnection", UILabel=Obj("NSString", s="panelLabel"), UISource=owner, UIDestination=label)
    action = Obj("UIRuntimeEventConnection", UILabel=Obj("NSString", s="nibButtonTapped:"), UISource=button, UIDestination=owner,
                 UIEventMask=64)
    root = Obj("NSObject", UINibTopLevelObjectsKey=Obj("NSArray", items=[owner, view]),
               UINibObjectsKey=Obj("NSArray", items=[owner, view, label, button]),
               UINibConnectionsKey=Obj("NSArray", items=[outlet, action]))
    return root

def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n == 0:
            out.append(b | 0x80)
            return bytes(out)
        out.append(b)

def nibarchive(root):
    objects, index = [], {}
    def visit(o):
        if id(o) in index: return index[id(o)]
        index[id(o)] = len(objects)
        objects.append(o)
        for k, v in o.values:
            if isinstance(v, Obj): visit(v)
            if k == "items": [visit(i) for i in v]
            if k == "pairs": [(visit(a), visit(b)) for a, b in v]
        return index[id(o)]
    visit(root)
    keys, classes, values, objtab = [], [], [], []
    def key(k):
        if k not in keys: keys.append(k)
        return keys.index(k)
    def cls(c):
        if c not in classes: classes.append(c)
        return classes.index(c)
    def val(k, v):
        kb = varint(key(k))
        if isinstance(v, Obj): return kb + bytes([10]) + struct.pack("<I", index[id(v)])
        if isinstance(v, bool): return kb + bytes([4 if v else 5])
        if isinstance(v, int): return kb + bytes([2]) + struct.pack("<i", v)
        if isinstance(v, tuple) and v[0] == "float": return kb + bytes([6]) + struct.pack("<f", v[1])
        if isinstance(v, tuple) and v[0] in ("rect", "point"):
            data = bytes([6]) + struct.pack("<" + "f" * (len(v) - 1), *v[1:])
            return kb + bytes([8]) + varint(len(data)) + data
        if isinstance(v, bytes): return kb + bytes([8]) + varint(len(v)) + v
        raise ValueError(v)
    for o in objects:
        vs = []
        for k, v in o.values:
            if o.cls in ("NSString",) and k == "s": vs.append(val("NS.bytes", v.encode()))
            elif o.cls == "NSNumber" and k == "i": vs.append(val("NS.intval", v))
            elif k == "items": vs += [val("UINibEncoderEmptyKey", i) for i in v]
            elif k == "pairs":
                for a, b in v: vs += [val("UINibEncoderEmptyKey", a), val("UINibEncoderEmptyKey", b)]
            else: vs.append(val(k, v))
        objtab.append((cls(o.cls), len(values), len(vs)))
        values += vs
    header_len = 10 + 8 + 32
    obj_bytes = b"".join(varint(c) + varint(s) + varint(n) for c, s, n in objtab)
    key_bytes = b"".join(varint(len(k.encode())) + k.encode() for k in keys)
    val_bytes = b"".join(values)
    cls_bytes = b"".join(varint(len(c) + 1) + varint(0) + c.encode() + b"\0" for c in classes)
    off = header_len
    o_off, k_off = off, off + len(obj_bytes)
    v_off = k_off + len(key_bytes)
    c_off = v_off + len(val_bytes)
    hdr = b"NIBArchive" + struct.pack("<II", 1, 9) + struct.pack("<8I", len(objects), o_off, len(keys), k_off, len(values), v_off, len(classes), c_off)
    return hdr + obj_bytes + key_bytes + val_bytes + cls_bytes

def keyed(root):
    objs = ["$null"]
    index, classes = {}, {}
    supers = {"UILabel": ["UILabel", "UIView", "UIResponder", "NSObject"], "UIButton": ["UIButton", "UIControl", "UIView", "UIResponder", "NSObject"],
              "UIView": ["UIView", "UIResponder", "NSObject"]}
    def classref(c):
        if c not in classes:
            classes[c] = len(objs)
            objs.append({"$classname": c, "$classes": supers.get(c, [c, "NSObject"])})
        return plistlib.UID(classes[c])
    def enc(v):
        if isinstance(v, Obj): return plistlib.UID(visit(v))
        if isinstance(v, tuple) and v[0] == "float": return float(v[1])
        if isinstance(v, tuple) and v[0] == "rect": return "{{%g, %g}, {%g, %g}}" % v[1:]
        if isinstance(v, tuple) and v[0] == "point": return "{%g, %g}" % v[1:]
        return v
    def visit(o):
        if id(o) in index: return index[id(o)]
        if o.cls == "NSString":
            index[id(o)] = len(objs); objs.append(dict(o.values)["s"]); return index[id(o)]
        if o.cls == "NSNumber":
            index[id(o)] = len(objs); objs.append(dict(o.values)["i"]); return index[id(o)]
        me = len(objs)
        index[id(o)] = me
        d = {}
        objs.append(d)
        for k, v in o.values:
            if k == "items": d["NS.objects"] = [plistlib.UID(visit(i)) for i in v]
            elif k == "pairs":
                d["NS.keys"] = [plistlib.UID(visit(a)) for a, _ in v]
                d["NS.objects"] = [plistlib.UID(visit(b)) for _, b in v]
            else: d[k] = enc(v)
        d["$class"] = classref(o.cls)
        return me
    top = {}
    for k, v in root.values: top[k] = plistlib.UID(visit(v))
    return plistlib.dumps({"$archiver": "NSKeyedArchiver", "$version": 100000, "$top": top, "$objects": objs}, fmt=plistlib.FMT_BINARY)

def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    green = lambda x, y: (0, 200, 0, 255)
    open(os.path.join(out, "test.png"), "wb").write(png(40, 20, green))
    open(os.path.join(out, "test@2x.png"), "wb").write(png(80, 40, green))
    # 2x2 crushed PNG: red, green / blue, half-transparent white
    px = {(0, 0): (255, 0, 0, 255), (1, 0): (0, 255, 0, 255), (0, 1): (0, 0, 255, 255), (1, 1): (255, 255, 255, 128)}
    open(os.path.join(out, "crushed.png"), "wb").write(png(2, 2, lambda x, y: px[(x, y)], cgbi=True))
    open(os.path.join(out, "Panel.nib"), "wb").write(nibarchive(build_panel()))
    open(os.path.join(out, "PanelKeyed.nib"), "wb").write(keyed(build_panel()))

if __name__ == "__main__":
    main()
