#ifndef _TNFSLIB_WSS_H
#define _TNFSLIB_WSS_H

// TNFS over a secure WebSocket (tnfsw:// host slots), for TNFS servers published through a
// WebSocket-to-TCP bridge. Each TNFS request is sent as one binary frame; replies are read as
// a byte stream and framed by tnfsStreamLink, never by WebSocket message boundaries.
//
// TLS always verifies the server: the certificate chain against the ESP-IDF certificate
// bundle, the certificate name against the host name (also sent as SNI), and the validity
// dates against a clock set by SNTP. There is no fallback to an unverified connection, to
// plain TCP or to UDP, no redirect following and no reconnection of its own: the next request
// reconnects. No WebSocket ping of ours either; idle mounts are kept alive with TNFS requests
// (tnfslib_keep_alive.h).

#include "tnfslibMountInfo.h"

// A tnfsw:// mount's budgets: a 2 s reply wait per attempt (connecting gets
// tnfsWsStream::CONNECT_TIMEOUT_MS, 5 s) and three attempts. There is no overall limit, so a
// server that accepts connections slowly can take longer.
constexpr int TNFS_WSS_TIMEOUT = 2000;
constexpr uint8_t TNFS_WSS_RETRIES = 3;

// A new, unconnected WSS transport that upgrades at `path`, or nullptr where none is
// available: on PC builds, without CONFIG_MBEDTLS_CERTIFICATE_BUNDLE, or when memory is short.
tnfsStreamTransportPtr tnfs_new_wss_stream(const char *path);

#endif // _TNFSLIB_WSS_H
