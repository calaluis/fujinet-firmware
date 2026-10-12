#include "tnfslib_host.h"

#include <cstring>

#include "tnfslibMountInfo.h"

tnfsHostSpec tnfs_parse_host(const char *name, uint16_t port)
{
    tnfsHostSpec spec = {false, false, TNFS_PROTOCOL_UNKNOWN, name, port};
    if (name == nullptr)
        return spec;

    static const struct
    {
        const char *prefix;
        uint8_t protocol;
    } prefixes[] = {
        {"_tcp.", TNFS_PROTOCOL_TCP},
        {"_udp.", TNFS_PROTOCOL_UDP},
        {"_wss.", TNFS_PROTOCOL_WS},
    };
    for (const auto &p : prefixes)
    {
        size_t len = strlen(p.prefix);
        if (strncmp(p.prefix, name, len) == 0)
        {
            spec.forced = true;
            spec.protocol = p.protocol;
            spec.host = name + len;
            break;
        }
    }

    if (spec.protocol == TNFS_PROTOCOL_WS && port == TNFS_DEFAULT_PORT)
        spec.port = TNFS_WS_DEFAULT_PORT;
    spec.valid = spec.host[0] != '\0';
    return spec;
}
