// HydrogenVM — 32-bit ARM Mach-O loader.
//
// Loads an armv6/armv7/armv7s Mach-O image (picking the best slice of a fat
// binary) into guest memory, applies rebases, and collects every symbol
// import as a BindRecord for the runtime to resolve. Both the iOS 4.3+
// "dyld info" format (LC_DYLD_INFO opcodes) and the older indirect-symbol-
// table / external-relocation format are supported, since 32-bit iOS apps
// span both eras.
#pragma once
#include "core/memory/GuestArena.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <utility>

namespace Hydrogen::Loader {

struct MachOError : std::runtime_error { using std::runtime_error::runtime_error; };

// Thrown when the binary is FairPlay-encrypted (cryptid != 0). Store
// downloads are encrypted and can't run anywhere but the device they were
// bought for; the user needs a decrypted copy.
struct EncryptedBinary : MachOError { using MachOError::MachOError; };

struct Segment {
    std::string name;
    uint32_t vmaddr = 0, vmsize = 0, fileoff = 0, filesize = 0;
    uint32_t maxprot = 0, initprot = 0;
};

struct Section {
    std::string segname, sectname;
    uint32_t addr = 0, size = 0, offset = 0, flags = 0, reserved1 = 0, reserved2 = 0;
    uint32_t type() const { return flags & 0xFF; }
};

enum class BindKind : uint8_t { Pointer = 1, TextAbsolute32 = 2, TextPcrel32 = 3, ExternalReloc = 0x10 };

struct BindRecord {
    uint32_t    address = 0;     // guest address to patch
    std::string symbol;          // with leading underscore, as in the file
    int32_t     addend = 0;
    int32_t     dylibOrdinal = 0; // 1-based index into Image::dylibs, or special (0 self, -1 main, -2 flat)
    BindKind    kind = BindKind::Pointer;
    bool        weakImport = false;
    bool        lazy = false;
};

// Shared with the ObjC runtime; snapshots remain valid during service teardown.
class MethodSymbols {
public:
    void set(uint64_t binding, uint32_t imp, std::string name);
    std::string describe(uint32_t address) const;
private:
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, uint32_t> bindings_;
    std::map<uint32_t, std::map<uint64_t, std::string>> names_;
};

struct Image {
    std::string path;
    uint32_t cpuSubtype = 0;
    uint32_t fileType = 0;        // 2 = MH_EXECUTE, 6 = MH_DYLIB, 8 = MH_BUNDLE
    uint32_t flags = 0;
    uint32_t slide = 0;
    uint32_t headerAddr = 0;      // guest address of the mach_header
    uint32_t lowAddr = 0, highAddr = 0;

    std::vector<Segment> segments;
    std::vector<Section> sections;
    std::vector<std::string> dylibs;   // LC_LOAD_DYLIB order (ordinal = index + 1)
    std::string installName;           // LC_ID_DYLIB for dylibs

    // Entry point (bit 0 set for Thumb). For LC_MAIN binaries this is main();
    // for LC_UNIXTHREAD binaries it's the crt1 `start` routine, which expects
    // the kernel's initial stack layout.
    uint32_t entry = 0;
    bool     entryIsMain = false;
    uint32_t stackSize = 0;            // from LC_MAIN, 0 = default

    std::vector<uint32_t>  initializers; // __mod_init_func pointers, in order
    std::vector<BindRecord> binds;
    std::vector<BindRecord> weakBinds;   // coalesced symbols (weak_bind table)
    uint32_t weakBindCount = 0;

    // Defined symbols: exported (N_EXT) by name, and every named symbol by
    // address for crash reports. Thumb functions carry bit 0.
    std::unordered_map<std::string, uint32_t> exports;
    std::map<uint32_t, std::string> symbolsByAddr;
    std::vector<uint32_t> functionStarts; // LC_FUNCTION_STARTS, slid and Thumb-normalized
    mutable std::shared_ptr<MethodSymbols> methodSymbols;

    const Section* findSection(const char* seg, const char* sect) const;
    std::vector<const Section*> sectionsOfType(uint32_t type) const;
    // "name + 0x1c" for an address, or empty if unknown.
    std::string symbolicate(uint32_t addr) const;
};

// Mach-O CPU subtypes for ARM.
enum : uint32_t { kCpuArmV6 = 6, kCpuArmV7 = 9, kCpuArmV7F = 10, kCpuArmV7S = 11, kCpuArmV7K = 12 };

struct LoadOptions {
    // Load at this address instead of the preferred one (0 = preferred).
    // Only PIE executables and dylibs can be moved.
    uint32_t loadAddress = 0;
};

// Pick the slice of a (possibly fat) Mach-O file to use. Returns its offset
// and size; throws if there is no 32-bit ARM slice.
struct Slice { uint32_t offset, size, cpuSubtype; };
Slice selectSlice(const std::vector<uint8_t>& file);

// Parse and load the image into guest memory.
Image loadMachO(Memory::GuestArena& mem, const std::vector<uint8_t>& file,
                const std::string& path, const LoadOptions& opts = {});

// Cheap check used by the importer: is this file a loadable 32-bit ARM
// Mach-O, and is it encrypted?
struct ProbeResult { bool isMachO = false, hasArm32 = false, encrypted = false; uint32_t cpuSubtype = 0; std::string error; };
ProbeResult probeMachO(const std::vector<uint8_t>& file);

} // namespace Hydrogen::Loader
