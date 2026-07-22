# AI + KVS WebRTC Streaming Integration Plan

> **2026-07-18 scope update: LCD preview is REQUIRED** (user decision —
> same experience as the iotc-stm32-n6-demos AI examples).  Adopt the
> donor's media architecture instead of grafting around our NV12 path:
> PIPE1 → RGB888 at display resolution (800x480) → shared framebuffer →
> (a) LTDC displays it, (b) detection boxes are drawn into it (donor
> draw.c), (c) VENC encodes it (H264ENC_RGB888 input, as donor app_enc.c)
> — so overlays appear on the LCD **and** in the cloud stream.  PIPE2 →
> NPU unchanged.  Bandwidth: 800x480 RGB888 ≈ 17 MB/s DCMIPP write +
> 17 MB/s VENC read + ~69 MB/s LTDC refresh — the donor sustains exactly
> this mix; our failed April config (1280x720 ARGB8888, 54 MB/s DCMIPP
> alone) was ~2.6x heavier.  This REPLACES the "no display" assumption
> below; media_cam/media_enc largely re-port from donor app_cam/app_enc,
> plus LTDC/BSP init and draw.c.  New phase order: 1 scaffold (done),
> 1b AI-enabled build, 2 Pipe2+NPU bring-up (no display yet), 3 telemetry,
> 4 display re-port (PIPE1 RGB + LTDC + draw overlay + VENC RGB input),
> 5 combined soak.  Phases 2-3 stay verifiable on the current NV12 path
> before the display rework lands.

Goal: one HW_Crypto firmware that runs NPU object detection AND streams
640x480 H.264 via KVS WebRTC, publishing detections as IOTCONNECT telemetry
(and optionally overlaying boxes in the encoded video later).

## Why this is feasible

ST's **x-cube-n6-ai-h264-usb-uvc** package (local copy:
`c:\dev\slim\x-cube-n6-ai-h264-usb-uvc`) already runs DCMIPP + NPU (ATON)
+ VENC concurrently on this exact board — it is the architecture donor.
The demos repo (`iotc-stm32-n6-demos`) `uvc` sample carries the IOTCONNECT
telemetry glue for its detections (`Src/app.c`), and this firmware already
has camera→VENC (Pipe1, NV12 640x480) plus the full IOTCONNECT/iotcl path.

## Architecture

- **DCMIPP Pipe1** (existing): sensor → downscale → NV12 640x480 → VENC →
  RTP/SRTP → KVS.  Unchanged.
- **DCMIPP Pipe2** (new): same sensor exposure → downscale to model input
  (e.g. 224x224 or model-native) RGB888 → NPU input buffer.
- **NPU (ATON/LL_ATON)**: object-detection network from the ST model zoo
  (same one the demos repo uses), weights placed per the uvc package's
  linker layout.
- **Detections task**: postprocess → throttle (e.g. 1-2 Hz or on-change) →
  `iotcl` telemetry (`d2c_rpt`) using the existing MQTT agent.  Template
  needs new attributes (class/conf/box) — extend `stm32n6wrt` template.

## Known constraints (measured this week — do not rediscover)

- PSRAM/AXI bandwidth is the scarce resource: 720p NV12 already stalled
  VENC (see `media_cam_config.h` history).  Pipe2 adds ~224*224*3*15fps
  ≈ 2.3 MB/s — small, but re-verify `ovr=` stays 0 in the `[M]` heartbeat.
- Heap floor during streaming ≈ 230 KB free; NPU activations must come
  from a dedicated pool (uvc package pattern), NOT the FreeRTOS heap.
- Build config: HW_Crypto at -O2 (dev @ f36fa6d).  Encoder 1 Mbps.
- Send path sustains ~9.5 fps; AI must not add >10 ms/frame to the media
  task loop (`enc=` heartbeat field will show it).

## Phases

1. **Scaffold**: copy NPU init + Pipe2 setup + model files from
   x-cube-n6-ai-h264-usb-uvc into `Appli/Common/app/ai/` behind an
   `ENABLE_AI_DETECTION` build define (default off) — firmware must still
   build+stream with it off.
