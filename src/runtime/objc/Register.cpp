#include "ObjC.hpp"

namespace Hydrogen::Runtime {

void registerObjCRuntime(Process& p) {
    ObjC::Runtime& r = ObjC::rt(p);
    ObjC::registerNSObject(r);
    ObjC::registerBlocks(p, r);
    ObjC::registerObjCApi(p, r);
    // Imports of framework classes HydrogenVM doesn't implement get stub classes,
    // so the app links and only fails (with a clear message) if it uses them.
    p.addMissingSymbolResolver([&p](const std::string& name) -> uint32_t {
        static const std::string cls = "_OBJC_CLASS_$_", meta = "_OBJC_METACLASS_$_";
        ObjC::Runtime& r = ObjC::rt(p);
        if (name.compare(0, cls.size(), cls) == 0) return r.stubClass(name.substr(cls.size()));
        if (name.compare(0, meta.size(), meta) == 0) return r.classOf(r.stubClass(name.substr(meta.size())));
        return 0;
    });
    p.onLoaded([&p] { ObjC::rt(p).registerImage(p.image()); });
    p.onBeforeInitializers([&p](CPU::Cpu& c) { ObjC::rt(p).callLoadMethods(c); });
}

} // namespace Hydrogen::Runtime
