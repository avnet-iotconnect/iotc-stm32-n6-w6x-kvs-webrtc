# Wired Ethernet Transport (ETH1 + RTL8211F, RGMII)

This project can run its entire network stack over the on-board **Gigabit
Ethernet** port instead of the ST67W6X Wi-Fi module. Wired Ethernet was added
as the transport for the KVS WebRTC video demo after the W6X Wi-Fi path proved
unable to sustain the upload bandwidth reliably (module-level TX stalls under
sustained load). The Wi-Fi path is untouched and is re-selected by flipping one
switch.

> **TL;DR** — `#define NET_USE_ETHERNET 1` in
> [`Appli/Core/Src/app_freertos.c`](../Appli/Core/Src/app_freertos.c) selects
> Ethernet and compiles the W6X network task out of the build. Everything else
> is self-contained in
> [`Appli/Common/net/W6X_ARCH_T02/eth_netif.c`](../Appli/Common/net/W6X_ARCH_T02/eth_netif.c).

---

## 1. The transport switch

`app_freertos.c` chooses exactly one network task at startup:

```c
#define NET_USE_ETHERNET 1

#if defined(ST67W6X_RCP) && !NET_USE_ETHERNET
    xTaskCreate(net_main,     "W6xNet", ...);   /* Wi-Fi (ST67W6X) */
#endif
#if NET_USE_ETHERNET
    xTaskCreate(eth_net_main, "EthNet", ...);   /* wired Ethernet  */
#endif
```

- `NET_USE_ETHERNET 1` → the `eth_net_main` task runs; `net_main` /
  `MX_LWIP_Init` **never run**. `eth_net_main` owns `tcpip_init`. The W6X Wi-Fi
  stack is dead code the linker garbage-collects (~30 KB ROM reclaimed).
- `NET_USE_ETHERNET 0` → back to the original Wi-Fi behaviour, unchanged.

**The one load-bearing contract:** every consumer (IoTConnect, KVS, NTP, …)
waits on `EVT_MASK_NET_CONNECTED` on the `xSystemEvents` event group. The
Ethernet path sets that bit when DHCP binds an address — identical to the Wi-Fi
path — so nothing downstream had to change.

---

## 2. Hardware

Facts taken from ST's STM32CubeN6 `Nx_WebServer` example for this exact board
(STM32N6570-DK).

| Item | Value |
|------|-------|
| MAC | STM32N657 **ETH1** (HAL ETH v2 API) |
| PHY | **RTL8211F(-CG)**, RGMII |
| MDIO / MDC | **PD12 / PD1** |
| PHY INTN | **PD3** |
| RGMII data/ctrl pins | PD1/3/12, PF0/2/5/7–15, PG3/4 |
| GTX_CLK | **PF0**, `GPIO_AF12_ETH1` (the one AF12 pin) — all other ETH pins `GPIO_AF11_ETH1` |
| MAC kernel clock | **HCLK** |
| RGMII RX/TX ref clocks | external (driven by PHY pads; reset default) |
| Interface select | `__HAL_RCC_ETH1PHY_CONFIG(RCC_ETH1PHYIF_RGMII)`; `CCIPR2` written by `HAL_ETH_Init` |
| Media interface | `HAL_ETH_RGMII_MODE` |

**RIF (security):** ETH1 is granted bus-master and slave-secure attributes so
its DMA can reach memory, mirroring how DCMIPP/VENC are already configured:

```c
HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_ETH1, &xMaster);
HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_ETH1, SEC|PRIV);
```

---

## 3. Software architecture — `eth_netif.c`

A single file (~450 lines). No BSP, no CubeMX ETH glue — it drives the N6 HAL
ETH v2 API directly and plumbs an lwIP `netif`.

**`eth_net_main` task** (created by `app_freertos.c`):
1. `tcpip_init` (this file owns it).
2. Seed the RX buffer free-list, build the MAC (§5), fill `ETH_HandleTypeDef`
   (`MediaInterface = HAL_ETH_RGMII_MODE`, `RxBuffLen = 1536`, TX/RX descriptor
   lists in NCRAM).
3. `HAL_ETH_Init` (its MspInit does clocks / GPIO / RIF), add the lwIP netif,
   register the link/status callbacks.
4. Loop forever:
   - **RX poll** every `ETH_RX_POLL_MS` (**2 ms**) — drain `HAL_ETH_ReadData`
     into pbufs, hand up to lwIP.
   - **Link poll** every `ETH_LINK_POLL_MS` (**500 ms**) — read RTL8211F link
     state, and on link-up program MAC speed/duplex, `HAL_ETH_Start`,
     `netif_set_link_up`, and start DHCP.

**RX path** — `HAL_ETH_RegisterRxAllocateCallback` / `RxLinkCallback` with an
`EthRxSeg_t` chain over a fixed pool of **6 × 1536-byte** buffers (`ucRxPool`).
Polling (no RX interrupt) keeps the ISR surface zero and coherency simple.

**TX path** — `prvLinkOutput`: clean the D-cache for each cacheable pbuf
segment, then a **blocking** `HAL_ETH_Transmit` (timeout `ETH_TX_TIMEOUT_MS =
100 ms`) on TX DMA channel 0 with `CRC_PAD_INSERT` and checksum offload
disabled, followed by `HAL_ETH_ReleaseTxPacket`.

