# Repository Structure

This document maps the repository layout and the role of each major module.

## Top-Level Layout

- `FSBL/` — first-stage bootloader project
- `Appli/` — main firmware project (FreeRTOS, networking, TLS, IoTConnect, KVS WebRTC)
- `bin/` — script-based flashing and provisioning flow
- `docs/` — technical documentation
- `IOTCONNECT_Templates/` — IoTConnect device template JSON
- `provision_iotconnect.md` — provisioning walkthrough

## `Appli/Common` Overview

### `app/`

Application tasks:

- `mqtt/` — MQTT agent task, reconnect logic, subscription dispatch
- `iotconnect/` — IoTConnect runtime (identity, telemetry, commands, KVS config parsing)
- `kvs_webrtc/` — KVS WebRTC task (signaling, media pipeline)
- `led/` — LED desired/reported control
- `button/` — button event reporting

### `cli/`

UART CLI framework and command handlers, including PKI and runtime configuration commands.

### `config/`

Central middleware/runtime configuration headers:

- `kvstore_config.h`
- `lwipopts.h`
- `mbedtls_config.h`
- `core_mqtt_config.h`

### `crypto/`

TLS and PKCS#11 integration:

- mbedTLS transport layer
- FreeRTOS integration hooks
- PKCS#11 PAL/helper components

### `include/`

Shared public headers used across modules.

### `kvstore/`

Non-volatile runtime configuration storage (KVS implementation).

### `net/`

ST67 networking integration:

- `W6X_ARCH_T02/` — ST67 T02 architecture path
- `lwip_port/` — LwIP and FreeRTOS glue

### `sys/`

Shared system utilities and support functions.
