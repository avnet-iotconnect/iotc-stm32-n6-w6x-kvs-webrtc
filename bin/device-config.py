#!/usr/bin/env python3
"""
device-config.py — Linux/macOS counterpart to device-config.ps1.

Automates board-side IOTCONNECT provisioning: generates an on-device key pair
and self-signed certificate, walks you through creating the device in the
IOTCONNECT web UI, then writes the resulting connection config (and your
Wi-Fi credentials) to the board over its ST-LINK virtual COM port.

This mirrors device-config.ps1's behavior and prompts field-for-field. Windows
users should keep using device-config.ps1 (or run_all.ps1); this script exists
so Linux/macOS hosts get the same one-command automated flow instead of
typing the CLI commands from the readme by hand.

Requirements:
    Python 3.8+
    pyserial   (pip install pyserial)

Usage:
    cd bin
    python3 device-config.py
"""

import json
import os
import re
import sys
import time
import urllib.parse
from pathlib import Path

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit(
        "Error: the 'pyserial' package is required.\n"
        "Install it with: pip install pyserial"
    )

SCRIPT_DIR = Path(__file__).resolve().parent
CONFIG_PATH = SCRIPT_DIR / "config.json"
BAUD_RATE = 115200

# STMicroelectronics USB vendor ID — used to auto-detect the ST-LINK virtual
# COM port regardless of which STLINK model (V2/V3) is attached.
ST_USB_VID = 0x0483

# ------------------------------------------------------------------
# Root CA certificates (identical to the PEMs embedded in device-config.ps1)
# ------------------------------------------------------------------

AWS_MQTT_ROOT_CA_PEM = """-----BEGIN CERTIFICATE-----
MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF
ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6
b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL
MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv
b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj
ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM
9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw
IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6
VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L
93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm
jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC
AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA
A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI
U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs
N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv
o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU
5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy
rqXRfboQnoZsG4q5WTP468SQvvG5
-----END CERTIFICATE-----"""

AZURE_MQTT_ROOT_CA_PEM = """-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz
tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ
vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV
5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY
1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4
NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG
Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91
8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe
pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----"""

IOTCONNECT_DRA_CA_PEM = """-----BEGIN CERTIFICATE-----
MIIE0DCCA7igAwIBAgIBBzANBgkqhkiG9w0BAQsFADCBgzELMAkGA1UEBhMCVVMx
EDAOBgNVBAgTB0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxGjAYBgNVBAoT
EUdvRGFkZHkuY29tLCBJbmMuMTEwLwYDVQQDEyhHbyBEYWRkeSBSb290IENlcnRp
ZmljYXRlIEF1dGhvcml0eSAtIEcyMB4XDTExMDUwMzA3MDAwMFoXDTMxMDUwMzA3
MDAwMFowgbQxCzAJBgNVBAYTAlVTMRAwDgYDVQQIEwdBcml6b25hMRMwEQYDVQQH
EwpTY290dHNkYWxlMRowGAYDVQQKExFHb0RhZGR5LmNvbSwgSW5jLjEtMCsGA1UE
CxMkaHR0cDovL2NlcnRzLmdvZGFkZHkuY29tL3JlcG9zaXRvcnkvMTMwMQYDVQQD
EypHbyBEYWRkeSBTZWN1cmUgQ2VydGlmaWNhdGUgQXV0aG9yaXR5IC0gRzIwggEi
MA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC54MsQ1K92vdSTYuswZLiBCGzD
BNliF44v/z5lz4/OYuY8UhzaFkVLVat4a2ODYpDOD2lsmcgaFItMzEUz6ojcnqOv
K/6AYZ15V8TPLvQ/MDxdR/yaFrzDN5ZBUY4RS1T4KL7QjL7wMDge87Am+GZHY23e
cSZHjzhHU9FGHbTj3ADqRay9vHHZqm8A29vNMDp5T19MR/gd71vCxJ1gO7GyQ5HY
pDNO6rPWJ0+tJYqlxvTV0KaudAVkV4i1RFXULSo6Pvi4vekyCgKUZMQWOlDxSq7n
eTOvDCAHf+jfBDnCaQJsY1L6d8EbyHSHyLmTGFBUNUtpTrw700kuH9zB0lL7AgMB
AAGjggEaMIIBFjAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBBjAdBgNV
HQ4EFgQUQMK9J47MNIMwojPX+2yz8LQsgM4wHwYDVR0jBBgwFoAUOpqFBxBnKLbv
9r0FQW4gwZTaD94wNAYIKwYBBQUHAQEEKDAmMCQGCCsGAQUFBzABhhhodHRwOi8v
b2NzcC5nb2RhZGR5LmNvbS8wNQYDVR0fBC4wLDAqoCigJoYkaHR0cDovL2NybC5n
b2RhZGR5LmNvbS9nZHJvb3QtZzIuY3JsMEYGA1UdIAQ/MD0wOwYEVR0gADAzMDEG
CCsGAQUFBwIBFiVodHRwczovL2NlcnRzLmdvZGFkZHkuY29tL3JlcG9zaXRvcnkv
MA0GCSqGSIb3DQEBCwUAA4IBAQAIfmyTEMg4uJapkEv/oV9PBO9sPpyIBslQj6Zz
91cxG7685C/b+LrTW+C05+Z5Yg4MotdqY3MxtfWoSKQ7CC2iXZDXtHwlTxFWMMS2
RJ17LJ3lXubvDGGqv+QqG+6EnriDfcFDzkSnE3ANkR/0yBOtg2DZ2HKocyQetawi
DsoXiWJYRBuriSUBAA/NxBti21G00w9RKpv0vHP8ds42pM3Z2Czqrpv1KrKQ0U11
GIo/ikGQI31bS/6kA1ibRrLDYGCD+H1QQc7CoZDDu+8CL9IVVO5EFdkKrqeKM+2x
LXY2JtwE65/3YR8V3Idv7kaWKK2hJn0KCacuBKONvPi8BDAB
-----END CERTIFICATE-----"""


