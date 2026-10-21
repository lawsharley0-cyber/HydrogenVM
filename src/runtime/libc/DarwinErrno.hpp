#pragma once
#include <cerrno>
namespace Hydrogen::Runtime {
inline int darwinErrno(int e) {
    switch (e) {
        case EAGAIN: return 35;
        case EINPROGRESS: return 36;
        case EALREADY: return 37;
        case ENOTSOCK: return 38;
        case EDESTADDRREQ: return 39;
        case EMSGSIZE: return 40;
        case EPROTOTYPE: return 41;
        case ENOPROTOOPT: return 42;
        case EPROTONOSUPPORT: return 43;
        case EOPNOTSUPP: return 45;
        case EAFNOSUPPORT: return 47;
        case EADDRINUSE: return 48;
        case EADDRNOTAVAIL: return 49;
        case ENETDOWN: return 50;
        case ENETUNREACH: return 51;
        case ENETRESET: return 52;
        case ECONNABORTED: return 53;
        case ECONNRESET: return 54;
        case ENOBUFS: return 55;
        case EISCONN: return 56;
        case ENOTCONN: return 57;
        case ESHUTDOWN: return 58;
        case ETIMEDOUT: return 60;
        case ECONNREFUSED: return 61;
        case ELOOP: return 62;
        case ENAMETOOLONG: return 63;
        case EHOSTUNREACH: return 65;
        case ENOTEMPTY: return 66;
        case ENOSYS: return 78;
        case EOVERFLOW: return 84;
        default: return e;
    }
}
}