2. **Pipe2 bring-up**: Pipe2 frames landing in the NPU input buffer;
   verify with a Y-mean probe; heartbeat gains `ai=` ms/frame field.
3. **NPU inference**: run the network on Pipe2 frames in a low-priority
   task; log top detection.
4. **Telemetry**: detections → iotcl JSON; extend the IOTCONNECT template
   (`IOTCONNECT_Templates/stm32n6wrt.json`) with detection attributes.
5. **Verify**: stream + detect simultaneously for 10+ min; heartbeat
   `ovr=0`, heap flat, fps unchanged (~9.5).
6. (Later) box overlay into NV12 before VENC; demos-repo doc updates.

## Donor survey results (Phase 1 inputs)

- `Src/app_cam.c`: `DCMIPP_PipeInitDisplay` (PIPE1) + `DCMIPP_PipeInitNn`
  (PIPE2) side by side; `CAM_NNPipe_Start(nn_pipe_dst, mode)` starts PIPE2.
  → graft the PipeInitNn/NNPipe_Start pattern into our `media_cam.c`.
- `Src/app.c`: ST Edge AI runtime (`stai_network_*`), `network_ctx`
  (STAI_NETWORK_CONTEXT_SIZE, ALIGN_32), `nn_input_buffers[2]` in PSRAM,
  `bqueue` producer = PIPE2 vsync callback (`HAL_DCMIPP_PIPE_SetMemoryAddress`
  per frame), consumer = inference thread → postprocess (`Lib/lib_vision_models_pp`).
- `Model/`: `network.c`, `stai_network.{c,h}`, `network_data.hex`,
  `network_ecblobs.h` (dir 41 MB); `Lib/` 63 MB (copy only ATON runtime +
  pp lib subset).  Check donor GCC linker script for weight placement and
  `app_fuseprogramming.c`/NPU clock init in its `main.c`.
- NN input dims come from `NN_WIDTH/NN_HEIGHT/NN_BPP` defines — find in Inc/.

## Session log

- 2026-07-20 (close of day): **STABILITY CHAPTER CLOSED (ab16022).**
  Final validation run: session 1 hit a genuine >3 s module outage
  1.2 s in — the time-based gate absorbed 13 consecutive send failures
  for the full 3 s window and then closed CORRECTLY; session 2 ran
  165+ s (still running at capture), clean 500 kbps -> 1 Mbps upshift
  at T+30 s, 1 Mbps sustained 135+ s, isnd 25-27 ms, ovr=0, heap FLAT
  (double-free fix), 317 inferences at 33-70 ms.  The complete W6X fix
  stack (one day, commits b7d3496..ab16022): DCMIPP IPPLUG repartition;
  RX double-free heap-corruption fix (+wrapper leak); SPI TXQ 8->32 +
  STA RXQ 16->32; 20 ms bounded enqueue under the lwIP core lock;
  pbuf-chain abort on first error; TXN_RDY pin-level fallback; hdr-ack
  loop cap; bounded RX enqueue (engine can no longer wedge);
  rx_stall episode logging + 1 s network-frame escape; tcpip mbox/prio/
  stack; EINPROGRESS as transient; ICE retry budget 200 ms; 3-strike
  gate made TIME-BASED (>=3 fails spanning >=3 s); adaptive bitrate
  500k<->1M (per-session LOW start, 30 s quiet upshift, get-modify-set
  rate ctrl).  Remaining forward work: platform template attrs
  (XG4EGET: ai_people/ai_top_conf/ai_infer_ms), dets>0 with a person
  in frame, PSRAM 200 MHz retry, PIPE2 Suspend->Resume validation
  (116297e), inference rate floor tuning, Phase 4 LCD re-port.

