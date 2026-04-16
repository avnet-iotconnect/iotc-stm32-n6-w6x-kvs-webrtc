# IoTConnect Provisioning for STM32N6570-DK

This guide explains how to provision a single STM32N6570-DK device on IoTConnect using the automated `provision.ps1` workflow.

The device will:
- generate its own certificate on the board
- register in IoTConnect with `Use my certificate`
- download the IoTConnect device JSON
- configure the runtime without reflashing firmware
- auto-configure KVS WebRTC streaming (if the template has Video Streaming enabled)

## 1. Hardware Setup

- Connect the Wi-Fi module to the `Arduino` connector.
- Connect ST-Link USB to your PC for power, flashing, and debugging.

## 2. Prerequisites

Before provisioning:

1. Build the application and FSBL in STM32CubeIDE (or use pre-built binaries).
2. If you built from STM32CubeIDE, from `bin/`, run:
   ```powershell
   .\copy_hex_from_project.ps1
   ```
3. Confirm `bin/config.json` contains:
   ```json
   {
     "broker_type": "iotconnect",
     "wifi_ssid": "YOUR_WIFI",
     "wifi_credential": "YOUR_PASSWORD"
   }
   ```
4. Close any serial terminal connected to the board before running the script.

## 3. Run Automated Provisioning

From `bin/`:

```powershell
.\run_all.ps1
```

Or to provision only (board already flashed):

```powershell
.\provision.ps1
```

The script:
- detects the ST-LINK virtual COM port automatically
- reads the board `thing_name`
- lets you keep or update the `thing_name`
- generates `tls_key_priv`, `tls_key_pub`, and `tls_cert` on the device
- prints the device certificate in PEM format for IoTConnect onboarding
- waits for you to paste the downloaded IoTConnect device JSON
- configures the runtime and resets the board

## 4. Create the Device in IoTConnect

When the script prints the onboarding instructions:

1. Open the IoTConnect `Create Device` page.
2. Use the board `thing_name` as both `Unique Id` and `Device Name`.
3. Select the appropriate `Entity`.
4. Select the imported device template.
   - For KVS WebRTC streaming, use a template with **Video Streaming** enabled.
5. In `Device certificate`, select only `Use my certificate`.
6. Paste the device certificate shown by the script into `Certificate Text`.
7. Click `Save & View`.
8. On the device page, click the document and gear icon to download the device JSON.

## 5. Paste the IoTConnect Device JSON

When the script prompts for the device JSON:

1. Paste the full JSON content into the terminal.
2. Type `ENDJSON` on its own line.

The script then:
- validates `pf`, `cpid`, `env`, `uid`, `did`, and `disc`
- verifies `uid` matches the board `thing_name`
- imports the built-in MQTT root CA and IoTConnect DRA CA
- writes the IoTConnect runtime configuration to KVStore
- resets the board

## 6. KVS WebRTC Auto-Configuration

If the device template has Video Streaming enabled, the firmware automatically:
1. Receives KVS config from the IoTConnect identity response (`vs` block)
2. Parses the signaling channel ARN and credentials endpoint
3. Connects to the KVS signaling channel as a WebRTC master peer

No manual KVS configuration is needed.

## 7. Monitor First Boot

After reset:

- wait up to 60 seconds for the device to connect
- monitor the ST-LINK VCP in a serial terminal at `115200 8N1`
- confirm the device appears connected in IoTConnect
- if Video Streaming is enabled, confirm `[KVSWebRTC]` logs in the serial output

If the device does not connect:

- verify Wi-Fi credentials in `bin/config.json`
- verify the pasted JSON belongs to the created device
- verify `Unique Id` matches the board `thing_name`
- rerun `.\provision.ps1` to reprovision without reflashing

## 8. Reprovision Without Reflashing

If the device is already flashed and you only need to reprovision:

```powershell
.\provision.ps1
```

Use this to:
- regenerate or replace the device certificate
- create a new IoTConnect device
- paste a new device JSON
- update Wi-Fi or IoTConnect runtime settings

---

[Back to Main README](readme.md)
