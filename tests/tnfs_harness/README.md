# tests/tnfs_harness

Stand-ins for the FujiNet headers that the TNFS library sources include, so the real
`lib/TNFSlib` transaction, mount and stat code links on a PC without the bus, the network
stack or the hardware. Used only by `tnfs_ws_transaction_tests`.

| File | Replaces | Behaviour |
|---|---|---|
| `fnSystem.h` | `lib/hardware/fnSystem.h` | Virtual clock: `millis()` returns a counter that `delay()` and `delay_microseconds()` advance |
| `bus.h` | `lib/bus/bus.h` | `SYSTEM_BUS.getShuttingDown()` is always false |
| `fnUDP.h`, `fnTcpClient.h` | `lib/tcpip/` | Always fail, and count every call (to prove a WSS mount never falls back) |
| `fnDNS.h` | `lib/tcpip/fnDNS.h` | `get_ip4_addr_by_name()` fails |
| `utils.h` | `lib/utils/utils.h` | Only the unused-variable pragma macros |

The definitions live in `TnfsWsTransactionTests.cpp`. Put this directory first on the
include path and do not add `lib/tcpip`, `lib/hardware`, `lib/bus` or `lib/utils`.