class ProvisionError(Exception):
    """Raised for any provisioning failure; caught once at the top level."""


# ------------------------------------------------------------------
# Helpers
# ------------------------------------------------------------------

def read_config_value_or_prompt(current_value, prompt, default=""):
    effective_default = current_value if current_value else default
    suffix = f" [{effective_default}]" if effective_default else ""
    value = input(f"{prompt}{suffix}: ").strip()
    return value if value else effective_default


def resolve_board_serial_port():
    env_port = os.environ.get("STM32_CLI_PORT", "").strip()
    if env_port:
        return env_port

    candidates = [
        p for p in serial.tools.list_ports.comports() if p.vid == ST_USB_VID
    ]

    if len(candidates) == 1:
        return candidates[0].device

    if len(candidates) > 1:
        listing = "\n".join(f"  {p.device}  ({p.description})" for p in candidates)
        raise ProvisionError(
            "Multiple ST-LINK serial ports detected:\n"
            f"{listing}\n"
            "Disconnect the extra board(s), or set STM32_CLI_PORT to the "
            "right device (e.g. /dev/ttyACM0), then rerun the script."
        )

    raise ProvisionError(
        "Could not auto-detect the ST-LINK virtual COM port. Connect the "
        "board, make sure it enumerated (check `ls /dev/ttyACM*` or "
        "`dmesg | tail`), and rerun the script. You can also set "
        "STM32_CLI_PORT explicitly (e.g. STM32_CLI_PORT=/dev/ttyACM0)."
    )


def open_board_serial_port(port_name, baud_rate):
    try:
        ser = serial.Serial(
            port=port_name,
            baudrate=baud_rate,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.5,
            write_timeout=0.5,
        )
        ser.dtr = True
        ser.rts = True
    except serial.SerialException as exc:
        message = str(exc)
        if "Permission denied" in message:
            raise ProvisionError(
                f"Permission denied opening '{port_name}'. On Linux, add your "
                "user to the 'dialout' group (sudo usermod -aG dialout $USER, "
                "then log out/in) or run with sudo. On macOS, check System "
                "Settings > Privacy for terminal serial access."
            ) from exc
        raise ProvisionError(
            f"Serial port '{port_name}' could not be opened: {message}\n"
            "Close any terminal or tool using it, confirm the board is "
            "connected, then rerun device-config.py."
        ) from exc

    time.sleep(0.3)
    return ser


def open_board_serial_port_with_retry(port_name, baud_rate):
    while True:
        try:
            return open_board_serial_port(port_name, baud_rate)
        except ProvisionError as exc:
            print()
            print(str(exc))
            retry = input(
                f"Close the serial terminal using {port_name}, then press "
                "Enter to retry or type q to quit: "
            ).strip().lower()
            if retry in ("q", "quit"):
                raise ProvisionError("Provisioning canceled by user.") from exc