- 2026-07-20 (final): **SESSION STABILITY RESOLVED — layered fixes, all
  validated.**  The "session drop" was three separate bugs peeled apart
  over one day: (1) DCMIPP IPPLUG partition starved PIPE1 when PIPE2 ran
  (b7d3496); (2) W6X network TX credit wait was 10 s, so a module stall
  wedged every socket incl. MQTT — bounded to 200 ms (f693cbf), plus a
  3-strike consecutive-failure gate on the nominated ICE socket (2502df1;
  NOTE: all LogWarn in ice_controller_net.c compile out at this log
  level, so the gate works silently) and RTP burst pacing (3f428b6);
  (3) LINK CAPACITY: on a degraded RF evening 1 Mbps (~100 pkt/s)
  overruns the W6X within ~2 s of media start — the 500 kbps A/B
  (03cc1b4) soaks cleanly (F1800+, isnd 11-18 ms, IDRs to 38 KB fine,
  full AI at ~40 ms/inference).  NEXT on this axis: adaptive bitrate
  (drop on consecutive send failures / raise on quiet), or ship 500 kbps
  as the safe default.  Diagnostic legacies fixed on the way: Y-probe
  stride ([M]Y240 now shows real luma), first-inference timing, XSPI2
  clock print.  Phase 3 telemetry (ai_people/ai_top_conf/ai_infer_ms)
  is live in the payload — platform template XG4EGET still needs the
  three NUMBER attributes added via the IOTCONNECT UI.  dets=0 all
  night: nobody in frame — verify detection with a person in view.
  Remaining queue: PSRAM 200 MHz retry (HSLV fix unblocks it), PIPE2
  Suspend->Resume across sessions (116297e still untested in the wild),
  inference rate floor tuning, Phase 4 LCD re-port.

- 2026-07-20 (cont.): **silent-drop fix + Phase 3 telemetry** in one build.
  (a) The ~4 min drop trigger was found in code: ANY single
  SendSocketPacket failure on the nominated ICE socket (each already
  spans up to 1 s of EAGAIN/ENOMEM retries) closed the whole session
  (ice_controller_net.c).  Now the nominated socket tolerates transient
  failures — the packet is dropped (NACK/rolling-buffer recovers) and
  only ICE_CONTROLLER_SEND_FAILURE_CLOSE_THRESHOLD (3) consecutive
  failures close the session; non-nominated candidate pruning unchanged.
  Look for "Transient send failure N/3" in logs.  (b) Phase 3:
  AiDetection_GetTelemetry() snapshot (people count, top conf %, infer
  ms) published in prvPublishDemoTelemetry as ai_people / ai_top_conf /
  ai_infer_ms; stm32n6wrt.json updated with the matching NUMBER
  attributes — the ACTIVE platform template (CD XG4EGET, not in repo)
  needs the same three attributes added via the IOTCONNECT UI before
  they appear on dashboards.  TEST: soak a viewer session well past
  5 min; expect "Transient send failure" warnings instead of drops, and
  ai_* fields in the telemetry payload log line.

- 2026-07-18: plan written; donor surveyed (above).  No code changes yet.
  Board/dev at stable f36fa6d.  Next concrete step: create
  `Appli/Common/app/ai/` with bqueue+nn-task skeleton behind
  ENABLE_AI_DETECTION (default off), copy Model/ + minimal Lib subset,
  add PIPE2 init to media_cam.c, NPU clocks to main.c, weights to linker.

- 2026-07-20: **Phase 2 COMPLETE** (build 56aa615). NPU inference runs live
  alongside streaming: `[AI] inferences=3 dets=1 pp_ret=0`, AiDetect <1% CPU.
  Chain of fixes that got here: littlefs base moved (real header
  xspi_nor_mx66uw1g45g.h, 0x2200000), NOR kept memory-mapped steady-state
  with window mutex, xAppContext+ecblob to PSRAM / app confined to
  AXISRAM1+2, real CACHEAXI driver (npu_cache_stub.c) with post-reset
  settle delay + RAMCFG power-up, PIPE2 started with the camera (not at AI
  init — the boot-time start failed on the idle sensor and configASSERT
  spun).  Streaming holds ~15 fps.  NEXT: Phase 3 telemetry (dets → iotcl),
  Phase 4 LCD, inference-rate tuning (only ~1 Hz observed), and the
  intermittent viewer-side session freeze (TURN ChannelData suspicion,
  see chrome-dtls-channeldata memory; tonight's net was also degraded —
  20 s TLS handshakes, MQTT CONNACK retry, "long time no SDP" timeout).

