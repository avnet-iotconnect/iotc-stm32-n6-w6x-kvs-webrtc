# IOTCONNECT UI Onboarding Quickstart for STM32N6570-DK W6X

This quickstart adapts the Avnet IOTCONNECT UI onboarding flow for this board and this repository.

Reference flow:
- Avnet UI onboarding guide: https://github.com/avnet-iotconnect/iotc-python-lite-sdk-demos/blob/main/common/general-guides/UI-ONBOARD.md

This version replaces the Avnet host quickstart with:
- the firmware already running on the STM32N6570-DK
- the board CLI on the ST-LINK virtual COM port
- [provision.ps1](../bin/provision.ps1)

## What This Flow Uses

- Board:
  - STM32N6570-DK running this project firmware in `Flash Boot`
- Host:
  - Windows PowerShell
  - ST-LINK VCP connected to the board CLI
- Repo assets:
  - [stm32n6_w6x_iot_template_completed.json](../IOTCONNECT_Templates/stm32n6_w6x_iot_template_completed.json)
  - [provision.ps1](../bin/provision.ps1)

## Before You Start

1. Make sure the board is flashed with the project firmware and is booting normally.
2. Put the board in `Flash Boot`.
3. Power-cycle it.
4. Confirm the board CLI is alive on the ST-LINK COM port.
5. Close any serial terminal before running the PowerShell script.

Recommended config in [config.json](../bin/config.json):

```json
{
  "broker_type": "iotconnect",
  "wifi_ssid": "YOUR_WIFI",
  "wifi_credential": "YOUR_PASSWORD"
}
```

## Step 1. Log In To IOTCONNECT

1. Open `https://console.iotconnect.io`
2. Sign in to your IOTCONNECT account.

## Step 2. Import The Device Template

1. Open the `Device` area in the IOTCONNECT UI.
2. Go to `Templates`.
3. Click `Create Template`.
4. Click `Import`.
5. Select:
   - [stm32n6_w6x_iot_template_completed.json](../IOTCONNECT_Templates/stm32n6_w6x_iot_template_completed.json)
6. Save the template.

## Step 3. Start Enrollment On The Board

Run:

```powershell
cd <repo>\bin
.\provision.ps1
```

The script:
- connects to the board CLI
- reads the board `thing_name`
- lets you keep that `thing_name` or change it
- generates `tls_key_priv` and `tls_key_pub` on the device
- generates `tls_cert` on the device
- saves the printed PEM certificate to:
  - `bin\generated_iotconnect_identity\<thing_name>.iotconnect.cert.pem`
- prints the exact `/IOTCONNECT` UI instructions in the terminal

## Step 4. Create The Device In IOTCONNECT

1. In IOTCONNECT, go to `Devices`.
2. Click `Create Device`.
3. Set:
   - `Unique ID = thing_name`
   - `Device Name = thing_name`
5. Select the desired `Entity`.
6. Select the imported STM32N6 W6X template.
7. In `Device Certificate`, choose:
   - `Use my certificate`
8. Do not select `Auto-generated` or any other certificate option.

## Step 5. Paste The Device Certificate

1. Open the generated file:
   - `bin\generated_iotconnect_identity\<thing_name>.iotconnect.cert.pem`
2. Copy the full PEM text including:
   - `-----BEGIN CERTIFICATE-----`
   - `-----END CERTIFICATE-----`
3. Paste it into the IOTCONNECT device certificate box.

## Step 6. Save The Device And Download The JSON

1. Click `Save & View`.
2. Open the device page.
3. Download the device configuration JSON from the UI.
4. Save it locally.

Recommended filename:
- `iotcDeviceConfig.json`

## Step 7. Paste The Device JSON Back Into The Script

1. Return to the running PowerShell provisioning script.
2. Press Enter when prompted.
3. Paste the full downloaded device JSON.
4. Type `ENDJSON` on its own line.

The script then:
- validates backend, CPID, ENV, UID, DID, and discovery URL
- checks that UID and DID match the board `thing_name`
- imports the MQTT root CA
- imports the IOTCONNECT DRA CA
- verifies the on-device certificate and public key
- writes:
  - `broker_type=iotconnect`
  - `iotc_cloud`
  - `iotc_cpid`
  - `iotc_env`
  - `iotc_app_mode`
  - Wi-Fi settings
- uses `thing_name` as the `/IOTCONNECT` device identity
- clears cached IOTCONNECT identity state
- commits config and resets the device

## Step 8. Verify Runtime

After the board reboots, open the board's ST-LINK COM port at `115200 8N1`.

Expected signs of success:
- project startup logs appear
- Wi-Fi initializes
- IOTCONNECT identity/bootstrap completes
- telemetry is published
- if Video Streaming is enabled, `[KVSWebRTC]` logs appear

## Supported On-Device Identity Types

Current firmware support for this flow:
- on-device EC private/public key generation
- self-signed certificate generation using the on-device private key

## Related Files

- [provision.ps1](../bin/provision.ps1)
- [provision_iotconnect.md](../provision_iotconnect.md)
- [stm32n6_w6x_iot_template_completed.json](../IOTCONNECT_Templates/stm32n6_w6x_iot_template_completed.json)
