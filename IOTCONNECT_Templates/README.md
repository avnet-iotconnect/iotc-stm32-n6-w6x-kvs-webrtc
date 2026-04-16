# IOTCONNECT Templates

This folder contains the reviewed IOTCONNECT device template for the STM32N6 W6X IoT reference firmware.

Source reviewed against:
- [iotconnect_runtime.c](../Appli/Common/app/iotconnect/iotconnect_runtime.c)
- original exported template JSON provided during bring-up review

## Firmware Modes

The firmware supports two IOTCONNECT app modes via KV key `iotc_app_mode`:

- `demo`
  - Default mode
  - Telemetry fields:
    - `mode`
    - `firmware_version`
    - `led_red`
    - `led_green`
    - `button_user`
  - Supported C2D commands:
    - `LED_RED_ON`
    - `LED_RED_OFF`
    - `LED_GREEN_ON`
    - `LED_GREEN_OFF`
    - `LED_ALL_ON`
    - `LED_ALL_OFF`

- `sample`
  - Telemetry fields:
    - `mode`
    - `version`
    - `random_int`
    - `random_decimal`
    - `random_boolean`
    - `coordinate.x`
    - `coordinate.y`
  - C2D command callback exists, but returns `Not implemented`
  - OTA callback is only a stub/status response, not a full OTA implementation

## Template Notes

- The completed JSON keeps the same general structure as the exported template.
- The missing `version` attribute used by sample mode has been added.
- Friendly `displayName` and `description` values have been filled in.
- Command `name` values are human-readable while `command` remains the exact token parsed by firmware.
- This template is a combined superset covering both `demo` and `sample` firmware modes.

## Cloud-Side Payload Note

The device publishes IOTCONNECT vendor telemetry in the raw `d` envelope format. Cloud dashboards/events may show a transformed wrapper such as `msgType`, `uniqueId`, `reporting`, and `time`. That wrapper is not the exact on-device MQTT payload.
