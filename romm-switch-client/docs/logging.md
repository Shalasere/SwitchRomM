# Logging

The client writes logs to `sdmc:/switch/romm_switch_client/log.txt`, mirrors to stdout/nxlink, and issues a debug SVC string so you can see output without nxlink.

- Default level: `info`. Configure via `.env` or `config.json` with `log_level=debug|info|warn|error`.
- Tags: `[APP]` core lifecycle (default for most logs), `[API]` API helper debug/info, `[DL]` downloader heartbeat/finalize, `[UI]` render traces, `[SDL]` SDL init issues.
- High-chatter debug entries (render traces, download heartbeats, per-file listings) only emit when `log_level=debug`.
- Info-level essentials kept: startup, config echo, API fetch counts, queue actions, download start/finalize/results. Input/controller codes stay at debug to reduce noise.

Environment example (`sdmc:/switch/romm_switch_client/.env`):
```
log_level=info
```

Tip: set `log_level=debug` only when diagnosing downloads/UI; keep `info` to reduce SD writes and keep logs readable.
