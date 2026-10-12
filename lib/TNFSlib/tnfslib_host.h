#ifndef _TNFSLIB_HOST_H
#define _TNFSLIB_HOST_H

// What a TNFS host-slot name asks for. "_tcp.", "_udp." and "_wss." force a transport;
// a name without a prefix lets TNFS pick (TCP first, then UDP).

#include <cstdint>

struct tnfsHostSpec
{
    bool valid;       // false for nullptr, an empty name, or a prefix with no host after it
    bool forced;      // a prefix chose the protocol
    uint8_t protocol; // TNFS_PROTOCOL_* when forced, otherwise TNFS_PROTOCOL_UNKNOWN
    const char *host; // the host name, pointing into the input after any prefix
    uint16_t port;    // `port`, except 443 for "_wss." when the default TNFS port was given
};

// Pure: nothing is resolved or allocated. When `valid` is false the other fields are not
// meaningful, and callers must leave their mount state untouched.
tnfsHostSpec tnfs_parse_host(const char *name, uint16_t port);

#endif // _TNFSLIB_HOST_H
