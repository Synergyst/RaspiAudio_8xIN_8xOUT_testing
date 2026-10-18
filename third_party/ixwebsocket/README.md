# Local IXWebSocket shutdown compatibility patches

`IXSocketOpenSSL.cpp` is copied from the IXWebSocket source used by this machine's
installed static library (`/root/Sources/IXWebSocket/ixwebsocket/IXSocketOpenSSL.cpp`).
Original SHA-256: `bbe1514e8bde788efb1dcd8c84ae51a405e6780b849c73dfeb302c82da914a0b`.
The original BSD license is retained in `LICENSE.txt`.

`IXSocketServer.cpp` is copied from the same matching checkout; original SHA-256:
`2555e28d95e88d34c33499d10999a3ae3dc8aa9eaac73279b5710ffc8794f721`.
Its local changes remove a raw second close after ownership has transferred to a
Socket (TLS failure already closes it), and close an accepted but not-yet-transferred
fd when stop wins. This prevents timeout/error handling from closing reused,
unrelated descriptors and avoids leaking the late-accepted descriptor.

TLS changes are deliberately limited to installed-header include paths and a one-second
server TLS handshake deadline with short readiness waits. IX accepts TLS on its
accept thread; the installed implementation otherwise retries `SSL_accept()` forever
for a silent TCP peer, preventing application shutdown. Client TLS behavior is unchanged.

`IXWebSocketServer.cpp` comes from the same checkout (original SHA-256
`9d66ad6d0f7bcb86898f592d5b23dc9561b51234eb1dabb0e104ddd4bc521e4c`).
It retains the registered message callback through socket destruction, instead
of clearing it while a shutdown snapshot can still call close(). The old path
races callback invocation and can throw `bad_function_call` during connection
churn. cm5audio uses weak socket captures to avoid an ownership cycle.

The Makefile links these three objects **before** `-lixwebsocket`. With the supported
static IX library, its old `IXSocketOpenSSL.cpp.o`, `IXSocketServer.cpp.o`, and `IXWebSocketServer.cpp.o` are not extracted. No installed library or
system audio configuration is modified. A different IX release/ABI needs comparison
against these headers/source before upgrading; do not assume this compatibility
layer is a universal IX patch. The build must use TLS-enabled IX headers matching
the linked library. Tests cover successful TLS and a deliberately stalled handshake.

The separate project-owned `include/reliable_websocket_server.h` closes another
shutdown race: accepted workers not yet present in IX's client snapshot.
