# IOTCONNECT Templates

This folder contains two IOTCONNECT device templates for the STM32N6 W6X IoT reference firmware. They are **not**
interchangeable — pick based on whether you need video streaming:

- **`stm32n6wrt.json`** ("STM32N6 W6X KVS WebRTC 2", template code `stm32n6wr2`, msgCode XG4EGET) — verified working export with `videoStreamResource`="2" / `videoStreamType`="3" (BOTH values are required for the backend to provision a KVS channel; a device on this template shows `CD: XG4EGET` in its boot log and receives `vs.carn` in the identity response).
  Use this one; it's what [readme.md](../readme.md)'s provisioning steps import. Required for the KVS WebRTC video
  demo.
- **`stm32n6_w6x_iot_template_completed.json`** ("STM32N6 W6X LED and Button Demo") — LED/button telemetry and
  commands only, no video-streaming properties. Use only if you specifically want a non-video template.

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
    - `version` (same value; n6uvc-dashboard-compatible name)
    - `led_red`
    - `led_green`
    - `button_user`
    - AI (once the network is up): `ai_people`, `ai_top_conf`, `ai_infer_ms`
    - n6uvc-compatible AI aliases (see below): `nb_detect`, `conf`, `box_x`, `box_y`, `box_w`,
      `box_h`, `box_area`, `x_center`, `y_center`, `width`, `height`
  - Supported C2D commands:
    - `LED_RED_ON`
    - `LED_RED_OFF`
    - `LED_GREEN_ON`
    - `LED_GREEN_OFF`
    - `LED_ALL_ON`
    - `LED_ALL_OFF`
    - `LCD_ON`
    - `LCD_OFF`

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
- The template covers all `demo`-mode telemetry and commands. Of `sample` mode's fields, only
  `mode` and `version` are declared — `random_int`, `random_decimal`, `random_boolean`, and
  `coordinate.x/.y` are not in the template, and the platform silently drops undeclared
  attributes if you run sample mode.

## Reusing Dashboards from ST's N6 AI Demos (`n6uvc`)

The demo-mode firmware also publishes the attribute names used by the
[iotc-stm32-n6-demos](https://github.com/avnet-iotconnect/iotc-stm32-n6-demos) UVC demo (template
code `n6uvc`), with matching value semantics, so dashboards built for that demo work against this
device:

| Attribute | Value published here |
|---|---|
| `nb_detect` | detection count from the box snapshot (caps at 8; always consistent with the box fields in the same message — `ai_people` is the uncapped count, 0..10) |
| `box_x`, `box_y`, `box_w`, `box_h` | top-confidence detection's clamped corner box, **pixels in a virtual 1280x720 frame** — the UVC demo's VENC coordinate space, so widget scales match |
| `box_area` | `box_w * box_h` (px^2; the UVC dashboard's distance zones at 200k/400k/600k behave identically) |
| `conf` | top-detection confidence, truncated percent 0..100 |
| `version` | firmware version string |
| `x_center`, `y_center` | box center, normalized 0..1 (the UVC firmware never actually sent these; here they're live) |
| `width`, `height` | constants 1280 / 720 documenting the virtual frame (also never sent by the UVC firmware) |

All box fields are zeroed while nothing is detected, mirroring the UVC firmware. One deliberate
difference: this firmware reports the **highest-confidence** detection; the UVC demo reported
whichever detection came first out of postprocessing.

Two ways to reuse your dashboards:

1. **Import a dashboard export against this template** *(recommended)*: create the device with
   `stm32n6wrt.json` as described in the [QUICKSTART](../QUICKSTART.md), then import your dashboard
   export (e.g. `Projects/uvc/strm32n6_uvc_dashboard.json` in the iotc-stm32-n6-demos repo) and
   point it at this device — widgets bind by attribute name, and all names/scales match.
2. **Keep dashboards bound to your existing `n6uvc` template**: create this device under `n6uvc`
   instead. **Caution — the stock `n6uvc` template has no commands and no video-streaming
   properties**, so you lose Start/Stop Video, KVS streaming, and the LED/LCD commands unless you
   first edit that template in the console: enable video streaming (`videoStreamResource` = `2`,
   `videoStreamType` = `3`), and add the commands and extra attributes from `stm32n6wrt.json`.
   Option 1 is simpler unless your dashboards can't be re-imported.

## Cloud-Side Payload Note

The device publishes IOTCONNECT vendor telemetry in the raw `d` envelope format. Cloud dashboards/events may show a transformed wrapper such as `msgType`, `uniqueId`, `reporting`, and `time`. That wrapper is not the exact on-device MQTT payload.
