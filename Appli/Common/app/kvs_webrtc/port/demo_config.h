/*
 * STM32N6570 KVS WebRTC port — demo_config.h
 *
 * Compile-time configuration for the KVS WebRTC SDK examples layer.
 * Runtime values (AWS region, channel name, credentials) are read from
 * KVStore by kvs_webrtc_task.c and passed directly to the SDK APIs;
 * they do NOT need to be defined as preprocessor macros here.
 *
 * Copyright (c) 2025 STMicroelectronics / project contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DEMO_CONFIG_H
#define DEMO_CONFIG_H

#include "kvs_runtime_config.h"

/* ── Feature flags ───────────────────────────────────────────────────────── */

/* Transport-wide congestion control (adaptive bitrate) */
#ifndef ENABLE_TWCC_SUPPORT
    #define ENABLE_TWCC_SUPPORT  1U
#endif

/* Maximum simultaneous WebRTC viewers */
#define AWS_MAX_VIEWER_NUM  2

/* KVS agent string reported to the signaling service */
#define AWS_KVS_AGENT_NAME  "AWS-SDK-KVS-STM32N6"

/* Drop TURN-over-TLS/TCP relay candidates (turns:?transport=tcp) from the ICE
 * server list.
 *
 * DISABLED (0) — 2026-07-25, after testing showed it makes things worse:
 * dropping turns:tcp on Wi-Fi caused the TURN Allocate to yield NO relay
 * candidate at all (fresh boot, first Start Video: only host+srflx gathered,
 * "Unable to find valid connection" -> black).  Root cause: the W6X's plain-UDP
 * TURN Allocate is intermittent (the Allocate response over WAN UDP does not
 * reliably arrive), and turns:tcp (TLS/TCP Allocate) was actually the RELIABLE
 * relay that let sessions connect at all.  Removing it left only the flaky UDP
 * Allocate -> often no relay -> can't connect.  So keep turns:tcp (0).
 *
 * The TCP relay's *media* path still wedges once ICE nominates it (that
 * black-after-connect is a separate symptom); the real fix is making the UDP
 * TURN relay reliable / preferred, NOT removing the TCP one.  See
 * developer.md (W6X module notes section). */
#ifndef KVS_TURN_DROP_TCP
    #define KVS_TURN_DROP_TCP  0
#endif

/* ── Codec selection (exactly one of each must be 1) ─────────────────────── */

/* Video codec */
#define USE_VIDEO_CODEC_H264  1
#define USE_VIDEO_CODEC_H265  0
#if ( USE_VIDEO_CODEC_H264 + USE_VIDEO_CODEC_H265 ) != 1
    #error "Exactly one video codec must be selected."
#endif

/* Audio codec */
#define AUDIO_G711_MULAW  1
#define AUDIO_G711_ALAW   0
#define AUDIO_OPUS        0
#if ( AUDIO_G711_MULAW + AUDIO_G711_ALAW + AUDIO_OPUS ) != 1
    #error "Exactly one audio codec must be selected."
#endif

/* Enable receiving audio frames (play back to local output) */
#define MEDIA_PORT_ENABLE_AUDIO_RECV  0   /* No audio output on this board */

/* Join Storage Session: disabled by default */
#ifndef JOIN_STORAGE_SESSION
    #define JOIN_STORAGE_SESSION  0
#endif

/* ── AWS Root CA (Amazon Root CA 1 — ATS chain used by all *.iot.<region>.amazonaws.com
 * endpoints, including the IoT credentials-provider endpoint this SDK connects to) ── */
#define AWS_CA_CERT_PEM \
    "-----BEGIN CERTIFICATE-----\n"                                            \
    "MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n"    \
    "ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\n"    \
    "b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\n"    \
    "MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n"    \
    "b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n"    \
    "ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n"    \
    "9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n"    \
    "IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n"    \
    "VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n"    \
    "93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n"    \
    "jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n"    \
    "AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\n"    \
    "A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\n"    \
    "U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\n"    \
    "N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\n"    \
    "o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\n"    \
    "5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\n"    \
    "rqXRfboQnoZsG4q5WTP468SQvvG5\n"                                        \
    "-----END CERTIFICATE-----\n"

/*
 * AWS_REGION and AWS_KVS_CHANNEL_NAME are required by app_common.c as
 * compile-time string literals.  Placeholder defaults are provided here;
 * kvs_webrtc_task.c reads the actual values from KVStore at runtime and
 * writes them into the AppContext before any SDK call consumes them.
 */
#ifndef AWS_REGION
    #define AWS_REGION            "us-east-1"
#endif
#ifndef AWS_KVS_CHANNEL_NAME
    #define AWS_KVS_CHANNEL_NAME  "kvs-channel"
#endif

#endif /* DEMO_CONFIG_H */
