# ST67W6X (W6X / WL6) Wi-Fi Module — Design Notes & Known Issues

Design notes for the **ST67W611M** NCP (network co-processor) used as the
Wi-Fi transport on this project (X-NUCLEO-67W61M1 add-on, "W6X"/"WL6"). It is
an SPI-attached companion radio: the STM32N6 host runs the LwIP stack and the
**ST67W6X_Network_Driver** middleware, and the NCP handles 802.11 + BLE.

This file collects the module-level quirks we hit bringing up KVS WebRTC
streaming over Wi-Fi, why each matters, and how each is handled today. It is
transport-scoped: everything here is about the Wi-Fi path. The wired path
(`NET_USE_ETHERNET`, RTL8211F/RGMII) does **not** go through the W6X and is not
subject to these issues — see [ethernet_support.md](ethernet_support.md).

## Module identity (as tested)

| Field | Value |
|---|---|
| Host middleware | ST67W6X_Network_Driver **V1.3.0** |
| NCP SDK | 2.0.106 |
| Wi-Fi MAC | 1.6.44 |
| NCP build | Mar 20 2026 |
| NCP MAC | 40:82:7b:03:be:9c |
| Attach | SPI5 (W61 bus), single shared TX queue |

Upgrade history from the V1.2.0 tree is in
[../Middlewares/ST/ST67W6X_Network_Driver/V130_UPGRADE_NOTES.md](../Middlewares/ST/ST67W6X_Network_Driver/V130_UPGRADE_NOTES.md).
A V1.2.0 backup is preserved at `Middlewares/ST/ST67W6X_Network_Driver.v120_backup/`.

---

## 1. WAN-sourced UDP receive black-hole (the "UDP on the wrong channel" issue)

**Symptom (V1.2.0 middleware):** LAN UDP was delivered 100%, but **UDP arriving
from the public internet was silently dropped inside the NCP** before it ever
reached the host. This looked like "UDP coming back on the wrong channel / not
coming back at all": DNS to public resolvers, NTP/123, and — critically —
**TURN `Allocate` responses for the KVS relay path** never arrived, so relay
candidates could never be gathered and off-LAN viewers could not connect.

**Root cause:** The drop is in the NCP's SPI→802.11 forwarding path or its
802.11 MAC filter, **before** `BusIo_SPI_ReceivePtr()` pulls the frame from the
SPI RX queue. No host-side component (LwIP, netconn, socket listener, ICE) ever
sees the frame. Confirmed with a raw-UART source-IP trace in
`lwip_netif.c → netif_rx_process` (bypasses all LwIP/FreeRTOS): over ~60 s of
active TURN `Allocate` retransmission against two AWS TURN servers, **zero**
`[rx] udp` traces with a non-LAN source IP, while the outbound `sendto` markers
fired normally. Reproduced on two ISPs (Comcast + Verizon).

**Fix:** Upgrade to the **full X-CUBE-ST67W61 V1.3.0** middleware (host tree +
Target port files + LwIP glue). A partial upgrade (V1.3.0 W61_bus files only on
a V1.2.0 tree) is **not** sufficient. Verified on the N6: public DNS/53
responses from `74.40.74.40` now arrive reliably at `netif_rx_process`, and the
KVS UDP relay path works.

**Related bug report:** filed to the ST firmware team 2026-04-14; artifacts
preserved under `_st_bugreport_artifacts/` (raw-UART trace patch, config
extracts, the V1.2.0→V1.3.0 delta, and a flat zip).

**Note on NTP:** NTP/123 to public servers can still fail on some LANs even with
V1.3.0 — that is a **network policy** issue (e.g. Google Nest/Google Wifi
intercepting client NTP), not the NCP. The HTTP time fallback in `sntp_port.c`
handles it; keep it.

---

## 2. NCP power-save must be disabled for real-time media

**Issue:** With the NCP's automatic power-save enabled, the radio drops into
low-power states between packets. That adds latency/jitter and can delay or drop
frames — fatal for real-time RTP media and for TURN keepalives, which the relay
uses to keep the allocation alive. Enabling power-save correlated with erratic
streaming and premature session teardown.

**Fix / config:** Power-save is disabled and must stay disabled for the
streaming build:

