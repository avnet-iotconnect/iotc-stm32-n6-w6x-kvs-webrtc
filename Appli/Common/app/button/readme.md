# MQTT Button Status Example (button_task.c)

This example publishes USER button state to MQTT whenever the button changes.

## MQTT Topic

- Report topic: `<thing_name>/sensor/button/reported`

Example thing name:
- `stm32n6-002C005B3332511738363236`

## Report Payload

Button released:

```json
{
  "buttonStatus": {
    "USER_Button": {
      "reported": "OFF"
    }
  }
}
```

Button pressed:

```json
{
  "buttonStatus": {
    "USER_Button": {
      "reported": "ON"
    }
  }
}
```

## Monitor Messages

1. Subscribe to `<thing_name>/sensor/button/reported`
2. Press/release USER button
3. Observe state updates

Button state is reported via IOTCONNECT telemetry. Monitor events in the IOTCONNECT dashboard.

## Firmware Notes

`vButtonTask()`:
- registers GPIO rising/falling callbacks
- waits on FreeRTOS event bits
- publishes consolidated JSON button state

State mapping is based on `USER_BUTTON_ON` configuration in firmware.
