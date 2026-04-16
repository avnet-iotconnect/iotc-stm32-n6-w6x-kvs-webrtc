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

/* ── AWS Root CA (Starfield G2 — same as used for AWS IoT MQTT) ───────────── */
#define AWS_CA_CERT_PEM \
    "-----BEGIN CERTIFICATE-----\n"                                            \
    "MIID7zCCAtegAwIBAgIBADANBgkqhkiG9w0BAQsFADCBmDELMAkGA1UEBhMCVVMx\n"    \
    "EDAOBgNVBAgTB0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxJTAjBgNVBAoT\n"    \
    "HFN0YXJmaWVsZCBUZWNobm9sb2dpZXMsIEluYy4xOzA5BgNVBAMTMlN0YXJmaWVs\n"    \
    "ZCBTZXJ2aWNlcyBSb290IENlcnRpZmljYXRlIEF1dGhvcml0eSAtIEcyMB4XDTA5\n"    \
    "MDkwMTAwMDAwMFoXDTM3MTIzMTIzNTk1OVowgZgxCzAJBgNVBAYTAlVTMRAwDgYD\n"    \
    "VQQIEwdBcml6b25hMRMwEQYDVQQHEwpTY290dHNkYWxlMSUwIwYDVQQKExxTdGFy\n"    \
    "ZmllbGQgVGVjaG5vbG9naWVzLCBJbmMuMTswOQYDVQQDEzJTdGFyZmllbGQgU2Vy\n"    \
    "dmljZXMgUm9vdCBDZXJ0aWZpY2F0ZSBBdXRob3JpdHkgLSBHMjCCASIwDQYJKoZI\n"    \
    "hvcNAQEBBQADggEPADCCAQoCggEBANUMOsQq+U7i9b4Zl1+OiFOxHz/Lz58gE20p\n"    \
    "OsgPfTz3a3Y4Y9k2YKibXlwAgLIvWX/2h/klQ4bnaRtSmpDhcePYLQ1Ob/bISdm2\n"    \
    "8xpWriu2dBTrz/sm4xq6HZYuajtYlIlHVv8loJNwU4PahHQUw2eeBGg6345AWh1K\n"    \
    "Ts9DkTvnVtYAcMtS7nt9rjrnvDH5RfbCYM8TWQIrgMw0R9+53pBlbQLPLJGmpufe\n"    \
    "hRhJfGZOozptqbXuNC66DQO4M99H67FrjSXZm86B0UVGMpZwh94CDklDhbZsc7tk\n"    \
    "6mFBrMnUVN+HL8cisibMn1lUaJ/8viovxFUcdUBgF4UCVTmLfwUCAwEAAaNCMEAw\n"    \
    "DwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYEFJxfAN+q\n"    \
    "AdcwKziIorhtSpzyEZGDMA0GCSqGSIb3DQEBCwUAA4IBAQBLNqaEd2ndOxmfZyMI\n"    \
    "bw5hyf2E3F/YNoHN2BtBLZ9g3ccaaNnRbobhiCPPE95Dz+I0swSdHynVv/heyNXB\n"    \
    "ve6SbzJ08pGCL72CQnqtKrcgfU28elUSwhXqvfdqlS5sdJ/PHLTyxQGjhdByPq1z\n"    \
    "qwubdQxtRbeOlKyWN7Wg0I8VRw7j6IPdj/3vQQF3zCepYoUz8jcI73HPdwbeyBkd\n"    \
    "iEDPfUYd/x7H4c7/I9vG+o1VTqkC50cRRj70/b17KSa7qWFiNyi2LSr2EIZkyXCn\n"    \
    "0q23KXB56jzaYyWf/Wi3MOxw+3WKt21gZ7IeyLnp2KhvAotnDU0mV3HaIPzBSlCN\n"    \
    "sSi6\n"                                                                    \
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
