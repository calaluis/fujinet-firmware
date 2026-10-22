#ifndef _TNFSLIB_WSS_H
#define _TNFSLIB_WSS_H

// TNFS over a secure WebSocket (tnfsw:// host slots), through a WebSocket-to-TCP bridge.
// TLS checks the chain against the ESP-IDF bundle, the host name, and the dates (SNTP).
// No fallback to an unverified connection, unencrypted TNFS-over-TCP or UDP.

#include "tnfslibMountInfo.h"

// A tnfsw:// mount's budgets: 2 s per reply wait (5 s to connect) and three attempts, with
// no overall limit.
constexpr int TNFS_WSS_TIMEOUT = 2000;
constexpr uint8_t TNFS_WSS_RETRIES = 3;

// A new, unconnected WSS transport that upgrades at `path`, or nullptr where none is
// available: on PC builds, without CONFIG_MBEDTLS_CERTIFICATE_BUNDLE, or when memory is short.
tnfsStreamTransportPtr tnfs_new_wss_stream(const char *path);

#endif // _TNFSLIB_WSS_H
