# ST67W6X Network Driver — v1.2.0 → v1.3.0 upgrade notes

Upgraded 2026-04-15 from X-CUBE-ST67W61 v1.2.0 to v1.3.0.  Backup of the
pre-upgrade tree is at `../ST67W6X_Network_Driver.v120_backup/`.

## Motivation

The WAN UDP RX blackhole in v1.2.0 (documented in
`_st_bugreport_artifacts/05_v130_reproduction_result.txt`) does not reproduce
on the full v1.3.0 middleware tree running the same NCP on the same AP.
Upgrade is required for UDP TURN support and to unblock the KVS WebRTC
NAT-traversal path.

## What was copied from v1.3.0

All files that existed in both v1.2.0 and v1.3.0 trees were replaced
wholesale with their v1.3.0 counterparts:

- `Api/` — `w6x_api.h`, `w6x_types.h`, `w6x_legacy.h`, `w6x_version.h`,
  `common_parser.h`
- `Core/` — `w6x_ble.c`, `w6x_default_config.h`, `w6x_http.c`,
  `w6x_internal.h`, `w6x_mqtt.c`, `w6x_net.c`, `w6x_netif.c`, `w6x_sys.c`,
  `w6x_wifi.c`
- `Driver/W61_at/` — all 12 files (modem_cmd_handler, w61_at_*)
- `Utils/Misc/common_parser.c`
- `Driver/W61_bus/` — already upgraded in a prior partial swap, unchanged

Project-local modifications to `Core/w6x_sys.c` (the `NCP-RECOVERY` factory
reset block behind `NCP_FACTORY_RESET_ONCE`) were dropped — they were
diagnostic instrumentation for the v1.2.0 UDP drop investigation and are no
longer needed.  A separate `NCP_FACTORY_RESET_ONCE` flag exists in
`Appli/Common/net/W6X_ARCH_T02/st67w6x_netconn.c` (independent).

## What was deliberately NOT copied from v1.3.0

The v1.3.0 release adds a CLI/shell/perf-measurement toolkit that is only
needed by the bundled `ST67W6X_CLI_LWIP` reference demo.  This project has
no shell task, so the following were omitted to keep flash and RAM
footprint unchanged:

**New sub-trees (entirely omitted):**
- `Utils/Shell/` — generic shell host (SHELL_CMD_EXPORT_ALIAS machinery)
- `Utils/Performance/` — iperf, WFA traffic generator, task/mem perf counters
- `Utils/Logging/` — ST's logging backend (we use the project's own
  `Appli/Common/cli/logging.h` / `logging.c` instead)
- `Conf/` — v1.3.0 config templates (we have project-specific equivalents
  at `Appli/ST67W6X_Network_Driver/Target/`)
- `Doc/` — markdown documentation
- `_htmresc/` — release-notes assets

**New shell CLI `.c` files in `Core/` (entirely omitted):**
- `w6x_ble_shell.c`, `w6x_mqtt_shell.c`, `w6x_net_shell.c`,
  `w6x_sys_shell.c`, `w6x_wifi_shell.c`
- Also `Core/w6x_ble_shell.c` (BLE CLI handlers)

**New `Api/` headers (entirely omitted):**
- `shell.h`, `shell_default_config.h` — shell host API
- `logging.h`, `logging_levels.h` — ST's logging API
  (would collide with project's own `Appli/Common/cli/logging.{h,c}` that
  already provides `LogError` / `LogInfo` / `LogDebug` / `LogWarn` expected
  by the middleware's `SYS_LOG_*` / `WIFI_LOG_*` / etc. macros)
- `iperf.h`, `wfa_tg.h` — performance-test APIs
- `util_mem_perf.h`, `util_task_perf.h` — perf-counter APIs

## Target port file changes (Appli/ST67W6X_Network_Driver/Target/)

- `spi_port.c` — replaced wholesale with v1.3.0 version.  Changes:
  adds `#include "logging.h"`, removes `extern SPI_HandleTypeDef
  NCP_SPI_HANDLE` (moved to bsp_conf.h), adds `HAL_SPI_STATE_RESET`
  guards + `LogError` in `spi_port_init`, `spi_port_transfer`,
  `spi_port_transfer_dma`; adds `hdmatx == NULL` check for DMA.
- `bsp_conf.h` — added `#include "main.h"` and
  `extern SPI_HandleTypeDef NCP_SPI_HANDLE` (moved from spi_port.c).
  Did NOT add `UART_HANDLE` (project doesn't use huart1).
- `w61_driver_config.h` — added `WIFI_LOG_ENABLE 1` and
  `NET_LOG_ENABLE 1` (new v1.3.0 per-subsystem log flags).
- `w6x_config.h` — left unchanged (preserves `W6X_POWER_SAVE_AUTO=0`).
- `logging_config.h`, `shell_config.h` — left unchanged (cosmetic-only
  diffs; `SHELL_ENABLE=0` is correct for this project).

## LwIP glue changes (Appli/Common/net/W6X_ARCH_T02/)

- `lwip_netif.c`, `lwip_netif.h` — replaced wholesale with v1.3.0.
  Key functional change: error path in `netif_rx_process` now calls
  `W6X_Netif_free(buffer)` + `netif_pbuf_free(pb)` separately instead
  of `pbuf_free(pb)` (bugfix for partially-set-up pbuf).
- `dhcp_server_raw.c`, `dhcp_server.h` — replaced wholesale.
  `dhcpd_stop_by_name()` removed; `dhcpd_stop()` now takes netif
  pointer directly.
- `lwip.c` — v1.3.0 base with project-local sections grafted back:
  (1) logging headers + `event_groups.h` instead of `app_config.h`;
  (2) `APP_setevent(&app_evt_current, EVT_APP_WIFI_GOT_IP)` in
  `netif_status_callback`.  Also gains v1.3.0 improvements:
  `net_if_init()` moved after netif setup (race fix),
  `netif_create_ip6_linklocal_address` flag 0→1 (MAC-based),
  `netif_set_link_down` added to AP down callback,
  `ap_sta_ipv4_table` per-station tracking, `sys_now()`.
- `lwip.h` — v1.3.0 base with `EVT_APP_*` defines preserved in
  USER CODE block.
- `st67w6x_netconn.c`, `st67w6x_netconn.h` — unchanged (project-local,
  no v1.3.0 counterpart).
- `net_diag.c`, `net_diag.h` — unchanged; `NetDiag_Run()` call in
  `kvs_webrtc_task.c` commented out.

## Logging compatibility note

v1.3.0 middleware files reference `LogError` / `LogWarn` / `LogInfo` /
`LogDebug` through macros like `SYS_LOG_INFO` (in `Driver/W61_at/w61_at_api.h`).
These macros resolve to the project's existing
`Appli/Common/cli/logging.h`, which provides compatible macros over the
project's `vLoggingPrintf(pcLogLevel, pcFunctionName, ulLineNumber, pcFormat,
...)` backend.  No change to the project's logging system was required.

## Next steps after upgrade

1. Re-build and confirm no compile errors (see `build_and_flash.sh`).
2. Confirm TCP-only TURN streaming still works on first boot (regression
   check; no behaviour change expected).
3. Flip the ICE filter in `Appli/Common/app/kvs_webrtc/app_common.c:735-749`
   from UDP-reject to UDP-accept, re-flash, and confirm UDP TURN now works
   end-to-end (the actual purpose of the upgrade).
