# lib/TNFSlib

Client library for TNFS, the Trivial Network File System that FujiNet hosts have spoken since the
beginning.

## Layout
| File | Defines |
|---|---|
| `tnfs-protocol.md` | the protocol specification |
| `tnfslib.h` | the C-style API used by every consumer |
| `tnfslib.cpp` | `tnfs_mount`/`tnfs_umount`, `tnfs_open`/`tnfs_close`/`tnfs_read`/`tnfs_write`/`tnfs_lseek`; read caching |
| `tnfslib_dir.cpp` | `tnfs_opendirx`/`tnfs_readdirx` and the directory cache, `tnfs_mkdir`/`tnfs_rmdir`, `tnfs_stat`, `tnfs_unlink`, `tnfs_rename`, `tnfs_chmod`, `tnfs_size`/`tnfs_free`, `tnfs_chdir`/`tnfs_getcwd` |
| `tnfslib_transport.cpp` | the UDP and TCP transport code, the request/response transaction with retries, and session recovery |
| `tnfslib_ws_frames.h`, `tnfslib_ws_frames.cpp` | `tnfsWsFrameTracker`: what one read from an RFC 6455 WebSocket means for the TNFS byte stream (data, PING, PONG, CLOSE, or a lost frame) |
| `tnfslib_ws_stream.h`, `tnfslib_ws_stream.cpp` | `tnfsWsStream`: `tnfsStreamTransport` over a WebSocket, behind the `tnfsWsIo` port; one binary frame per request, replies read as a stream |
| `tnfslib_wss.h`, `tnfslib_wss.cpp` | `tnfs_new_wss_stream()`: `tnfsWsStream` on ESP-IDF `esp_transport_ws` over verified TLS (certificate bundle, host name, validity dates); `nullptr` on PC; the `tnfsw://` budgets |
| `tnfslib_wss_url.h`, `tnfslib_wss_url.cpp` | `tnfs_parse_wss_url()`: `tnfsw://host[:port][/path]` host slots |
| `tnfslib_cert_time.h`, `tnfslib_cert_time.cpp` | `tnfs_cert_validity()`: a certificate's validity dates against an SNTP-set clock |
| `tnfslib_keep_alive.h`, `tnfslib_keep_alive.cpp` | `tnfs_keep_alive()` and `tnfsKeepAliveRegistry`: `tnfsw://` keep-alives run by the main loop, one request per pass at most, never connecting and never waiting for a busy mount |
| `tnfslib_codes.cpp` | packet debug dumps, command and result-code names, `tnfs_code_to_errno` |
| `tnfslib_internal.h` | declarations shared by the `tnfslib*.cpp` files; not part of the public API |
| `tnfslibMountInfo.h`, `tnfslibMountInfo.cpp` | `tnfsMountInfo` (host, session, `TNFS_PROTOCOL_TCP` or `TNFS_PROTOCOL_UDP`, the stream transport, time of the last reply, open handles, directory cache) and `tnfsStat` |
| `tnfslib_udp.h`, `tnfslib_udp_testing.cpp` | the UDP send and receive hooks, with a replacement that simulates packet loss and duplication when `TNFS_UDP_SIMULATE_POOR_CONNECTION` is defined |
| `tnfslib_framing.h`, `tnfslib_framing.cpp` | `tnfs_frame_reply` and `tnfsReplyAssembler`: finds where each reply ends in a byte stream (TNFS has no length field); used by the TCP receive path |
| `tnfslib_stream_transport.h` | `tnfsStreamTransport`, the byte-stream connection a mount talks through |
| `tnfslib_stream_link.h`, `tnfslib_stream_link.cpp` | `tnfsStreamLink`: requests and replies over any `tnfsStreamTransport`; the framing, format, drop and reconnect rules |
| `tnfslib_tcp_stream.h`, `tnfslib_tcp_stream.cpp` | `tnfsTcpStream`: `tnfsStreamTransport` on `fnTcpClient` |

## How it fits
- `FileSystemTNFS` in [lib/FileSystem/](../FileSystem/) is the main consumer (host slots and the
  web file browser); on ESP its VFS shim registers TNFS mounts with ESP-IDF. The `TNFS:` adapter
  in [lib/network-protocol/](../network-protocol/) and the Meatloaf `tnfs` filesystem use it too.
- Transport is `fnUDP` or `fnTcpClient` from [lib/tcpip/](../tcpip/). A host name prefixed
  `_tcp.` or `_udp.` forces one; a failed TCP connect falls back to UDP.
- Over TCP, `tnfsStreamLink` runs the mount's `tnfsTcpStream`, and `tnfsReplyAssembler` cuts the
  byte stream into replies for the pending request: it
  checks session, sequence and command, settles a previous reply's optional fields (STAT uid
  and gid strings, a failed READ's or WRITE's count) by the header that follows them, skips
  earlier replies and TNFSD's sessionless ones, and never takes bytes received before the
  request went out as its answer. A malformed or ambiguous reply, a closed connection or a
  timeout with part of an answer received drops the connection, and the retry reconnects. The
  assembler exists only while TCP is connected; on ESP it is allocated with `PSRAMAllocator`
  (PSRAM first, then the default heap).
- A `tnfsw://` host slot is TNFS over a secure WebSocket, for a TNFS server behind a
  WebSocket-to-TCP bridge. `FileSystemTNFS` gives the mount a `tnfsWsStream` as
  `stream_transport`, which replaces `tcp_stream`, and forces `TNFS_PROTOCOL_TCP`: the same
  stream rules and reply framing, never UDP and never a fallback. Port 443 unless given. TLS
  always verifies the chain against the ESP-IDF certificate bundle, the name against the host
  and the dates against the SNTP clock; with no SNTP time, no connection. The transport is
  allocated like the assembler.
- Keep-alives: TCP and UDP mounts send one every 60 s from an `esp_timer`. A `tnfsw://` mount
  cannot (the timer task's stack cannot hold a TLS session), so `FileSystemTNFS` registers it
  in a `tnfsKeepAliveRegistry` that the main loop services: a tick every 60 s per mount;
  skipped if the mount replied in the last 30 s, is in a transaction on another task, or has no
  open connection; otherwise one `STAT keep-alive` with a 1.5 s reply wait, no retry delay and
  connecting refused. At most one such request per pass, mounts taken in turn. A connection
  that drops while idle is reopened by the next real operation; if that is more than TNFSD's
  session timeout later, the session is recovered but open files are not (as with TCP and UDP).

## Build
ESP: globbed into every target. PC: every `FUJINET_TARGET`.
