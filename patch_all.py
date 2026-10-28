import sys

# Defaults.cpp
content = open('src/runtime/foundation/Defaults.cpp').read()
search = """    if (!v) {
        if (rt(x).respondsTo(value, rt(x).sel("data"))) {
            v = objectToPlist(x, id(rt(x).send(x.cpu, value, rt(x).sel("data"))));
        } else if (rt(x).respondsTo(value, rt(x).sel("archive"))) {
            v = objectToPlist(x, id(rt(x).send(x.cpu, value, rt(x).sel("archive"))));
        }
    }
    if (!v) {
        if (isString(x, value)) {
            v = Plist::Value::makeString(utf8(x, value));
        } else if (rt(x).respondsTo(value, rt(x).sel("string"))) {
            v = Plist::Value::makeString(utf8(x, id(rt(x).send(x.cpu, value, rt(x).sel("string")))));
        } else if (rt(x).respondsTo(value, rt(x).sel("description"))) {
            v = Plist::Value::makeString(describe(x, value));
        }
    }
    if (!v) {
        x.proc.logf(LogLevel::Warning, "NSUserDefaults setObject:forKey: ignoring non-plist object %s for key %s", describe(x, value).c_str(), K(x, key).c_str());
        return;
    }"""
replace = """    if (!v) {
        const id name = constString(x.proc, "NSInvalidArgumentException");
        const id reason = makeString(x, "Attempt to insert non-property list object " + describe(x, value) + " for key " + K(x, key));
        throwException(x, excc_with(x, rt(x).getClass("NSException"), 0, name, reason, 0));
    }"""
content = content.replace(search, replace)
open('src/runtime/foundation/Defaults.cpp', 'w').write(content)

# Foundation.hpp
content = open('src/runtime/foundation/Foundation.hpp').read()
search = """// ---- notifications (Notifications.cpp)"""
replace = """// ---- exceptions (Misc.cpp)
id excc_with(Ctx& x, id self, SEL s, id name, id reason, id userInfo);
[[noreturn]] void throwException(Ctx& x, id e);

// ---- notifications (Notifications.cpp)"""
if "throwException(Ctx" not in content:
    content = content.replace(search, replace)
open('src/runtime/foundation/Foundation.hpp', 'w').write(content)

# TestRunner.cpp
content = open('tests/runner/TestRunner.cpp').read()
search = """    // 2f7. NSUserDefaults non-plist object handling
    {
        const ObjC::Class nsoCls = r.getClass("NSObject");
        const ObjC::id nso = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, nsoCls, r.selAlloc)), r.selInit));
        const ObjC::Class udCls = r.getClass("NSUserDefaults");
        const ObjC::id ud = ObjC::id(r.send(cpu, udCls, r.sel("standardUserDefaults")));
        const ObjC::id testKey = Runtime::Foundation::makeString(cx, "testKeyNonPlist");
        r.send(cpu, ud, r.sel("setObject:forKey:"), {uint32_t(nso), uint32_t(testKey)});

        // ensure it was translated to description (fallback)
        const ObjC::id val = ObjC::id(r.send(cpu, ud, r.sel("objectForKey:"), {uint32_t(testKey)}));
        const std::string desc = Runtime::Foundation::utf8(cx, val);
        check(desc.find("NSObject") != std::string::npos, "NSUserDefaults setObject:forKey: fallback on description for non-plist objects");

        // synchronize and test round trip for supported type
        const ObjC::id validKey = Runtime::Foundation::makeString(cx, "testKeyPlist");
        const ObjC::id validString = Runtime::Foundation::makeString(cx, "validStringValue");
        r.send(cpu, ud, r.sel("setObject:forKey:"), {uint32_t(validString), uint32_t(validKey)});
        r.send(cpu, ud, r.sel("synchronize"));

        const ObjC::id validVal = ObjC::id(r.send(cpu, ud, r.sel("objectForKey:"), {uint32_t(validKey)}));
        const std::string validDesc = Runtime::Foundation::utf8(cx, validVal);
        check(validDesc == "validStringValue", "NSUserDefaults setObject:forKey: preserves supported types across round-trip");
    }"""
replace = """    // 2f7. NSUserDefaults non-plist object handling
    {
        const ObjC::Class nsoCls = r.getClass("NSObject");
        const ObjC::id nso = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, nsoCls, r.selAlloc)), r.selInit));
        const ObjC::Class udCls = r.getClass("NSUserDefaults");
        const ObjC::id ud = ObjC::id(r.send(cpu, udCls, r.sel("standardUserDefaults")));
        const ObjC::id testKey = Runtime::Foundation::makeString(cx, "testKeyNonPlist");

        bool threw = false;
        try {
            r.send(cpu, ud, r.sel("setObject:forKey:"), {uint32_t(nso), uint32_t(testKey)});
        } catch (const Process::GuestThrow&) {
            threw = true;
        }
        check(threw, "NSUserDefaults setObject:forKey: raises NSInvalidArgumentException for non-plist objects");

        // synchronize and test round trip for supported type
        const ObjC::id validKey = Runtime::Foundation::makeString(cx, "testKeyPlist");

        // create a nested array
        const ObjC::id validString = Runtime::Foundation::makeString(cx, "validStringValue");
        const ObjC::id validArray = Runtime::Foundation::makeArray(cx, {validString});
        const ObjC::id validDict = Runtime::Foundation::makeDict(cx, {{validString, validArray}});
        r.send(cpu, ud, r.sel("setObject:forKey:"), {uint32_t(validDict), uint32_t(validKey)});
        r.send(cpu, ud, r.sel("synchronize"));

        const ObjC::id validVal = ObjC::id(r.send(cpu, ud, r.sel("objectForKey:"), {uint32_t(validKey)}));
        const ObjC::id validArrBack = Runtime::Foundation::dictGet(cx, validVal, validString);
        const ObjC::id validStrBack = Runtime::Foundation::arrayItems(cx, validArrBack)[0];
        const std::string validDesc = Runtime::Foundation::utf8(cx, validStrBack);
        check(validDesc == "validStringValue", "NSUserDefaults setObject:forKey: preserves supported types across round-trip");
    }"""
content = content.replace(search, replace)
open('tests/runner/TestRunner.cpp', 'w').write(content)