**Link/DHCP** — `prvLinkUpdate` reads `RTL8211_GetLinkState`, sets
`PortSelect`/`Speed`/`Duplex` (`PortSelect = DISABLE` = GMII/RGMII 1000),
`HAL_ETH_SetMACConfig`, `HAL_ETH_Start`, `netif_set_link_up`, then
`netifapi_dhcp_start`. `prvStatusCallback` sets
`EVT_MASK_NET_CONNECTED` when the address binds.

The PHY driver is [`rtl8211.c/.h`](../Appli/Common/net/W6X_ARCH_T02/rtl8211.c),
from [STMicroelectronics/stm32-rtl8211](https://github.com/STMicroelectronics/stm32-rtl8211)
(only `Init`/`DeInit`/`GetLinkState`/`SetLinkState` are implemented; BMCR reset
default already enables auto-negotiation). It exposes
`ENABLE_RTL8211F_TXDELAY` / `ENABLE_RTL8211F_RXDELAY` compile knobs for RGMII
clock-skew tuning — **not** currently defined (the board wiring / PHY strapping
provides the delay).

---

## 4. Memory & DMA coherency

The N6 HAL ETH driver does **no cache maintenance**. Rather than sprinkle
invalidate/clean calls, the DMA descriptors **and** the RX buffer pool live in a
small **non-cacheable** region carved from the top of app RAM. TX payloads stay
cacheable and are cleaned per-segment right before transmit.

Linker ([`STM32N657X0HXQ_LRUN_kvs.ld`](../Appli/STM32N657X0HXQ_LRUN_kvs.ld)):

```
RAM   (xrw) : ORIGIN = 0x3410D400, LENGTH = 0xF0400   /* was 0xF2C00; -10K for the carve */
NCRAM (rw)  : ORIGIN = 0x341FD800, LENGTH = 0x2800    /* ETH DMA descriptors + RX pool */

.eth_nocache (NOLOAD) : { *(.eth_nocache) *(.eth_nocache*) } >NCRAM
```

`eth_netif.c` puts `xDmaTxDesc`, `xDmaRxDesc`, and `ucRxPool` in `.eth_nocache`
(via the `NC_SECTION` attribute) and, at init, programs an **MPU region** over
`0x341FD800 … 0x341FFFFF` as non-cacheable (`ETH_NOCACHE_BASE` /
`ETH_NOCACHE_LIMIT` must match the linker `NCRAM`).

> If you resize the RX pool or descriptor lists, keep the `NCRAM` length, the
> `RAM` length, `ETH_NOCACHE_*`, and the MPU region in sync.

---

## 5. MAC address

Locally-administered, derived from the device UID so each board is stable and
unique without provisioning:

```
02:80:E1:xx:yy:zz    where xxyyzz = (UIDw0 ^ UIDw1 ^ UIDw2)[23:0]
```

---

## 6. Build plumbing

Enabling Ethernet touches the (hand-maintained, mostly gitignored) CubeIDE make
files — only the specific files below are force-added to git:

- **HAL module:** `#define HAL_ETH_MODULE_ENABLED` in
  [`Appli/Core/Inc/stm32n6xx_hal_conf.h`](../Appli/Core/Inc/stm32n6xx_hal_conf.h).
- **HAL driver sources** (copied byte-identical from ST HAL V1.3.0):
  `stm32n6xx_hal_eth.c`, `stm32n6xx_hal_eth_ex.c` — added to the HAL
  `subdir.mk` (C_SRCS/OBJS/DEPS + per-file compile rules) with matching
  `.c_includes.args`.
- **App sources:** `eth_netif.c`, `rtl8211.c` added to
  `Common/net/W6X_ARCH_T02/subdir.mk` and to `objects.list` (the linker input).

See [`repo_structure.md`](repo_structure.md) for how the CubeIDE make files are
tracked.

---

## 7. Bring-up log & troubleshooting

Healthy bring-up prints (over the debug UART):

```
[ETH] bring-up: RTL8211F RGMII, kernel=HCLK
... (link poll) ...
<INF> ... DHCP bound / NET_CONNECTED ...
```

then NTP, IoTConnect discovery/MQTT, and KVS all proceed exactly as on Wi-Fi.

| Symptom | Likely cause |
|---------|--------------|
| No link ever (link poll never reports up) | cable / PHY strapping; check MDIO on PD12, MDC on PD1 |
| Link up but DHCP never binds | RGMII clock skew — try `ENABLE_RTL8211F_TXDELAY` / `RXDELAY`; or no DHCP server on the segment |
| Random RX corruption / hard-fault in lwIP | NCRAM / MPU / linker lengths out of sync (see §4) |
| Consumers never start (stuck "waiting for network") | `EVT_MASK_NET_CONNECTED` not set — DHCP didn't bind |

---

## 8. Returning to Wi-Fi

Set `NET_USE_ETHERNET 0` in `app_freertos.c` and rebuild. The W6X task returns
and the Ethernet task compiles out. The Ethernet HAL module / driver sources and
`eth_netif.c` remain in the tree but are unreferenced (linker GCs them). Nothing
in the IoTConnect / KVS / NTP consumers changes — both transports satisfy the
same `EVT_MASK_NET_CONNECTED` gate.
