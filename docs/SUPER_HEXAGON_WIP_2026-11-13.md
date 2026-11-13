# Super Hexagon checkpoint — superseded priority

Issue #438, branch codex/super-hexagon-compat, initially based on Canabalt #437.
The owner reprioritized the Get these working folder before this game was qualified.
This is unfinished work, not a launch/playability claim.

Fixture: Super_Hexagon_1.0.0/Payload/SuperHexagon.app outside Git. SHA256
 db3165c8150493f2ce5dad5a73827092501edb637241bdc0392dc82d856c7290.
Armv7, cryptid0, minimum iOS4.0. No IPA files or guest bytes changed.

Initial launch crashes at missing thread_get_exception_ports before any frame.
Shared local exception metadata now supports grouped task/thread get/set/swap,
checked target/mask/buffers, copied send rights and dead receive-endpoint metadata.
Approx labels explicitly state that guest Mach exception message delivery is
unavailable. No host exception rights are exposed.

Runner red:806pass/1fail; TL-AN interrupted before fix. Final metadata fulltest
807/0, Test Lab40/0. The destroyed-port case separately fails against the
pre-correction library. Other final gates were not yet run; no PR or playable
claim has been made for this batch.

Actual rerun reaches a second fault before its first frame. Runtime generated
GOT table at0x182aa70 slot+0x228 points at0x1034c228, whose contents remain zero;
code later reads the resulting null pointer+0x10 in block0xd9a0c. Read-only GDB
probes show the allocation call at guest0x6cc4 requests mmap(addr0,len0x504,
prot0,flags0x1001,fd-1,off0). System.cpp currently exposes PROT_NONE allocations
as writable/readable zero pages. This strongly suggests the runtime expects
fault-driven lazy slot initialization through its installed Mach exception
handler, which the emulator does not deliver. Confirm the complete handler
protocol before implementing memory/protection/IPC changes. A metadata fix alone
is insufficient; do not bypass the fault or synthesize class-slot values.

Evidence lives under ~/bench/runs/super-hexagon-* and ~/codex-super-hexagon-*.
The CLI private debugger scripts and extracted app remain outside the repository.
