// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Pull in the component's own configuration first, then switch on the classes
 * that its Kconfig does not expose.
 *
 * Both UAC implementations are compiled in and selected at boot, so the buffers
 * are sized for whichever one is used: UAC1 needs the larger IN endpoint (it has
 * no microphone but its descriptor historically reserved 100 bytes), UAC2 needs
 * the IN endpoint and two AudioStreaming interfaces. */
#include_next <tusb_config.h>

#undef CFG_TUD_AUDIO
#define CFG_TUD_AUDIO 1

#undef CFG_TUD_AUDIO_ENABLE_EP_OUT
#define CFG_TUD_AUDIO_ENABLE_EP_OUT 1

#undef CFG_TUD_AUDIO_ENABLE_EP_IN
#define CFG_TUD_AUDIO_ENABLE_EP_IN 1

/* The official ESP-IDF UAC component enables explicit feedback and corrects the
 * feedback format for macOS; AppleUSBAudio refuses a layout without it. */
#undef CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP 1

#undef CFG_TUD_AUDIO_ENABLE_FEEDBACK_FORMAT_CORRECTION
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_FORMAT_CORRECTION 1

#undef CFG_TUD_AUDIO_CTRL_BUF_SZ
#define CFG_TUD_AUDIO_CTRL_BUF_SZ 64

/* One millisecond of 48 kHz stereo 16-bit audio plus headroom. */
#undef CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX 196

/* UAC2 exposes playback + capture; UAC1 only playback (the extra slot stays
 * unused for it). */
#undef CFG_TUD_AUDIO_FUNC_1_N_AS_INT
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT 2

#undef CFG_TUD_AUDIO_FUNC_1_N_FORMATS
#define CFG_TUD_AUDIO_FUNC_1_N_FORMATS 1

#undef CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX 100

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