- 2026-07-20 SESSION-DROP ROOT CAUSE IDENTIFIED (not yet fixed): viewer
  sessions die ~1.8 s after F0 **only when PIPE2/inference runs**; every
  long session (F900/F1320) had AI accidentally off (FMODE park, init
  grind, -4 start).  "first inference done" lands exactly at teardown.
  The ~1.75 s NPU weight fetch (30 MB over XSPI2) saturates the NoC and
  stalls the media/network path during connection-critical seconds.
  FIX CANDIDATES, in order: (1) make inference cheap — NPU clock
  IC6 PLL1/4->PLL1/2 in FSBL + verify XSPI2 200MHz bump took effect
  ([PSRAM] print still shows 64MHz for XSPI1 pre-bump; check XSPI2's
  actual kernel clock at runtime); (2) defer first inference until ~10 s
  after session start to confirm the correlation; (3) rate-limit
  inference (e.g. one per N seconds) until fetch time drops.  Wi-Fi and
  power were ruled out by the user; NACK/rolling-buffer fix (capacity
  81) is validated separately.

- 2026-07-20 (later): SESSION-DROP FIX IMPLEMENTED, pending board test.
  Why the 7fad82d XSPI2 kernel bump changed nothing: the kernel clock was
  ALREADY HCLK/200MHz (HAL_XSPI_MspInit sets it); the NOR was slow because
  (a) extmem_manager.c configured EXTMEM_LINK_CONFIG_1LINE — single-wire
  SPI — and (b) the SFDP driver parks SCLK at its 50 MHz default and,
  with sfdp_public.MaxFreq==0 (config is memset), never raises it for
  non-octal links.  Effective weight path: ~50MHz/1-line ≈ 6 MB/s.
  FIXES: Appli extmem_manager.c -> EXTMEM_LINK_CONFIG_8LINES: the MW
  switches the MX66UW1G45G to octal DTR (8D-8D-8D) and sets SCLK from the
  SFDP xSPI-profile tables -> 200 MHz DTR w/ DQS (~400 MB/s raw; all 8 IOs
  + DQS already in MspInit; littlefs is EXTMEM/mapped-window agnostic;
  FSBL EXTMEM left at 1LINE deliberately — boot-critical, one 1.5 MB copy).
  FSBL IC6 div 4->2 (NPU 400->800 MHz), IC11 8->4 (NPU RAMs 200->400 MHz);
  VOS0+SMPS-overdrive already set.  DIAGNOSTICS added in ai_detection.c:
  "[AI] XSPI2 kernel/presc/sclk" log at task start (verify presc=0 @
  200 MHz), "[AI] first inference: N ms" (pass/fail evidence vs the old
  ~1750 ms), heartbeat gains last=Nms.  Also added AI_MIN_INFER_INTERVAL_MS
  500 rate floor as insurance until the rate-tuning pass.
  TEST: flash both FSBL+Appli (bin/flash.ps1), open a viewer, expect
  session to survive >60 s with inferences ticking and first-inference
  well under ~300 ms.  If it still dies at first inference, next step is
  the 10 s deferred-first-inference experiment to re-check correlation.

