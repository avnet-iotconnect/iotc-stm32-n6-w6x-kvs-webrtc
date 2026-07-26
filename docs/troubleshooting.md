# Troubleshooting

Common issues and practical fixes.

## COM Port Busy / Access Denied

Symptoms:

- Serial port open fails
- `Access to the port 'COMxx' is denied`

Fix:

1. Close other serial tools using the same COM port (PuTTY, TeraTerm, VS Code serial monitor, etc.).
2. Rerun `bin\run_all.ps1` or the provisioning script.

## Board Does Not Boot or Debug

Check:

1. Board mode is correct for the current step (Dev mode for debug/flash sequence).
2. If board mode changed mid-session, reflash and retry.

## Signed Application Does Not Boot

Symptoms:

- Flash appears to succeed
- FSBL starts, but the signed application does not boot correctly
- Behavior changes depending on STM32CubeProgrammer version

Check:

1. If you built from STM32CubeIDE, run `bin\copy_hex_from_project.ps1` before `bin\run_all.ps1` or `bin\flash.ps1`.
2. Confirm the staged files under `bin\Appli\...` and `bin\FSBL\...` match the build configuration you actually built.
3. STM32CubeProgrammer `2.21.0+` no longer auto-pads STM32N6 payloads to the `0x400` offset —
   the `-align` / `--align` flag is **mandatory** there (ST errata). Without it, flash and verify
   succeed but the image **silently never boots**: the payload lands at file offset `0x240` while
   the header entry point and vectors are linked for `0x400`, so the BootROM jumps into misplaced
   bytes. `2.20.x` pads automatically and has no `-align` flag.
4. `bin\flash.ps1` handles this automatically: it detects `-align` support in the installed
   signing tool's help text and appends the flag when present. If signing manually, add `-align`
   on `2.21.0+` only.
5. Root cause confirmed empirically (2026-07-26): a `2.23.0 -align` image is byte-identical to a
   `2.20.0` image outside the random-fill padding extension (`0xA8`–`0x23F`, which differs even
   between two consecutive runs of the same version). Both signing paths and `2.23.0` programming
   validated on-board.

References:

- ST errata ("-align mandatory with STM32N6 from v2.21.0"): https://wiki.st.com/stm32mcu/wiki/STM32CubeProgrammer_errata_2.23.x
- ST announcement: https://community.st.com/t5/stm32cubeprogrammer-mcus/signingtool-for-stm32n6-in-stm32cubeprogrammer-v2-21-0/td-p/859154
- https://community.st.com/t5/stm32cubeprogrammer-mcus/no-padding-align-with-padding-in-stm32-signingtool-cli-2-21/td-p/859110
- Same regression + fix in Zephyr: https://github.com/zephyrproject-rtos/zephyr/issues/99456

## Debug Load Issues After STM32CubeMX Regeneration

STM32CubeMX can overwrite boot files used by the debug flow.

Verify and re-apply in `Middlewares/ST/STM32_ExtMem_Manager/boot/stm32_boot_lrun.c`:

```c
#if !defined(_DEBUG_)
    retr = CopyApplication();
#endif
```

Then run `update.sh` and restart debug.

## IOTCONNECT Device Not Connecting

Check:

1. Wi-Fi credentials in `bin/config.json` are correct.
2. The pasted device JSON belongs to the created device.
3. `Unique Id` in IOTCONNECT matches the board `thing_name`.
4. Rerun `bin\provision.ps1` to reprovision without reflashing.

## KVS WebRTC Video Not Streaming

Check:

1. **ST67W611M module firmware and X-CUBE-ST67W61 middleware must be V1.3.0 or later.** Earlier versions have a WAN UDP receive bug that prevents WebRTC ICE/TURN relay negotiation from completing. The device will connect to IOTCONNECT but video streaming will fail.
2. The IOTCONNECT device template has **Video Streaming** enabled.
3. Serial log shows `[KVSWebRTC]` lines — if missing, the KVS task did not start.
4. If using an external 5V supply, ensure it is stable. USB power from ST-LINK alone can cause brownouts during streaming.
