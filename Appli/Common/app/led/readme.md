# MQTT LED Control Example (led_task.c)

This example shows how to control onboard LEDs over MQTT and report current LED state back to the broker.

## MQTT Topics

- Command topic: `<thing_name>/led/desired`
- Report topic: `<thing_name>/led/reported`

Example thing name:
- `stm32n6-002C005B3332511738363236`

## Command Payloads

### JSON command (recommended)

Turn LED red ON:

```json
{
  "ledStatus": {
    "LED_RED": {
      "desired": "ON"
    }
  }
}
```

Turn LED red OFF:

```json
{
  "ledStatus": {
    "LED_RED": {
      "desired": "OFF"
    }
  }
}
```

### Raw command fallback (supported)

- `LED_RED_ON`
- `LED_RED_OFF`
- `LED_GREEN_ON`
- `LED_GREEN_OFF`

## Report Payload

The firmware publishes LED status as JSON:

```json
{
  "ledStatus": {
    "LED_RED": {
      "reported": "ON"
    },
    "LED_GREEN": {
      "reported": "OFF"
    }
  }
}
```

## Monitor and Test

1. Subscribe to `<thing_name>/led/reported`
2. Publish control payloads to `<thing_name>/led/desired`

LED control is managed via IOTCONNECT cloud-to-device commands. LED state is reported back via IOTCONNECT telemetry. Monitor and control LEDs from the IOTCONNECT dashboard.

## Firmware Notes

`vLEDTask()`:
- waits for MQTT readiness
- subscribes to desired topic
- applies LED updates
- publishes reported state

The parser supports both JSON and raw-string command formats.