- `W6X_POWER_SAVE_AUTO = 0` in `Appli/ST67W6X_Network_Driver/Target/w6x_config.h`
- `W61_SetPowerMode(_, 0, 0)` at bring-up
- `W61_MAX_SPI_XFER = 1520` (matches the V1.3.0 default / ST recommendation)

**Trade-off:** Higher idle power draw. Acceptable for a mains-/USB-powered demo;
revisit only if a battery use-case appears (and then only for periods with no
active KVS session).

---

## 3. Shared-SPI TX flow-control (`rx_stall`) and the session-stability stack

**Issue:** Media TX, MQTT, and AT control all share **one** SPI TX queue to the
NCP. When the module asserts `rx_stall` flow control (it prioritizes draining
its RX path), **all** host→NCP TX freezes — media *and* MQTT stall together.
Combined with a few driver liveness holes, this produced the long-running "KVS
session drop" saga (sessions dying anywhere from 11 s to 231 s).

**Resolved** 2026-07-20/21 as a stack of fixes (feature/ai-detection,
`b7d3496..ab16022`, `af5b668`, `aaa92f1`). The load-bearing config landing spots
(the only *live* override headers):

- `SPI_TXQ_LEN = 32` in `Appli/ST67W6X_Network_Driver/Target/w61_driver_config.h`
- `W6X_NETIF_STA_RXQ_DEPTH = 32` in `Target/w6x_config.h`
- tcpip mbox 64 / prio 45 / stack 1024 in `lwipopts_freertos.h`
- `TCP_MSS = 1150` in `Appli/Common/config/lwipopts.h` (see §4)

Brief cause list (each masked the next): DCMIPP IPPLUG partition starvation;
an `lwip_netif.c` RX-error path **double-free** (heap corruption → the lifetime
lottery); the `rx_stall` whole-stack convoy above; SPI engine liveness holes
(edge-only `TXN_RDY`, unbounded hdr-ack loop); and an ICE close-gate calibrated
for slow strikes. Full detail and the diagnosis playbook live in the commit
history; the diagnostic vocabulary is summarized in §5.

---

## 4. TCP-TLS TURN relay wedges on Wi-Fi (UDP relay is the reliable path)

**Issue:** The W6X **TCP transmit path** stalls under sustained load. When a KVS
session is nominated onto the **`turns:...?transport=tcp`** relay (TURN-over-TLS
on TCP/443), large RTP segments jam: the TCP `snd_buf` never drains
(`[TLS] snd stall len=… done=0 e=11`), the socket mutex times out
(`socketMutex timeout (1500 ms) — prior send wedged`), and the ICE close gate
eventually fires (`[icn] GATE CLOSE fails=… span=…`). This is a **W6X SPI-side
TCP-TX defect** — it is Wi-Fi-only and does **not** occur on Ethernet.

Two things make the **UDP** relay path solid instead:
- WAN UDP RX now works (§1), so the plain-UDP TURN `Allocate` succeeds and
  DTLS/UDP media flows through the relay.
- The ICE filter in `Appli/Libraries/kvs_webrtc/examples/app_common/app_common.c`
  accepts `turn:...?transport=udp` (UDP relay), skips only
  `turns:...?transport=udp` (DTLS-TURN-over-UDP, which `ice_controller_net.c`
  rejects), and keeps `turns:...?transport=tcp` as a fallback.

ICE prioritizes the UDP relay over the TCP relay, so in normal use the device
streams cleanly over UDP (validated 2+ min, `ovr=0`, zero `[TLS] snd stall`).
The TCP relay is only nominated when the UDP relay pair fails connectivity for a
particular viewer.

**Mitigation in place — ICE close-timeout.** Independently of transport choice,
a stuck relay teardown used to wedge the peer-connection slot forever (the W6X
relay never confirms the TURN release), and after both of the 2 slots wedged the
master silently dropped every new viewer. That is fixed with a bounded deadline
in `ice_controller.c` (`ICE_CONTROLLER_CLOSING_TIMEOUT_MS`, default 2000 ms):
after the deadline the lingering sockets are force-closed and `ICE_CLOSED` is
emitted so the slot re-arms. Net effect: even if a session lands on the TCP
relay and wedges, it **self-heals** — forced teardown in ~2 s, and the next
viewer connects (typically over UDP). Validated: back-to-back viewers with no
board reset.

