#ifndef TNFS_HARNESS_FNTCPCLIENT_H
#define TNFS_HARNESS_FNTCPCLIENT_H

// Test stand-in: never connects; every connect is counted in harness_tcp_connects.

#include <cstddef>
#include <cstdint>

#include "compat_inet.h"

extern int harness_tcp_connects;

class fnTcpClient
{
public:
    uint8_t connected() { return 0; }
    int connect(const char *, uint16_t, int32_t = -1) { return ++harness_tcp_connects, 0; }
    int connect(in_addr_t, uint16_t, int32_t = -1) { return ++harness_tcp_connects, 0; }
    size_t write(const uint8_t *, size_t) { return 0; }
    size_t available() { return 0; }
    int read(uint8_t *, size_t) { return -1; }
    in_addr_t remoteIP() const { return IPADDR_NONE; }
};

#endif
