# Logging

Logs go to `sdmc:/switch/romm_switch_client/log.txt`, mirrored to stdout/nxlink, and a debug SVC string so you can see output without nxlink. The log file is kept open with a mutex-protected sink and a simple size cap (~512KB) that rotates to `log.txt.1` to reduce SD wear.

- Default: `info`. Set `log_level=debug|info|warn|error` in `.env`/`config.json`.
- Tags: `[APP]` lifecycle (default), `[API]` API info/debug, `[DL]` download heartbeat/finalize, `[UI]` render traces, `[SDL]` SDL init issues.
- Debug-only noise (render traces, download heartbeats, per-file listings) only when `log_level=debug`.
- Preflight logs HTTP status codes for HEAD/Range so 404/tiny responses are visible in info logs.
- Info keeps startup, config echo, API counts, queue actions, download start/finalize/results. Input/controller codes stay at debug to keep logs readable.

Environment example (`sdmc:/switch/romm_switch_client/.env`):
```
log_level=info
```

Tip: use `debug` only when diagnosing; keep `info` to reduce SD writes and keep logs readable.

TODO:
- Make logger thread-safe (single sink + mutex) and avoid open-per-line writes (add buffering/rotation to reduce SD wear).
