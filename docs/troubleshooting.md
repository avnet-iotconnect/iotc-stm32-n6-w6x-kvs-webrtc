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
3. For this repo's signing command, STM32CubeProgrammer `2.20.x` is known-good.
4. STM32CubeProgrammer `2.21.0+` changed STM32N6 signing behavior and requires `-align` / `--align` for STM32N6 images.

References:

- https://community.st.com/t5/stm32cubeprogrammer-mcus/signingtool-for-stm32n6-in-stm32cubeprogrammer-v2-21-0/td-p/859154
- https://community.st.com/t5/stm32cubeprogrammer-mcus/no-padding-align-with-padding-in-stm32-signingtool-cli-2-21/td-p/859110

## Debug Load Issues After STM32CubeMX Regeneration

STM32CubeMX can overwrite boot files used by the debug flow.

Verify and re-apply in `Middlewares/ST/STM32_ExtMem_Manager/boot/stm32_boot_lrun.c`:

```c
#if !defined(_DEBUG_)
    retr = CopyApplication();
#endif
```

Then run `update.sh` and restart debug.

## IoTConnect Device Not Connecting

Check:

1. Wi-Fi credentials in `bin/config.json` are correct.
2. The pasted device JSON belongs to the created device.
3. `Unique Id` in IoTConnect matches the board `thing_name`.
4. Rerun `bin\provision_iotconnect.ps1` to reprovision without reflashing.
