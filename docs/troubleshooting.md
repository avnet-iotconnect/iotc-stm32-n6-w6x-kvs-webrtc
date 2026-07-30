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

## Wi-Fi Streaming Freezes or Stalls Mid-Session (Router UDP Port Filtering)

**Symptom:** On the Wi-Fi build, viewer sessions connect but the video freezes
after ~20-90 s with `[TLS] snd stall ... e=11` then `GATE CLOSE` in the serial
log, and boot-time NTP always falls back to HTTP (`[NTP] select timeout` for
every server, then `[HTTP] time set OK`). Streaming on the same firmware works
perfectly on other networks.

**Diagnosis (read it straight off the serial log):** during ICE gathering the
one-shot `[icn] relay ...` markers show what each TURN server did:

```
[icn] relay[1] ... proto=2 ... [icn] relay CREATED proto=2    <- TLS/TCP relay OK
[icn] relay[2] ... proto=1 ... [icn] relay IN_PROGRESS proto=1  <- UDP relay Allocate never answered
[icn] tcp-relay priority demoted
```

If the UDP relay (`proto=1`) never reaches `CREATED`, every session is forced
onto the TLS/TCP relay — whose media path can wedge under sustained load (the
known W6X TCP-TX stall, `docs/w6x_module_notes.md`). Compare the UDP ports the
network passes: DNS (UDP/53) resolves fine, NTP (UDP/123) gets no reply, and
the KVS TURN Allocate (UDP/443) gets no reply. That pattern is **router policy,
not firmware**: many routers/firewalls ship "QUIC blocking" or advanced-security
features that silently drop outbound UDP/443, and commonly intercept client NTP
(UDP/123) as well.

**What to do:**

1. **Fix the router (preferred).** In the router's security settings disable
   QUIC/UDP-443 blocking (or exempt the device's MAC/IP), and allow outbound
   NTP. Names vary: "QUIC filtering", "advanced security", "protected
   browsing", "UDP flood protection".
2. **Confirm it's the network** by booting the same firmware on a different
   LAN or hotspot: `[icn] relay CREATED proto=1` should appear and sessions
   run stall-free on the UDP relay (validated 127 s+, zero TLS stalls).
3. **Use the wired Ethernet image** (`bin/quickstart/...-ethernet.hex`) — its
   TCP path does not have the W6X TX stall, so it streams reliably even behind
   a filtering router. Credentials are preserved when swapping images.
4. **NTP-only relief:** the firmware already falls back to HTTP time (works
   everywhere, ~10 s slower boot). You can also point NTP elsewhere without
   reflashing: `conf set ntp_host <server>` / `conf set ntp_port <port>` at
   the CLI, stored in littlefs.

Even when stuck on the TLS/TCP relay the firmware self-heals: a wedged session
is torn down via the ICE close-timeout and the next viewer connect retries.
The TCP relay candidate is advertised at minimum ICE priority, so the moment
the network lets the UDP Allocate through, sessions automatically prefer the
reliable UDP relay.
