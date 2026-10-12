#ifndef TNFS_HARNESS_FNUDP_H
#define TNFS_HARNESS_FNUDP_H

// Test stand-in: every call fails and is counted in harness_udp_calls.

#include <cstddef>
#include <cstdint>

#include "compat_inet.h"

extern int harness_udp_calls;

class fnUDP
{
public:
    bool beginPacket(in_addr_t, uint16_t) { return ++harness_udp_calls, false; }
    bool beginPacket(const char *, uint16_t) { return ++harness_udp_calls, false; }
    bool endPacket() { return ++harness_udp_calls, false; }
    size_t write(const uint8_t *, size_t) { return ++harness_udp_calls, 0; }
    int parsePacket() { return ++harness_udp_calls, 0; }
    int read(unsigned char *, size_t) { return ++harness_udp_calls, -1; }
    in_addr_t remoteIP() { return IPADDR_NONE; }
};

#endif