def read_until_quiet(ser, initial_delay=0.3, idle_s=0.7, max_wait_s=6.0):
    response = ""
    time.sleep(initial_delay)

    overall_start = time.monotonic()
    idle_start = time.monotonic()

    while (time.monotonic() - overall_start) < max_wait_s:
        waiting = ser.in_waiting
        if waiting:
            response += ser.read(waiting).decode("utf-8", errors="replace")
            idle_start = time.monotonic()
        elif (time.monotonic() - idle_start) >= idle_s:
            break
        time.sleep(0.05)

    return response


def send_command(ser, command, timeout_s=1.5):
    ser.write((command + "\r\n").encode("utf-8"))

    response = ""
    start = time.monotonic()
    while (time.monotonic() - start) < timeout_s:
        waiting = ser.in_waiting
        if waiting:
            response += ser.read(waiting).decode("utf-8", errors="replace")
        time.sleep(0.05)

    return response


def wait_for_cli_ready(ser):
    for _ in range(12):
        response = send_command(ser, "", timeout_s=1.2)
        if response.strip():
            return

        response = send_command(ser, "conf get broker_type", timeout_s=1.8)
        if response.strip():
            return

        time.sleep(1.0)

    raise ProvisionError(
        "Device CLI did not respond. Make sure the board is running the "
        "project app in Flash Boot."
    )


def assert_response_contains(response, expected_text, operation):
    if expected_text not in response:
        trimmed = response.strip() or "<no response>"
        raise ProvisionError(
            f"{operation} did not return the expected text '{expected_text}'. "
            f"Device response: {trimmed}"
        )


def extract_pem_block(text, begin_marker, end_marker):
    pattern = re.escape(begin_marker) + ".*?" + re.escape(end_marker)
    match = re.search(pattern, text, re.DOTALL)
    if not match:
        raise ProvisionError(
            f"Failed to extract PEM block between '{begin_marker}' and "
            f"'{end_marker}'."
        )
    return match.group(0).strip()


def ensure_output_directory():
    path = SCRIPT_DIR / "generated_iotconnect_identity"
    path.mkdir(exist_ok=True)
    return path


def get_thing_name(ser):
    response = send_command(ser, "conf get thing_name", timeout_s=3.0)
    match = re.search(r'thing_name\s*=\s*"([^"]+)"', response)
    if not match:
        raise ProvisionError(
            f"Failed to read thing_name from device. Response: {response.strip()}"
        )
    return match.group(1)


def set_config_value(ser, key, value=""):
    if value:
        send_command(ser, f"conf set {key} {value}")
    else:
        send_command(ser, f"conf set {key} ")


def send_text_content(ser, text):
    ser.write((text + "\r\n").encode("utf-8"))
    return read_until_quiet(ser, initial_delay=0.4, idle_s=0.9, max_wait_s=7.0)


def parse_iotconnect_device_config_json(json_text):
    device_config = json.loads(json_text)

    platform = str(device_config.get("pf", "")).lower()
    if platform == "aws":
        backend = "aws"
    elif platform in ("az", "azure"):
        backend = "azure"
    else:
        raise ProvisionError(f"Unsupported pf value '{platform}'. Expected 'aws' or 'az'.")

    cpid = str(device_config.get("cpid", ""))
    env_name = str(device_config.get("env", ""))
    uid = str(device_config.get("uid", ""))
    did = str(device_config.get("did", ""))
    disc = str(device_config.get("disc", ""))

    if not cpid or not env_name or (not uid and not did):
        raise ProvisionError(
            "Device JSON is missing required values. Expected cpid, env, "
            "and uid or did."
        )

    if not uid:
        uid = did

    return {
        "backend": backend,
        "cpid": cpid,
        "env": env_name,
        "uid": uid,
        "did": did,
        "discovery_url": disc,
    }


def validate_discovery_url(backend, discovery_url):
    if not discovery_url:
        return

    parsed = urllib.parse.urlparse(discovery_url)
    if not parsed.scheme or not parsed.netloc:
        raise ProvisionError(f"The device JSON contains an invalid discovery URL: {discovery_url}")

    if parsed.scheme != "https":
        raise ProvisionError(f"The device JSON discovery URL must use https: {discovery_url}")

    discovery_host = parsed.hostname.lower() if parsed.hostname else ""
    if backend == "aws":
        if discovery_host not in ("awsdiscovery.iotconnect.io", "discovery.iotconnect.io"):
            raise ProvisionError(f"Unexpected AWS discovery host '{discovery_host}' in device JSON.")
    elif backend == "azure":
        if discovery_host != "discovery.iotconnect.io":
            raise ProvisionError(f"Unexpected Azure discovery host '{discovery_host}' in device JSON.")


