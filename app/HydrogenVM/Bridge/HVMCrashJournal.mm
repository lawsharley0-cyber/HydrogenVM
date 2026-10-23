#import "HVMEngine.h"
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <os/proc.h>

namespace {
volatile sig_atomic_t signalFD = -1;
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
}
+ (void)closeSignalFile {
    const int fd = signalFD;
    signalFD = -1;
    if (fd >= 0) close(fd);
}
+ (uint64_t)availableMemory { return os_proc_available_memory(); }
@end
