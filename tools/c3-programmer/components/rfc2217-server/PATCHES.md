# Local changes to rfc2217-server

This is igrr/rfc2217-server v0.4.0 (https://github.com/igrr/rfc2217-server, commit
281e424d4bca6c85fb8e4d727dd1fa0c315b95f1, Apache-2.0, see LICENSE.txt), kept in
the C3 programmer's `components/` instead of being pulled by the component
manager, with these changes:

- **TCP keepalive on the client socket.** `rfc2217_server_config_t` has three
  new fields, `keepalive_idle_s`, `keepalive_interval_s` and `keepalive_count`
  (0 = off, the upstream behaviour). When set, the accepted socket gets
  `SO_KEEPALIVE`, `TCP_KEEPIDLE`, `TCP_KEEPINTVL` and `TCP_KEEPCNT`. Upstream
  has no way to detect a client that vanished without closing the connection
  (laptop asleep, route lost): the receive thread then waits forever,
  `on_client_disconnected` never runs, and since the server takes one client at
  a time nobody else can connect.
- **Liveness check and disconnect** (`rfc2217_server_rx_count()`,
  `rfc2217_server_probe()`, `rfc2217_server_disconnect()`). Keepalive does not
  act while the server has unacknowledged data for the client (the target's log
  after a reset): lwIP retransmits with growing pauses instead, and the dead
  connection lingered for 31–34 s. The receive thread counts received chunks;
  `probe` sends telnet `DO TIMING-MARK` without blocking (trylock on the send
  mutex, `MSG_DONTWAIT`), which a telnet client answers with `WONT`; and
  `disconnect` does `shutdown(SHUT_RDWR)` on the client socket, which wakes the
  receive thread (and aborts a blocked send, `LWIP_NETCONN_FULLDUPLEX`), so
  `on_client_disconnected` runs and the next client is accepted. The policy
  (when to probe and drop) lives in the C3's `main/bridge.c`. `client_socket`
  is now -1 when there is no client (set in `create`, and before `close`).

Removed from the copy: examples, CI and tooling files. The rest is unchanged.
Worth proposing upstream; until then, update this copy by hand.