def read_pasted_json():
    print()
    print("Paste the IOTCONNECT device JSON now.")
    print("When you are finished, type ENDJSON on its own line and press Enter.")
    print()

    lines = []
    while True:
        line = input()
        if line == "ENDJSON":
            break
        lines.append(line)

    json_text = "\n".join(lines).strip()
    if not json_text:
        raise ProvisionError("No IOTCONNECT device JSON was pasted.")
    return json_text


def validate_thing_name(thing_name):
    if re.search(r"\s", thing_name):
        raise ProvisionError("thing_name must not contain whitespace.")
    if not thing_name:
        raise ProvisionError("thing_name must not be empty.")


def show_ui_instructions(thing_name, certificate_pem, saved_cert_path):
    print()
    print("=" * 60)
    print(" IOTCONNECT UI STEPS")
    print("=" * 60)
    print("1. Create a new device in IOTCONNECT.")
    print("2. Unique Id:")
    print(f"   {thing_name}")
    print("3. Device Name:")
    print(f"   {thing_name}")
    print("4. Select the appropriate Entity.")
    print("5. Select Template:")
    print("   STM32N6 W6X KVS WebRTC 2")
    print("6. In Device certificate, select ONLY:")
    print("   Use my certificate")
    print("   Do not select Auto-generated or any other certificate option.")
    print("7. Paste the certificate below into the 'Certificate Text' box.")
    print("   Copy the full PEM including:")
    print("   -----BEGIN CERTIFICATE-----")
    print("   -----END CERTIFICATE-----")
    print("8. Click 'Save & View'.")
    print("9. On the device page, click the document + gear icon in the top right to download the device JSON.")
    print()
    print("A copy of the certificate was saved to:")
    print(f"  {saved_cert_path}")
    print()
    print("Certificate to paste into IOTCONNECT:")
    print(certificate_pem)


def load_config():
    if CONFIG_PATH.exists():
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            return json.load(f)
    return {}


# ------------------------------------------------------------------
# Main
# ------------------------------------------------------------------

