// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Pull in the component's own configuration first, then switch on the classes
 * that its Kconfig does not expose. */
#include_next <tusb_config.h>

/* Audio class (UAC1) for the composite sound card. */
#undef CFG_TUD_AUDIO
#define CFG_TUD_AUDIO 1

#undef CFG_TUD_AUDIO_ENABLE_EP_OUT
#define CFG_TUD_AUDIO_ENABLE_EP_OUT 1

/* Microphone capture: the descriptor below adds a mono IN endpoint. */
#undef CFG_TUD_AUDIO_ENABLE_EP_IN
#define CFG_TUD_AUDIO_ENABLE_EP_IN 1

/* The official ESP-IDF UAC component enables explicit feedback and corrects
 * the feedback format for macOS; AppleUSBAudio refuses a UAC2 playback
 * endpoint that has no feedback path. */
#undef CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP 1

#undef CFG_TUD_AUDIO_ENABLE_FEEDBACK_FORMAT_CORRECTION
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_FORMAT_CORRECTION 1

#undef CFG_TUD_AUDIO_CTRL_BUF_SZ
#define CFG_TUD_AUDIO_CTRL_BUF_SZ 64

/* Software buffers for one UAC1 function: 48 kHz, 16-bit, stereo out / mono in,
 * one 1 ms packet above the nominal size. */
#undef CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX 196
/* Two AudioStreaming interfaces: playback and capture. */
#undef CFG_TUD_AUDIO_FUNC_1_N_AS_INT
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT 2
#undef CFG_TUD_AUDIO_FUNC_1_N_FORMATS
#define CFG_TUD_AUDIO_FUNC_1_N_FORMATS 1

/* Mono 48 kHz 16-bit needs 12 bytes per microframe; 16 leaves headroom. */
#undef CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX 16

#undef CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ (196 * 11)

#undef CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ 1024

/* Temporary diagnosis: let TinyUSB print class-driver parse failures. */
#undef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 3

/* Report dropped isochronous OUT packets as completed transfers so the audio
 * class re-arms them. Without this the DWC2 receive path stays unarmed after a
 * single dropped packet and the stream dies. */
#undef CFG_TUD_DWC2_PTI_ENABLE
#define CFG_TUD_DWC2_PTI_ENABLE 0
