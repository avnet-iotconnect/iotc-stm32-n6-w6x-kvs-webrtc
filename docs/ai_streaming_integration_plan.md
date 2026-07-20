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
