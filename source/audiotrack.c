/*
 * android.media.AudioTrack on top of sceAudioOut, for Vox's AudioTrack driver
 * (vox::DriverAndroid::_InitAT / UpdateThreadedAT / DoCallbackAT in the .so):
 *
 *   minBuf = AudioTrack.getMinBufferSize(44100, CHANNEL_OUT_STEREO, PCM_16BIT)
 *   frames = min(minBuf / 4, 1024)
 *   track  = new AudioTrack(STREAM_MUSIC, 44100, 0xc, 2, minBuf, MODE_STREAM)
 *   track.play(); loop { mix `frames` frames; track.write(buf, 0, frames * 4) }
 *   track.stop(); track.release();
 *
 * write() is a blocking sceAudioOutOutput(), which paces Vox's mixer thread
 * at the real playback rate -- the same contract as AudioTrack MODE_STREAM.
 * Vox only ever has one track, and CallNonvirtual*Method doesn't pass the
 * object through FalsoJNI's va_list, so a single global track is used.
 */

#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_ImplBridge.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <stdlib.h>
#include <string.h>

#include "utils/logger.h"

// Frames per sceAudioOutOutput call. getMinBufferSize() is chosen so that
// Vox's write size (frames * 4 bytes) is exactly one grain.
#define AT_GRAIN      1024
#define AT_MIN_BUFFER (AT_GRAIN * 2 * 4)

#define CHANNEL_OUT_MONO   0x4
#define CHANNEL_OUT_STEREO 0xc

static struct {
    int port;
    int channels;
    int frame_bytes;
    uint8_t *pending;      // one grain, filled across write() calls
    int pending_len;
    int playing;
    unsigned long long written;
} at = { .port = -1 };

// AudioTrack.getMinBufferSize(III)I
jint impl_AudioTrack_getMinBufferSize(jmethodID id, va_list args) {
    return AT_MIN_BUFFER;
}

// new AudioTrack(IIIIII)V -> returns the (single) track handle
jobject impl_AudioTrack_init(jmethodID id, va_list args) {
    int stream = va_arg(args, int);
    int rate = va_arg(args, int);
    int channel_cfg = va_arg(args, int);
    int format = va_arg(args, int);
    int buf_size = va_arg(args, int);
    int mode = va_arg(args, int);

    if (at.port >= 0)
        sceAudioOutReleasePort(at.port);

    at.channels = (channel_cfg == CHANNEL_OUT_MONO) ? 1 : 2;
    at.frame_bytes = at.channels * 2;
    at.pending_len = 0;
    at.written = 0;
    free(at.pending);
    at.pending = malloc(AT_GRAIN * at.frame_bytes);

    // BGM ports accept 8-48 kHz; MAIN is 48 kHz only.
    at.port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AT_GRAIN, rate,
                                  at.channels == 2 ? SCE_AUDIO_OUT_MODE_STEREO
                                                   : SCE_AUDIO_OUT_MODE_MONO);
    if (at.port < 0 || !at.pending) {
        l_error("[audiotrack] sceAudioOutOpenPort(rate %d, ch %d) failed: 0x%x",
                rate, at.channels, at.port);
        at.port = -1;
        return NULL; // Vox logs "AudioTrack driver could not initialize"
    }

    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(at.port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);

    l_success("[audiotrack] new AudioTrack(stream %d, %d Hz, cfg 0x%x, fmt %d, buf %d, mode %d) -> port %d",
              stream, rate, channel_cfg, format, buf_size, mode, at.port);
    return (jobject)&at;
}

// AudioTrack.write([BII)I
jint impl_AudioTrack_write(jmethodID id, va_list args) {
    JavaDynArray *jda = va_arg(args, JavaDynArray *);
    int offset = va_arg(args, int);
    int size = va_arg(args, int);

    if (!jda || !jda->array || size <= 0)
        return size;
    if (at.port < 0 || !at.playing) {
        static int warned = 0;
        if (warned++ < 3)
            l_warn("[audiotrack] write(%d) while %s -- dropped", size,
                   at.port < 0 ? "no port" : "not playing");
        // Keep Vox's thread from spinning if we're paused / not open.
        sceKernelDelayThread(AT_GRAIN * 1000000ULL / 44100);
        return size;
    }

    const uint8_t *src = (const uint8_t *)jda->array + offset;
    int grain_bytes = AT_GRAIN * at.frame_bytes;
    int left = size;

    if (at.written == 0)
        l_success("[audiotrack] first write(%d bytes) -- Vox output path is alive", size);

    while (left > 0) {
        if (at.pending_len == 0 && left >= grain_bytes) {
            sceAudioOutOutput(at.port, src); // blocks until the hw consumed it
            src += grain_bytes;
            left -= grain_bytes;
            continue;
        }
        int n = grain_bytes - at.pending_len;
        if (n > left)
            n = left;
        memcpy(at.pending + at.pending_len, src, n);
        at.pending_len += n;
        src += n;
        left -= n;
        if (at.pending_len == grain_bytes) {
            sceAudioOutOutput(at.port, at.pending);
            at.pending_len = 0;
        }
    }
    at.written += size;
    return size;
}

void impl_AudioTrack_play(jmethodID id, va_list args) {
    l_info("[audiotrack] play()");
    at.playing = 1;
}

void impl_AudioTrack_pause(jmethodID id, va_list args) {
    l_info("[audiotrack] pause()");
    at.playing = 0;
}

void impl_AudioTrack_stop(jmethodID id, va_list args) {
    l_info("[audiotrack] stop()");
    at.playing = 0;
    at.pending_len = 0;
}

void impl_AudioTrack_release(jmethodID id, va_list args) {
    at.playing = 0;
    if (at.port >= 0) {
        sceAudioOutOutput(at.port, NULL); // drain
        sceAudioOutReleasePort(at.port);
        at.port = -1;
    }
    free(at.pending);
    at.pending = NULL;
    at.pending_len = 0;
}