### Attempted + REVERTED: dropping `turns:tcp` on Wi-Fi (2026-07-25)

The idea was: since ICE sometimes nominates the wedging TCP relay (intermittent
black screen on Start), drop `turns:?transport=tcp` on Wi-Fi so only the UDP
relay is used. Implemented behind `KVS_TURN_DROP_TCP` (`demo_config.h`) with the
ICE-filter skip wrapped in `#if KVS_TURN_DROP_TCP`.

**It made things worse and was reverted (`KVS_TURN_DROP_TCP` defaults to 0).**
Fresh-boot, first-ever Start Video with the drop active gathered **only host +
srflx candidates, no relay at all** (`Unable to find valid connection … closing`
→ black). Root cause the test exposed:

- **The W6X's plain-UDP TURN Allocate is intermittent** — the Allocate response
  over WAN UDP doesn't reliably arrive, so a `turn:?transport=udp`-only config
  frequently yields **no relay candidate**.
- **`turns:tcp` (TLS/TCP Allocate) was the *reliable* relay** all along — TCP
  delivers the Allocate response every time, so a relay candidate always forms.
  Sessions were connecting *because* of the TCP relay; removing it left only the
  flaky UDP Allocate and often no relay → couldn't connect.

So `turns:tcp` must stay. The remaining symptom is the **TCP relay's media path
wedging once nominated** (`[TLS] snd stall` → `GATE CLOSE` → black-after-connect,
which the close-timeout then self-heals). The correct fix is **not** removing the
TCP relay but making the **UDP relay reliable or preferred**:
- Make the UDP TURN Allocate robust (retry / confirm the Allocate response is
  actually arriving over WAN UDP for the relay flow, not just DNS).
- And/or bias ICE nomination toward the UDP relay pair when it *does* allocate.

The `KVS_TURN_DROP_TCP` flag + filter block are left in place (default 0, i.e.
no-op) as documentation of the dead end.

---

## 5. Diagnostic vocabulary (W6X health, in logs)

These markers are always-on raw prints (the `LogWarn`/`LogError` variants in
`ice_controller_net.c` compile out):

| Marker | Meaning |
|---|---|
| `[M] … isnd=` | W6X TX health. Healthy 2–30 ms; **>1000 = link dying**. |
| `rx_stall BEGIN/END dur=` | NCP credit/flow-control episode (host→NCP TX paused). Brief (≤~15 ms) is benign; sustained bursts choke TX. |
| `socketMutex timeout (1500 ms) — prior send wedged` | A TX stalled long enough to block the next send (TCP-TX wedge signature). |
| `[TLS] snd stall len= done= z= e=` | TURN-over-TLS/TCP send stalled. `done=0 e=11` (WANT_WRITE) → `snd_buf` never drains → segment size vs path MTU (see §4 MSS fix). |
| `[icn] GATE CLOSE fails= span=` | ICE close gate tripped after N send failures spanning the window. |
| `TURN release not confirmed after N ms; forcing socket teardown` | §4 close-timeout firing — the W6X relay never confirmed the TURN release; slot is being force-reclaimed. Expected on Wi-Fi relay teardown. |
| `[rx] udp src=… sp=… dp=…` | Raw inbound-UDP trace (diagnostic, normally compiled out via `WAN_UDP_RX_DIAG` in `lwip_netif.c`). Used to prove §1. |

---

## References

- [ethernet_support.md](ethernet_support.md) — the wired transport (not affected by any of the above).
- [../Middlewares/ST/ST67W6X_Network_Driver/V130_UPGRADE_NOTES.md](../Middlewares/ST/ST67W6X_Network_Driver/V130_UPGRADE_NOTES.md) — V1.2.0 → V1.3.0 upgrade.
- `_st_bugreport_artifacts/` — the WAN-UDP-RX bug report package filed to ST.
- Config: `Appli/ST67W6X_Network_Driver/Target/w6x_config.h`, `Target/w61_driver_config.h`, `Appli/Common/config/lwipopts.h`, `lwipopts_freertos.h`.
- ICE: `Appli/Libraries/kvs_webrtc/examples/app_common/app_common.c` (server filter), `.../ice_controller/ice_controller.c` (close-timeout).
