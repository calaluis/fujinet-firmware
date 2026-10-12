#ifndef _TNFSLIB_WS_H
#define _TNFSLIB_WS_H

// TNFS over a secure WebSocket (wss://host/), for TNFS servers published through a
// WebSocket-to-TCP bridge. Each TNFS request is sent as one binary frame; replies are read
// as a byte stream and framed by tnfsStreamLink, never by WebSocket message boundaries.
//
// TLS always verifies the server: the certificate chain against the ESP-IDF certificate
// bundle and the certificate name against the host name (also sent as SNI). There is no
// fallback to an unverified connection, to plain TCP or to UDP, no redirect following, no
// automatic reconnection and no client-originated ping.

#include <memory>

#include "tnfslib_stream.h"

// A new, unconnected WSS transport, or nullptr where none is available: on PC builds,
// without CONFIG_MBEDTLS_CERTIFICATE_BUNDLE, or when memory is short.
std::unique_ptr<tnfsStreamTransport> tnfs_make_ws_transport();

#endif // _TNFSLIB_WS_H
