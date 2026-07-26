# STM32N6570-DK KVS WebRTC Demo — QuickStart

<img src="https://raw.githubusercontent.com/avnet-iotconnect/iotc-stm32-n6-demos/main/media/n6.jpg" alt="STM32N6570-DK" width="500"/>

## 1. Introduction

This guide takes you from **nothing to a live /IOTCONNECT device** streaming real-time video — no
IDE, no compiler, no build step. You will flash **pre-built firmware**, provision the board over a
serial terminal, and end with:

- **Live video streaming** from the on-board camera to the /IOTCONNECT **Video Streaming** tab
  (AWS Kinesis Video Streams WebRTC)
- **On-device AI people detection** running on the STM32N6's Neural-ART NPU (YOLOv2), reported as
  telemetry (`ai_people`, `ai_top_conf`, `ai_infer_ms`) and drawn as overlay boxes on the board's LCD
- **Cloud commands** — start/stop video, LCD preview on/off, LED control

Two firmware variants are provided — pick one:

| Variant      | Network transport                        | Firmware image                                                                       |
|--------------|------------------------------------------|--------------------------------------------------------------------------------------|
| **Wi-Fi**    | ST67W611M1 Wi-Fi 6 module (X-NUCLEO)     | [bin/quickstart/stm32n6570-dk-kvs-demo-wifi.hex](bin/quickstart/stm32n6570-dk-kvs-demo-wifi.hex)         |
| **Ethernet** | On-board Gigabit Ethernet (RJ45)         | [bin/quickstart/stm32n6570-dk-kvs-demo-ethernet.hex](bin/quickstart/stm32n6570-dk-kvs-demo-ethernet.hex) |

> [!TIP]
> **Which variant?** Ethernet is the most robust for sustained video streaming and needs no extra
> hardware beyond a network cable — if your board is near a router, start there. The Wi-Fi variant
> needs the X-NUCLEO-67W61M1 module and a one-time module firmware update (Step 5).

All steps work on **Windows and Linux**. The only ST tool required is **STM32CubeProgrammer**.

## 2. Prerequisites

### Hardware