def main():
    config = load_config()

    wifi_ssid = read_config_value_or_prompt(config.get("wifi_ssid", ""), "Wi-Fi SSID")
    wifi_credential = read_config_value_or_prompt(
        config.get("wifi_credential", ""), "Wi-Fi credential/password"
    )

    port_name = resolve_board_serial_port()
    app_mode = "demo"

    print()
    print(f"Using board serial port: {port_name}")

    output_directory = ensure_output_directory()
    ser = None

    try:
        ser = open_board_serial_port_with_retry(port_name, BAUD_RATE)
        wait_for_cli_ready(ser)

        # Step 1: Thing name
        device_thing_name = get_thing_name(ser)
        print()
        print(f"Detected device thing_name: {device_thing_name}")

        thing_name_input = input(
            "Press Enter to keep this thing_name, or type a new one: "
        ).strip()
        if thing_name_input:
            validate_thing_name(thing_name_input)
            set_config_value(ser, "thing_name", thing_name_input)
            send_command(ser, "conf commit")
            device_thing_name = thing_name_input
            print(f"Updated thing_name to: {device_thing_name}")

        # Step 2: Generate device key pair and self-signed certificate
        generate_key_response = send_command(
            ser, "pki generate key tls_key_pub tls_key_priv ec prime256v1", timeout_s=12.0
        )
        assert_response_contains(generate_key_response, "SUCCESS:", "Device key generation")

        cert_response = send_command(ser, "pki generate cert tls_cert tls_key_priv", timeout_s=15.0)
        cert_pem = extract_pem_block(
            cert_response, "-----BEGIN CERTIFICATE-----", "-----END CERTIFICATE-----"
        )

        cert_path = output_directory / f"{device_thing_name}.iotconnect.cert.pem"
        cert_path.write_text(cert_pem + "\n", encoding="ascii")

        # Step 3: Show IoTConnect UI instructions
        show_ui_instructions(device_thing_name, cert_pem, cert_path)

        print()
        print("After creating the device and downloading the IOTCONNECT device JSON, return here.")
        input("Press Enter when you are ready to paste the device JSON: ")

        # Step 4: Parse device JSON
        pasted_json = read_pasted_json()
        json_save_path = output_directory / f"{device_thing_name}.iotcDeviceConfig.json"
        json_save_path.write_text(pasted_json + "\n", encoding="utf-8")

        device_config_info = parse_iotconnect_device_config_json(pasted_json)
        validate_discovery_url(device_config_info["backend"], device_config_info["discovery_url"])

        if device_config_info["uid"] != device_thing_name:
            raise ProvisionError(
                f"IOTCONNECT device JSON uid '{device_config_info['uid']}' does "
                f"not match thing_name '{device_thing_name}'."
            )

        print()
        print("Device JSON accepted.")
        print(f"  pf/backend : {device_config_info['backend']}")
        print(f"  cpid       : {device_config_info['cpid']}")
        print(f"  env        : {device_config_info['env']}")
        print(f"  uid        : {device_config_info['uid']}")
        if device_config_info["did"]:
            print(f"  did        : {device_config_info['did']}")
            expected_did_forms = {
                device_thing_name,
                f"{device_config_info['cpid']}-{device_thing_name}",
            }
            if device_config_info["did"] not in expected_did_forms:
                print(
                    f"Warning: device JSON did '{device_config_info['did']}' does not "
                    f"match the usual forms '{device_thing_name}' or "
                    f"'{device_config_info['cpid']}-{device_thing_name}'. Continuing "
                    "because UID is the authoritative board identity."
                )
        if device_config_info["discovery_url"]:
            print(f"  disc       : {device_config_info['discovery_url']}")
            print(
                "Note: firmware derives discovery from pf/cpid/env; the discovery "
                "URL is validated for reference, not stored as a separate conf key."
            )
        print(f"  saved json : {json_save_path}")

        # Step 5: Import root CA certificates
        mqtt_root_ca_text = (
            AZURE_MQTT_ROOT_CA_PEM if device_config_info["backend"] == "azure" else AWS_MQTT_ROOT_CA_PEM
        )

        send_command(ser, "pki import cert root_ca_cert")
        root_ca_response = send_text_content(ser, mqtt_root_ca_text)
        assert_response_contains(root_ca_response, "Success:", "MQTT root CA import")

        send_command(ser, "pki import cert iotconnect_dra_ca")
        dra_ca_response = send_text_content(ser, IOTCONNECT_DRA_CA_PEM)
        assert_response_contains(dra_ca_response, "Success:", "IOTCONNECT DRA CA import")

        # Verify certificate and key are present on device
        cert_verify_response = send_command(ser, "pki export cert tls_cert", timeout_s=5.0)
        assert_response_contains(
            cert_verify_response, "-----BEGIN CERTIFICATE-----", "On-device certificate verification"
        )

        pub_key_response = send_command(ser, "pki export key tls_key_pub", timeout_s=5.0)
        assert_response_contains(
            pub_key_response, "-----BEGIN PUBLIC KEY-----", "On-device public key verification"
        )

        # Step 6: Set IoTConnect configuration
        set_config_value(ser, "broker_type", "iotconnect")
        set_config_value(ser, "iotc_cloud", device_config_info["backend"])
        set_config_value(ser, "iotc_cpid", device_config_info["cpid"])
        set_config_value(ser, "iotc_env", device_config_info["env"])
        set_config_value(ser, "iotc_app_mode", app_mode)
        set_config_value(ser, "iotc_identity_json", "")
        set_config_value(ser, "wifi_ssid", wifi_ssid)
        set_config_value(ser, "wifi_credential", wifi_credential)
        send_command(ser, "conf set iotc_cache_valid 0")

        # KVS WebRTC configuration is automatic — the firmware parses the
        # IoTConnect identity response "vs" block at runtime to get the
        # signaling channel ARN, credentials endpoint, and role alias.
        # No manual KVS conf set needed.

        # Step 7: Commit and reset
        send_command(ser, "conf commit")

        print()
        print("Board configured for IOTCONNECT.")
        print("KVS WebRTC will auto-configure from the IoTConnect device template")
        print("(requires a template with Video Streaming enabled, e.g. plitekvs).")
        print("Resetting device now...")
        time.sleep(1)
        send_command(ser, "reset")

        print()
        print("Next:")
        print("1. Let the board reboot.")
        print("2. Wait up to 60 seconds for the device to connect to IOTCONNECT.")
        print("3. If the IoTConnect template has Video Streaming enabled, KVS WebRTC")
        print("   will auto-connect. View the stream in the KVS console or IoTConnect UI.")
        print(f"4. Monitor {port_name} in your serial terminal at 115200 8N1.")
        print()
        print("Provisioning completed. Releasing the serial port now.")

    except ProvisionError as exc:
        print()
        print(str(exc))
        sys.exit(1)
    except KeyboardInterrupt:
        print()
        print("Provisioning canceled by user.")
        sys.exit(1)
    finally:
        if ser is not None and ser.is_open:
            ser.close()
            print("Serial port closed.")


if __name__ == "__main__":
    main()