- 2026-07-20 (evening): first octal build (8f94aff) BOOT-LOOPED — app got
  through main(), logged up to "IWDG Enabled", then froze silently until
  the 32.8 s IWDG reset (no HardFault dump, no ticks: an AXI bus stall on
  the first memory-mapped DTR read, the signature of a mis-negotiated
  octal link).  ROOT CAUSE (explains BOTH this and the April "PSRAM at
  200 MHz corrupts frames" that forced XSPI1 back to 64 MHz): N6 XSPI
  pads need (a) the HSLV OTP fuse — word 124 bits 15 (VDDIO3/XSPI2-NOR)
  and 16 (VDDIO2/XSPI1-PSRAM), which every ST N6570-DK example programs
  on first boot via app_fuseprogramming.c — AND (b) runtime
  HAL_PWREx_ConfigVddIORange(VDDIOx, 1V8) (the DK BSP does both per
  port).  Our firmware called only HAL_PWREx_EnableVddIO2/3: pads stayed
  in 3.3V-range mode — fine at <=64 MHz, broken at 200 MHz.
  FIXES (this session): FSBL now checks/programs the HSLV fuses
  (idempotent, prints [FSBL] HSLV...) and sets both VDDIO ranges to 1V8;
  Appli EXTMEM init is now fail-safe — octal init return code is checked
  (was ignored), the link is verified with INDIRECT reads (HAL-timeout
  protected, cannot AXI-stall) before any mapped access, and it falls
  back to 1-line on failure so the board always boots; ExtMem SFDP debug
  trace (level 2) prints init steps to raw UART during init only; [LFS]
  markers bracket the first mapped read.  FOLLOW-UP once octal is
  proven: retry XSPI1/PSRAM at 200 MHz — the HSLV fix likely cures the
  April corruption and doubles media-path headroom.

- 2026-07-20 (late night): **SESSION-DROP ROOT CAUSE FOUND AND FIXED**
  (b7d3496).  Bisect: PIPE2-off survived 160s+; PIPE2-on/NPU-idle showed
  garbage PIPE1 frames and died ~10s; full AI died ~1.5s.  Cause: the
  DCMIPP IPPLUG partition in media_cam.c predated PIPE2 and was
  backwards for this workload — per RM0486 Client2/3 are PIPE1's Y/UV
  planes and Client5 is PIPE2, so PIPE1-Y ran on a one-line FIFO at
  1/16 arbitration weight, PIPE1-UV was unpartitioned, and the NN pipe
  held 15/16 weight; starting PIPE2 corrupted PIPE1 (noise P-frames
  8-9KB), ballooned bitrate onto the TURN uplink, and collapsed the ICE
  session — faster with the NPU adding contention.  Fix: all five
  clients explicitly partitioned (full 1024-entry DPREG, >=2 lines
  each, PIPE1 Y=15/UV=7 weights over PIPE2=1, BURST_64 kept).
  VALIDATED: full AI + streaming ran 231 s continuous — F3420 @
  ~14.8fps, 433 inferences @ ~62ms (2Hz floor), enc=16ms isnd=26ms
  ovr=0, heap flat, 44% idle.  REMAINING BUG (separate): session
  closed silently at ~236s after the TURN allocation — right at the
  refresh point of the 300s ICE-server TTL; "connection not ready"
  close with no error suggests the TURN allocation refresh
  fails/never happens (matches the historic "intermittent freeze on
  long sessions").  NEXT: TURN-refresh investigation in
  ice_controller (allocation refresh timer / permission refresh),
  then camera scene check (Y=0xAA uniform, dets=0 — aim at objects),
  then queued: PSRAM 200MHz retry, PIPE2 Resume test, Phase 3
  telemetry, Phase 4 LCD.

- 2026-07-20 (night): b118e27 ON-BOARD RESULTS.  HSLV fuses were ALREADY
  set (OOB demo had burned them) — the missing piece was only the
  runtime VddIO 1V8 range.  Octal DTR NOR VALIDATED: SFDP trace clean,
  flash ID c2:81:3b, [XMEM] octal link OK, XSPI2 sclk=200 MHz presc=0,
  littlefs mounts, mapped reads fine.  NPU fix VALIDATED: [AI] first
  inference: 37 ms (was ~1750 — 47x), sustained 37-39 ms/inference
  across sessions, 2 Hz rate floor active, real 17-22 KB IDR F0 frames.
  BUT sessions still die 1.3-1.8 s after F0 (3/3 attempts): no close
  reason in device log, then "Fail to send RTP packet ret 22" (a
  consequence of the close flow, not the cause), PeerClosed, STOPPING
  MEDIA.  Attempt 1 of 3 also hit "DTLS handshaking timeout".
  CONCLUSION: the AI/NoC-stall theory is DISPROVEN as the session
  killer — the old "died at first inference" correlation was temporal
  coincidence (the 1.75 s stall happened to land at the same ~1.5 s
  post-F0 mark where this close occurs).  The remaining session drop
  matches the known intermittent Chrome DTLS/TURN ChannelData issue
  (see chrome-dtls-channeldata memory: peer ChannelBinds the TURN
  relay, device drops 0x40-prefixed ChannelData).  NEXT DISCRIMINATORS:
  (a) viewer on the same LAN (host candidates, no TURN) — if sessions
  survive, TURN path confirmed; (b) different browser (Firefox vs
  Chrome); (c) chrome://webrtc-internals on the viewer side for the
  close reason; (d) re-enable/check the [isl] ChannelData traces (none
  fired in tonight's log — verify they're still compiled in).

- 2026-07-21: PSRAM 200 MHz VALIDATED (8efa710).  Donor-timing diff showed
  our prvPsramInit already matched the donor's 200 MHz recipe field-for-
  field (RL/WL 7, dummy 6, DQS, DHQC, CSHT 5, 16KB CS boundary; N6 HAL
  has no delay block) — the only delta was the kernel clock.  April's
  "skip-all-MB P-frames" corruption was VDDIO2 3V3-range/HSLV-off pads,
  cured by the same FSBL fuse + 1V8 work that fixed the NOR.  Switch
  done inline in prvPsramInit (PeriphCommonClock_Config stays disabled —
  it carries an unvalidated TIM prescaler change).  New 64KB bulk smoke
  test (> D-cache, 4 CS-boundary crossings): bad=0 at 200 MHz.

- 2026-07-21: TURN-TLS RELAY RESOLVED (e7af228 + af5b668) — session-drop
  cause #6.  First off-LAN viewer forced TURN-over-TLS relay; sessions
  died at exactly 3.1 s (close gate, correctly).  e7af228 added the
  mbedtls I/O mutex (media-task ssl_write raced listener-task ssl_read
  on the same ctx — real bug, not the killer) plus ALWAYS-ON raw-UART
  send-loop diagnostics (LogWarn/LogError never print from
  ice_controller_net.c).  One diagnostic run nailed it: "[TLS] snd stall
  len=1232 done=0 z=40 e=11" — TCP snd_buf never drains; jam always
  began at the first non-black (large) frame.  Small segments + all RX
  flowed; ~1301-1315-wire-byte segments were never ACKed = path-MTU
  black hole on the WAN uplink (validated direct-UDP media at ~1242
  wire bytes was LAN-only and never crossed it).  af5b668: TCP_MSS
  1476->1150 (~1204 wire max).  VALIDATED on-board: relay session 84+ s
  and climbing, 55-69KB I-frames through the relay, clean 500k->1M
  upshift at T+30s (sendFails=0), heap flat, AI 157 inferences 31-53 ms
  concurrent.  ROM region +16K (linker split shift in
  STM32N657X0HXQ_LRUN_kvs.ld; RAM keeps ~21K slack).

- 2026-07-21 (final): RELAY VALIDATION COMPLETE (aaa92f1).  5.3+ min
  TURN-TLS relay session at 1 Mbps, survived THREE Wi-Fi hiccups
  ([TLS] snd stall events at T+186/266/272 s): each cost 1-2 dropped
  packets, triggered the 500k downshift, recovered instantly, upshifted
  back at +30 s.  Zero GATE CLOSE (12 s relay window).  Heap pinned at
  ~149.9 KB all run; AI 597 inferences 31-65 ms concurrent.  Stability
  matrix now: direct-UDP LAN 165+ s AND relay-TCP WAN 5+ min, both with
  AI + adaptive bitrate.  Remaining queue: inference rate tuning,
  Phase 4 LCD re-port, IOTCONNECT template attrs (user).
