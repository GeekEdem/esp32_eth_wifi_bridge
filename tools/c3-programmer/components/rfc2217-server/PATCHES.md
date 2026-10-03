# Local changes to rfc2217-server

This is igrr/rfc2217-server v0.4.0 (https://github.com/igrr/rfc2217-server, commit
281e424d4bca6c85fb8e4d727dd1fa0c315b95f1, Apache-2.0, see LICENSE.txt), kept in
the C3 programmer's `components/` instead of being pulled by the component
manager, with one change:

- **TCP keepalive on the client socket.** `rfc2217_server_config_t` has three
  new fields, `keepalive_idle_s`, `keepalive_interval_s` and `keepalive_count`
  (0 = off, the upstream behaviour). When set, the accepted socket gets
  `SO_KEEPALIVE`, `TCP_KEEPIDLE`, `TCP_KEEPINTVL` and `TCP_KEEPCNT`. Upstream
  has no way to detect a client that vanished without closing the connection
  (laptop asleep, route lost): the receive thread then waits forever,
  `on_client_disconnected` never runs, and since the server takes one client at
  a time nobody else can connect.

Removed from the copy: examples, CI and tooling files. The rest is unchanged.
Worth proposing upstream; until then, update this copy by hand.
