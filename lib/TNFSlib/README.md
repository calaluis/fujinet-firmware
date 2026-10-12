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
| `tnfslib_transport.cpp` | the UDP, TCP and WSS transport code, the request/response transaction with retries, and session recovery |
| `tnfslib_codes.cpp` | packet debug dumps, command and result-code names, `tnfs_code_to_errno` |
| `tnfslib_internal.h` | declarations shared by the `tnfslib*.cpp` files; not part of the public API |
| `tnfslibMountInfo.h`, `tnfslibMountInfo.cpp` | `tnfsMountInfo` (host, session, `TNFS_PROTOCOL_TCP`, `TNFS_PROTOCOL_UDP` or `TNFS_PROTOCOL_WS` with its stream link, open handles, directory cache) and `tnfsStat` |
| `tnfslib_udp.h`, `tnfslib_udp_testing.cpp` | the UDP send and receive hooks, with a replacement that simulates packet loss and duplication when `TNFS_UDP_SIMULATE_POOR_CONNECTION` is defined |
| `tnfslib_framing.h`, `tnfslib_framing.cpp` | `tnfs_frame_reply` and `tnfsReplyAssembler`: finds where each reply ends in a byte stream (TNFS has no length field). Used by the WSS path, not by the TCP receive path |
| `tnfslib_stream.h`, `tnfslib_stream.cpp` | `tnfsStreamTransport`, the port a byte-stream transport implements, and `tnfsStreamLink`, which writes whole requests within one deadline and cuts replies out of that stream |
| `tnfslib_ws.h`, `tnfslib_ws.cpp` | `tnfs_make_ws_transport`: the WSS transport on `esp_transport_ws` over `esp_transport_ssl` with the certificate bundle (ESP only; `nullptr` on PC) |
| `tnfslib_ws_frames.h`, `tnfslib_ws_frames.cpp` | `tnfsWsFrameTracker`: what each WebSocket read means (data, PING, CLOSE, lost frame); pure, so it is tested on PC |

## How it fits
- `FileSystemTNFS` in [lib/FileSystem/](../FileSystem/) is the main consumer (host slots and the
  web file browser); on ESP its VFS shim registers TNFS mounts with ESP-IDF. The `TNFS:` adapter
  in [lib/network-protocol/](../network-protocol/) and the Meatloaf `tnfs` filesystem use it too.
- Transport is `fnUDP` or `fnTcpClient` from [lib/tcpip/](../tcpip/). A host name prefixed
  `_tcp.` or `_udp.` forces one; a failed TCP connect falls back to UDP.
- A host name prefixed `_wss.` (host slots only) uses TNFS over `wss://host/` (port 443 unless
  given): one binary frame per request, replies framed from the byte stream, TLS verified
  against the certificate bundle and the host name. It never falls back to TCP or UDP.

## Build
ESP: globbed into every target. PC: every `FUJINET_TARGET`.
