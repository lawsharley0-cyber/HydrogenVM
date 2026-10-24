// Opt-in native entry tracing without changing guest bytes or return values.
// HVM_ENTRY_TRACE=path HVM_ENTRY_PCS=hex,hex HVM_ENTRY_AFTER=hex (optional gate)
// HVM_ENTRY_BUFFER=pointerOffset,indexOffset optionally follows a buffer at r0.
#pragma once
#include "runtime/Process.hpp"
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <set>

class EntryTrace {
public:
    void attach(Hydrogen::Runtime::Process& process) {
        const char* path = std::getenv("HVM_ENTRY_TRACE");
        if (!path) return;
        const char* pcs = std::getenv("HVM_ENTRY_PCS");
        if (!pcs || !*pcs) throw std::runtime_error("HVM_ENTRY_TRACE requires HVM_ENTRY_PCS");
        auto hex = [](const std::string& value) {
            size_t end = 0;
            const unsigned long number = std::stoul(value, &end, 16);
            if (end != value.size() || number > UINT32_MAX) throw std::runtime_error("Invalid entry trace address");
            return uint32_t(number);
        };
        std::set<uint32_t> entries;
        std::stringstream input(pcs); std::string part;
        while (std::getline(input, part, ',')) entries.insert(hex(part) & ~1u);
        uint32_t gate = 0;
        if (const char* value = std::getenv("HVM_ENTRY_AFTER")) gate = hex(value) & ~1u;
        if (gate) entries.insert(gate);
        active_ = !gate;
        uint32_t pointerOffset = 0, indexOffset = 0;
        const char* buffer = std::getenv("HVM_ENTRY_BUFFER");
        if (buffer) {
            const std::string spec(buffer);
            const size_t comma = spec.find(',');
            if (comma == std::string::npos) throw std::runtime_error("HVM_ENTRY_BUFFER requires two hexadecimal offsets");
            pointerOffset = hex(spec.substr(0, comma)); indexOffset = hex(spec.substr(comma + 1));
            if (pointerOffset > 252 || indexOffset > 252) throw std::runtime_error("Entry trace offsets exceed the 256-byte observation window");
        }
        out_.open(path, std::ios::out | std::ios::trunc);
        if (!out_) throw std::runtime_error("Cannot open entry trace file");
        for (uint32_t pc : entries) process.engine().observeEntry(pc,
            [this, &process, gate, buffer = bool(buffer), pointerOffset, indexOffset](uint32_t address, const Hydrogen::CPU::Cpu& cpu) {
                auto& mem = process.mem();
                std::lock_guard<std::mutex> lock(mutex_);
                if (address == gate) active_ = true;
                if (!active_) return;
                if (++count_ > 100000) { if (count_ == 100001) out_ << "trace limit reached\n"; return; }
                char text[32];
                auto readable = [&](uint64_t base, size_t size) {
                    if (base + size > Hydrogen::Memory::GuestArena::kArenaSize) return false;
                    for (size_t i = 0; i < size; ++i)
                        if (!(mem.protAt(uint32_t(base + i)) & Hydrogen::Memory::kProtRead)) return false;
                    return true;
                };
                auto field = [&](const char* name, uint32_t value) {
                    std::snprintf(text, sizeof text, " %s=%08x", name, value); out_ << text;
                };
                auto bytes = [&](uint64_t base, size_t size) {
                    if (!readable(base, size)) {
                        out_ << "unmapped"; return;
                    }
                    for (size_t i = 0; i < size; ++i) {
                        std::snprintf(text, sizeof text, "%02x", mem.read<uint8_t>(uint32_t(base + i))); out_ << text;
                    }
                };
                out_ << "entry"; field("pc", address); field("lr", cpu.r[14]);
                for (int i = 0; i < 13; ++i) { const auto name = "r" + std::to_string(i); field(name.c_str(), cpu.r[i]); }
                out_ << " mem="; bytes(cpu.r[0], 32);
                const uint64_t a = uint64_t(cpu.r[0]) + pointerOffset, b = uint64_t(cpu.r[0]) + indexOffset;
                if (buffer && a <= UINT32_MAX && b <= UINT32_MAX &&
                    readable(a, 4) && readable(b, 4)) {
                    const uint32_t pointer = mem.read<uint32_t>(uint32_t(a)), index = mem.read<uint32_t>(uint32_t(b));
                    field("index", index); out_ << " buffer="; bytes(uint64_t(pointer) + index, 32);
                }
                out_ << '\n'; out_.flush();
                if (!out_) throw std::runtime_error("Entry trace write failed");
            });
    }
private:
    std::ofstream out_;
    std::mutex mutex_;
    bool active_ = false;
    size_t count_ = 0;
};
