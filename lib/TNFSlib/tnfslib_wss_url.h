#ifndef _TNFSLIB_WSS_URL_H
#define _TNFSLIB_WSS_URL_H

// Host slots of the form tnfsw://host[:port][/path]: TNFS over a secure WebSocket. The path is
// the WebSocket endpoint on the bridge in front of the TNFS server, not a TNFS directory.

#include <cstdint>

constexpr uint16_t TNFS_WSS_DEFAULT_PORT = 443;
constexpr int TNFS_WSS_HOST_SIZE = 64; // tnfsMountInfo::hostname
constexpr int TNFS_WSS_PATH_SIZE = 32; // a host slot holds 31 characters in all

enum class tnfsWssUrlResult
{
    NOT_WSS, // not a tnfsw:// URL; the caller treats it as an ordinary TNFS host
    VALID,
    INVALID, // a tnfsw:// URL that cannot be used; never fall back to another transport
};

struct tnfsWssUrl
{
    char host[TNFS_WSS_HOST_SIZE];
    uint16_t port;
    char path[TNFS_WSS_PATH_SIZE];
};

// Rejects an empty host, user:password@, IPv6 literals, a port outside 1..65535, a query or a
// fragment, and anything that does not fit `out`. `out` is only written when VALID.
tnfsWssUrlResult tnfs_parse_wss_url(const char *text, tnfsWssUrl &out);

#endif // _TNFSLIB_WSS_URL_H
