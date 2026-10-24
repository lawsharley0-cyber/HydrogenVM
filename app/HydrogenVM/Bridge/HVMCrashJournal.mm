#import "HVMEngine.h"
#include "core/cpu/Engine.hpp"
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <os/proc.h>

namespace {
volatile sig_atomic_t signalFD = -1;
int hostFaultFD = -1;   // Engine writes host-fault diagnostics (log tail, guest threads) here
void hostFailure(int signal) {
    // No Foundation, allocation, locks, backtrace, or guest state in signal context.
    // The shell has already persisted a bounded session snapshot alongside this file.
    static const char segv[] = "{\"exceptionKind\":\"host-SIGSEGV\"}\n";
    static const char bus[] = "{\"exceptionKind\":\"host-SIGBUS\"}\n";
    const int fd = signalFD;
    if (fd >= 0) {
        if (signal == SIGBUS) write(fd, bus, sizeof(bus) - 1);
        else write(fd, segv, sizeof(segv) - 1);
        fsync(fd);
    }
    _exit(128 + signal);
}
}
@implementation HVMCrashJournal
+ (void)installSignalHandlers {
    // Install before Engine's fault handler, which chains non-guest faults here.
    struct sigaction action = {};
    action.sa_handler = hostFailure;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
}
+ (void)openSignalFile:(NSString *)path {
    [self closeSignalFile];
    signalFD = open(path.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    // Beside it, "<name>.hostfault.txt" (pending.signal.json -> pending.hostfault.txt)
    // receives the emulator's own diagnostics when the host process faults.
    NSString *faultPath = [[path stringByDeletingPathExtension] stringByDeletingPathExtension];
    faultPath = [faultPath stringByAppendingString:@".hostfault.txt"];
    hostFaultFD = open(faultPath.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (hostFaultFD >= 0) Hydrogen::CPU::Engine::setFaultReportFd(hostFaultFD);
}
+ (void)closeSignalFile {
    const int fd = signalFD;
    signalFD = -1;
    if (fd >= 0) close(fd);
    Hydrogen::CPU::Engine::setFaultReportFd(-1);
    if (hostFaultFD >= 0) close(hostFaultFD);
    hostFaultFD = -1;
}
+ (uint64_t)availableMemory { return os_proc_available_memory(); }
@end
