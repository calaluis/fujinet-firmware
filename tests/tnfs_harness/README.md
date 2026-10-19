# tests/tnfs_harness

Stand-ins for the headers the TNFS client includes, so the real `lib/TNFSlib` transaction,
mount, file and directory code runs on a host against a scripted server.

## Layout
| File | Replaces | Defines |
|---|---|---|
| `harness_server.h` | — | `HarnessServer`, the scripted peer: `on_request` sees each request, `deliver()` queues bytes that become readable after a delay; connection and request counters |
| `fnTcpClient.h` | `lib/tcpip/fnTcpClient.h` | A TCP connection to `harness_server`: a byte stream where a read returns every byte that has arrived; `stop()` discards unread bytes; a peer close is seen once everything queued has been read |
| `fnUDP.h` | `lib/tcpip/fnUDP.h` | UDP to `harness_server`: each delivery is one datagram, read whole |
| `fnSystem.h` | `lib/hardware/fnSystem.h` | A virtual clock: `millis()` moves only when the code under test calls `delay()` or `delay_microseconds()` |
| `bus.h` | `lib/bus/bus.h` | `SYSTEM_BUS.getShuttingDown()`, always false |
| `fnDNS.h` | `lib/tcpip/fnDNS.h` | `get_ip4_addr_by_name()`, always fails; tests use a host name only as a label |
| `utils.h` | `lib/utils/utils.h` | The unused-variable pragma macros only |
| `harness_ws.h` | `tnfslib_wss.cpp`'s ESP-IDF adapter | `harness_ws::SimIo`, a `tnfsWsIo` that reads frames the way ESP-IDF 5.4 `esp_transport_ws` does with control frames propagated (including forgetting a frame whose payload read times out), over `Net`, scripted byte runs and stalls; `WsLog` records connects, frames sent and timeouts, and injects failures |

## How it fits
- Used by `tnfs_tcp_recv_tests` (`tests/TNFSTcpRecvTests.cpp`), which defines the
  `fnSystem`, `SYSTEM_BUS` and `harness_server` objects and a minimal TNFS server on top of
  `harness_server`.
- That test links the real `tnfslib.cpp`, `tnfslib_dir.cpp`, `tnfslib_transport.cpp`,
  `tnfslib_codes.cpp`, `tnfslibMountInfo.cpp`, `tnfslib_framing.cpp`,
  `tnfslib_stream_link.cpp` and `tnfslib_tcp_stream.cpp` from
  [lib/TNFSlib/](../../lib/TNFSlib/), and for its WSS cases `tnfslib_ws_frames.cpp`,
  `tnfslib_ws_stream.cpp`, `tnfslib_wss.cpp` and `tnfslib_keep_alive.cpp`.
- `tnfs_ws_tests` (`tests/TNFSWsTests.cpp`) runs `tnfsWsStream` under `tnfsStreamLink` on
  `harness_ws.h`.
- `tnfs_stream_link_tests` (`tests/TNFSStreamLinkTests.cpp`) drives `tnfsStreamLink`
  through its own scripted transport; it uses these headers only because
  `tnfslibMountInfo.cpp` and the link include them.

## Build
PC only. `tests/CMakeLists.txt` puts this directory first on the include path of
`tnfs_tcp_recv_tests`, `tnfs_stream_link_tests` and `tnfs_ws_tests`, so these headers win over the real ones,
and builds them with `UNIT_TESTS`. Do not add `lib/tcpip`, `lib/hardware`, `lib/bus` or
`lib/utils` to those targets. The tests run under ctest (`./build.sh -p ATARI`, or
`ctest -R tnfs_` from `build/`).

## Notes
What it does not verify:
- real sockets, lwIP or ESP-IDF behavior, or the real `fnTcpClient` and `fnUDP`;
- timing on a device: the clock is virtual, and only the host (non-`ESP_PLATFORM`) branches of
  the receive loop are compiled;
- the `esp_timer` keep-alive of TCP and UDP mounts, or concurrent access to a mount beyond
  the WSS keep-alive's own cases (a busy mount, removal during a keep-alive);
- DNS, UDP loss or reordering, and TNFS servers other than the scripted one;
- TLS, the HTTP upgrade or the real `esp_transport_ws`: `harness_ws.h` follows its read
  semantics, but the ESP-IDF adapter in `tnfslib_wss.cpp` is only compiled on ESP.