- **[STM32N6570-DK](https://www.st.com/en/evaluation-tools/stm32n6570-dk.html)** Discovery Kit,
  with the **camera module included in the kit** connected to the CSI connector
- **2× USB-C cables** (one powers/programs the board; keep the second spare for external power)
- **Wi-Fi variant only:**
  - [X-NUCLEO-67W61M1](https://www.st.com/en/evaluation-tools/x-nucleo-67w61m1.html) Wi-Fi module board
    (plugged onto the DK's Arduino headers)
  - A NUCLEO host board (e.g. [NUCLEO-U575ZI-Q](https://www.st.com/en/evaluation-tools/nucleo-u575zi-q.html))
    — used **once** in Step 5 to update the Wi-Fi module firmware
  - A 2.4/5 GHz WPA2 Wi-Fi network
- **Ethernet variant only:** an RJ45 cable to a network with DHCP

### Software

- **[STM32CubeProgrammer](https://www.st.com/en/development-tools/stm32cubeprog.html) version `2.20.0` or newer**
  (the latest version is fine — this guide's images are pre-signed, and programming was validated
  with `2.23.0`)

- A **serial terminal** — [Tera Term](https://teratermproject.github.io/index-en.html) or PuTTY on
  Windows, `picocom`/`minicom` on Linux

  > [!NOTE]
  > **Linux:** STM32CubeProgrammer needs udev rules the first time you connect a board:
  > ```sh
  > sudo cp ~/STMicroelectronics/STM32Cube/STM32CubeProgrammer/Drivers/rules/*.rules /etc/udev/rules.d/
  > sudo udevadm control --reload-rules && sudo udevadm trigger
  > sudo usermod -aG plugdev $USER   # then log out/in
  > ```

### Files to download

If you don't want to clone the whole repository, download just these three files (open each link on
GitHub, then click the **Download raw file** button at the top right of the file view):

1. Your firmware variant hex (table above)
2. The AI model: [bin/Model/network_data.hex](bin/Model/network_data.hex)
3. The device template: [IOTCONNECT_Templates/stm32n6wrt.json](IOTCONNECT_Templates/stm32n6wrt.json)

## 3. Cloud Account Setup

An /IOTCONNECT account with an **AWS backend** is required. If you need to create an account, a
**free trial subscription** is available — no credit card required:

- Sign up: [https://subscription.iotconnect.io/subscribe?cloud=aws](https://subscription.iotconnect.io/subscribe?cloud=aws)
  (see the [AWS Marketplace option](https://github.com/avnet-iotconnect/avnet-iotconnect.github.io/blob/main/documentation/iotconnect/subscription/iotconnect_aws_marketplace.md) if you prefer)
- Log in: [https://console.iotconnect.io/login](https://console.iotconnect.io/login)

> [!NOTE]
> Be sure to check your SPAM folder for the temporary password after registering if you don't see
> it after a couple of minutes.

## 4. Program the Board

### 4.1 Enter Dev Boot mode

1. With the board **unplugged**, set the **BOOT1** switch (SW2, near the ST-LINK connector) to the
   **right**. The BOOT0 switch position doesn't matter in this mode.
2. Connect a USB-C cable from your PC to the board's **ST-LINK USB port** (CN6).
3. Confirm: **LED2 lights up whenever Dev Boot is active** — that's the reliable visual check.

### 4.2 Flash with STM32CubeProgrammer (GUI)

1. Open STM32CubeProgrammer and click **Connect** (top right; port `SWD`, mode `Hot plug`).
2. Click the **EL** (External Loader) icon in the left menu and check
   **`MX66UW1G45G_STM32N6570-DK`** — this enables writing to the DK's external NOR flash.
3. Open the **Erasing & Programming** panel (left menu, second icon).
4. **Program the AI model:** *File path* → browse to `network_data.hex` → click **Start Programming**.
   Wait for *File download complete* (this file is large — allow a few minutes).
5. **Program the firmware:** *File path* → browse to your variant hex
   (`stm32n6570-dk-kvs-demo-wifi.hex` or `stm32n6570-dk-kvs-demo-ethernet.hex`) →
   **Start Programming** → wait for *File download complete*.
6. **Disconnect** in STM32CubeProgrammer.

> [!NOTE]
> **No separate bootloader step.** Unlike other STM32N6 guides that flash a bootloader
> (`ai_fsbl.hex`), model, and application as three files, the variant hex here already contains
> **both the bootloader (FSBL) and the application** — two files flashed total. Both hex files are
> **pre-signed** and carry their flash addresses internally (bootloader at `0x70000000`,
> application at `0x70100000`, AI model at `0x70380000`) — you never enter an address manually,
> and no signing tool is needed.

<details>
<summary><b>Command-line alternative (Windows & Linux)</b></summary>

Windows (PowerShell — adjust the install path if needed):

```powershell
$P = "C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer20\bin"
$EL = "$P\ExternalLoader\MX66UW1G45G_STM32N6570-DK.stldr"
& "$P\STM32_Programmer_CLI.exe" -c port=SWD mode=HOTPLUG ap=1 -w network_data.hex -el $EL
& "$P\STM32_Programmer_CLI.exe" -c port=SWD mode=HOTPLUG ap=1 -w stm32n6570-dk-kvs-demo-wifi.hex -el $EL
```

Linux:

```sh
P=~/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin
EL=$P/ExternalLoader/MX66UW1G45G_STM32N6570-DK.stldr
$P/STM32_Programmer_CLI -c port=SWD mode=HOTPLUG ap=1 -w network_data.hex -el $EL
$P/STM32_Programmer_CLI -c port=SWD mode=HOTPLUG ap=1 -w stm32n6570-dk-kvs-demo-wifi.hex -el $EL
```

(Substitute the `-ethernet` hex for the Ethernet variant.)

</details>

### 4.3 Return to Flash Boot mode

Unplug the USB cable, set **BOOT1 to the left and BOOT0 to the left**, then reconnect. LED2 should
now be **off** — the board boots the freshly-flashed firmware from external flash.

## 5. Wi-Fi Variant Only: Update the Wi-Fi Module Firmware

> [!NOTE]
> **Ethernet variant users: skip this step entirely.**

The ST67W611M1 module runs its own firmware, and this demo requires the **T02 mission profile,
V1.3.0 or newer** (older versions have a WAN UDP bug that silently breaks WebRTC connectivity).
This is a one-time update, done with a NUCLEO host board as the programmer:

1. Plug the **X-NUCLEO-67W61M1** onto the NUCLEO host board's Arduino headers, and connect USB to
   the **NUCLEO's ST-LINK port**.
2. Clone ST's tool repo: `git clone https://github.com/STMicroelectronics/x-cube-st67w61.git`
3. From `x-cube-st67w61/Projects/ST67W6X_Scripts/Binaries/`, run
   `NCP_update_mission_profile_t02.bat` (Windows) or `./NCP_update_mission_profile_t02.sh` (Linux).

   > On Linux, first: `chmod +x QConn_Flash/QConn_Flash_Cmd-ubuntu`

4. When the script reports success, move the X-NUCLEO board onto the **STM32N6570-DK's Arduino
   headers**. The NUCLEO host board is no longer needed.

## 6. Import the Device Template in /IOTCONNECT

1. Log in at [console.iotconnect.io](https://console.iotconnect.io).
2. Open the **Device** module:

   <img src="https://raw.githubusercontent.com/avnet-iotconnect/iotc-python-lite-sdk-demos/main/common/media/device-page.png" width="600"/>

3. At the bottom of the page, click **Templates**:

   <img src="https://raw.githubusercontent.com/avnet-iotconnect/iotc-python-lite-sdk-demos/main/common/media/templates-button.png" width="600"/>

4. Click **Create Template**:

   <img src="https://raw.githubusercontent.com/avnet-iotconnect/iotc-python-lite-sdk-demos/main/common/media/create-template-button.png" width="600"/>

5. Click **Import**, and select the **`stm32n6wrt.json`** file you downloaded in Step 2:

   <img src="https://raw.githubusercontent.com/avnet-iotconnect/iotc-python-lite-sdk-demos/main/common/media/import-button.png" width="600"/>

> [!IMPORTANT]
> Use **`stm32n6wrt.json`** — it ships with the video-streaming properties and the AI telemetry
> attributes (`ai_people`, `ai_top_conf`, `ai_infer_ms`) plus the `LCD_ON`/`LCD_OFF` commands
> already configured.

## 7. Provision the Device (Serial)

The device generates its own key pair and certificate — nothing secret ever leaves the board.
There are two ways to do this:

### Option A — scripted (Windows, cloned repo)

If you cloned the repository on Windows, [bin/provision.ps1](bin/provision.ps1) automates this
step **and Step 9** end-to-end:

```powershell
cd bin
.\provision.ps1
```

The script auto-detects the board's COM port, reads the `thing_name` (and lets you keep or change
it), generates the key pair and certificate on-device, then **prints the certificate and pauses**.
While it waits: do [Step 8](#8-create-the-device-in-iotconnect) in the /IOTCONNECT console, then
paste the downloaded device configuration JSON back into the script (end the paste with `ENDJSON`
on its own line). It finishes by writing the connection config — you're prompted for Wi-Fi credentials (or they're
pre-filled from `bin/config.json` if you created one; Ethernet variant: the Wi-Fi values are
ignored by the firmware, enter anything) — then it imports the root CAs, commits, and resets the
board. **Then skip Step 9 entirely and go to [Step 10](#10-verify-the-demo).**

> [!NOTE]
> `bin\run_all.ps1` chains flashing (Step 4) and provisioning into a single script if you'd rather
> do everything in one shot. The provisioning scripts are Windows-only (they rely on Windows
> serial-port APIs) — on Linux, use Option B.

### Option B — manual (Windows or Linux)

1. Open a serial terminal on the board's ST-LINK virtual COM port at **115200 8N1**:
   - **Windows:** Device Manager → *Ports* → "STMicroelectronics STLink Virtual COM Port" → open
     that COM port in Tera Term/PuTTY
   - **Linux:** `picocom -b 115200 /dev/ttyACM0` (try `ttyACM1` if not found)
2. Read the board's identity — note the value, you'll use it as the device's **Unique ID**:
   ```
   conf get thing_name
   ```
3. Generate the on-device key pair and certificate:
   ```
   pki generate key tls_key_pub tls_key_priv ec prime256v1
   pki generate cert tls_cert tls_key_priv
   ```
   The second command prints the certificate PEM (`-----BEGIN CERTIFICATE-----` …
   `-----END CERTIFICATE-----`). **Copy the whole block** — you'll paste it into /IOTCONNECT next.

## 8. Create the Device in /IOTCONNECT

1. Back on the **Device** page, click **Create Device**:

   <img src="https://raw.githubusercontent.com/avnet-iotconnect/iotc-python-lite-sdk-demos/main/common/media/create-device-button.png" width="600"/>

2. Set **Unique ID** *and* **Display Name** to the `thing_name` value from Step 7:

   <img src="https://raw.githubusercontent.com/avnet-iotconnect/iotc-python-lite-sdk-demos/main/common/media/device-name.png" width="600"/>

3. Select your **Entity**:

   <img src="https://raw.githubusercontent.com/avnet-iotconnect/iotc-python-lite-sdk-demos/main/common/media/select-entity.png" width="600"/>

4. Select the template you imported (**STM32N6 W6X KVS WebRTC 2**):

   <img src="https://raw.githubusercontent.com/avnet-iotconnect/iotc-python-lite-sdk-demos/main/common/media/template-name.png" width="600"/>

5. Under the certificate section choose **Use my certificate**, and paste the certificate PEM from
   Step 7:

   <img src="https://raw.githubusercontent.com/avnet-iotconnect/iotc-python-lite-sdk-demos/main/common/media/use-my-cert.png" width="600"/>

6. Under the streaming settings, set:
   - **Stream Type**: `Module Based`
   - **Stream Resource**: `WebRTC`
   - **Auto Start Video Stream**: leave **off** — you start the stream on demand from the console.
7. Click **Save & View**:

   <img src="https://raw.githubusercontent.com/avnet-iotconnect/iotc-python-lite-sdk-demos/main/common/media/save-and-view.png" width="600"/>

8. On the device's page, click the **paper-and-cog icon** (near *Connection Info*) to download the
   **device configuration JSON** — you need three values from it in the next step:

   <img src="https://raw.githubusercontent.com/avnet-iotconnect/iotc-python-lite-sdk-demos/main/common/media/paper-and-cog.png" width="600"/>

## 9. Configure the Device Connection (Serial)

> [!NOTE]
> **Used `provision.ps1` (Option A in Step 7)?** This step is already done — the script wrote
> these settings and reset the board. Jump to [Step 10](#10-verify-the-demo).

Back in the serial terminal, write the connection settings. Take `pf` (→ `aws`), `cpid`, and `env`
from the device configuration JSON you just downloaded:

```
conf set broker_type iotconnect
conf set iotc_cloud aws
conf set iotc_cpid <your-cpid>
conf set iotc_env <your-env>
conf set iotc_app_mode demo
conf set iotc_identity_json
conf set iotc_cache_valid 0
```

**Wi-Fi variant only**, add your network credentials:

```
conf set wifi_ssid <your-wifi-ssid>
conf set wifi_credential <your-wifi-password>
```

> [!IMPORTANT]
> `conf set iotc_identity_json` is intentionally set with **no value** — it clears any cached
> identity. Do **not** paste the downloaded JSON here; the firmware fetches its identity at runtime
> from `cpid`/`env` + its on-device certificate.

Then commit and reboot:

```
conf commit
reset
```

## 10. Verify the Demo

1. Watch the serial terminal: within ~60 seconds the device connects to /IOTCONNECT
   (`[IOTC]` log lines), and the board's **LCD lights up with the live camera preview** — people in
   view get detection boxes drawn on them.
2. In the /IOTCONNECT console the device now shows **Connected**. Open the device's **Live Data**
   tab: telemetry arrives every few seconds, including the AI attributes:

   | Attribute     | Meaning                                          |
   |---------------|--------------------------------------------------|
   | `ai_people`   | People detected in frame (NPU YOLOv2)            |
   | `ai_top_conf` | Highest detection confidence (%)                 |
   | `ai_infer_ms` | NPU inference time (ms)                          |

3. Open the device's **Video Streaming** tab and click **Start** — after a few seconds the live
   camera stream appears in your browser, with AI detection running on-device simultaneously.
4. Try the cloud commands (**Command** tab): `LCD_ON` / `LCD_OFF` toggles the board's LCD preview;
   `LED_RED_ON`, `LED_GREEN_ON`, etc. control the user LEDs.

**That's the full demo: live WebRTC video + on-device AI + cloud telemetry and commands.**

## 11. Troubleshooting

| Symptom                                            | Fix                                                                                                                                             |
|----------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------|
| CubeProgrammer fails to program / verify           | Confirm external loader **MX66UW1G45G** checked (and *only* that one — deselect OTP_FUSES), board in **Dev Boot** (LED2 on)                      |
| Board does nothing after flashing                  | Boot switches back to Flash mode (BOOT1 **left**, BOOT0 **left**), then power-cycle                                                              |
| `libusb ... errno=13` on Linux                     | Install the udev rules (Step 2 note), replug the board                                                                                           |
| Wi-Fi variant never gets an IP                     | Re-check Step 5 module update completed (needs **V1.3.0+**); check SSID/password; 802.1x enterprise networks are not supported                   |
| Device connects but video **Start** stays black    | Click **Stop**, wait ~10 s, click **Start** again (a known Wi-Fi TURN-relay quirk — see [docs/w6x_module_notes.md](docs/w6x_module_notes.md))    |
| Telemetry shows but no `ai_*` attributes           | Template mismatch — confirm the device uses the imported **stm32n6wrt.json** template                                                            |

More depth: [docs/troubleshooting.md](docs/troubleshooting.md)

## 12. Resources

- [Full project readme](readme.md) — building from source, architecture, provisioning details
- [docs/architecture.md](docs/architecture.md) — firmware architecture
- [docs/ethernet_support.md](docs/ethernet_support.md) — how the Ethernet transport works
- [docs/w6x_module_notes.md](docs/w6x_module_notes.md) — Wi-Fi module design notes & known limitations
- [/IOTCONNECT overview](https://www.iotconnect.io/) · [/IOTCONNECT documentation](https://docs.iotconnect.io/)
- [STM32N6570-DK product page](https://www.st.com/en/evaluation-tools/stm32n6570-dk.html)

## Revision Info

| Revision | Date       | Notes                                        |
|----------|------------|----------------------------------------------|
| 1.0      | 2026-07-26 | Initial QuickStart (Wi-Fi + Ethernet images) |
