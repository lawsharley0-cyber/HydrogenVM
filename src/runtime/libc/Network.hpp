#pragma once
namespace Hydrogen::Runtime {
class Process;
struct Ctx;
// Socket descriptors share the existing POSIX host-fd namespace. Track their
// ownership so ordinary close/dup/dup2 and process teardown agree.
bool isNetworkSocket(Process&, int fd);
int closeNetworkSocket(Process&, int fd); // -2: not a tracked socket
void duplicateNetworkSocket(Process&, int from, int to);
int networkRead(Ctx&, int fd, unsigned int buffer, unsigned int count);
int networkWrite(Ctx&, int fd, unsigned int buffer, unsigned int count);
}
