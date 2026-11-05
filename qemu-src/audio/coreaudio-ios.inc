/*
 * QEMU CoreAudio driver for iOS: plays through a RemoteIO AudioUnit.
 *
 * Included from coreaudio.m when building for iOS, where the macOS HAL APIs
 * (AudioDevice*, AudioObject*) are unavailable. The host app is expected to
 * configure and activate the AVAudioSession. Keeps the "coreaudio" driver name
 * and options so command lines are the same on both platforms.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>

#include "qemu/module.h"
#include "audio.h"

#define AUDIO_CAP "coreaudio"
#include "audio_int.h"

typedef struct coreaudioVoiceOut {
    HWVoiceOut hw;
    pthread_mutex_t buf_mutex;
    AudioComponentInstance unit;
    bool running;
} coreaudioVoiceOut;

#define COREAUDIO_WRAPPER_FUNC(name, ret_type, args_decl, args) \
    static ret_type glue(coreaudio_, name)args_decl             \
    {                                                           \
        coreaudioVoiceOut *core = (coreaudioVoiceOut *) hw;     \
        ret_type ret;                                           \
        pthread_mutex_lock(&core->buf_mutex);                   \
        ret = glue(audio_generic_, name)args;                   \
        pthread_mutex_unlock(&core->buf_mutex);                 \
        return ret;                                             \
    }
COREAUDIO_WRAPPER_FUNC(buffer_get_free, size_t, (HWVoiceOut *hw), (hw))
COREAUDIO_WRAPPER_FUNC(get_buffer_out, void *, (HWVoiceOut *hw, size_t *size),
                       (hw, size))
COREAUDIO_WRAPPER_FUNC(put_buffer_out, size_t,
                       (HWVoiceOut *hw, void *buf, size_t size),
                       (hw, buf, size))
COREAUDIO_WRAPPER_FUNC(write, size_t, (HWVoiceOut *hw, void *buf, size_t size),
                       (hw, buf, size))
#undef COREAUDIO_WRAPPER_FUNC

/* Render callback (audio thread, no BQL): drain the emulated ring, pad with silence. */
static OSStatus coreaudio_render(void *ref, AudioUnitRenderActionFlags *flags,
                                 const AudioTimeStamp *ts, UInt32 bus,
                                 UInt32 frames, AudioBufferList *io)
{
    coreaudioVoiceOut *core = ref;
    HWVoiceOut *hw = &core->hw;
    uint8_t *out = io->mBuffers[0].mData;
    size_t len = MIN((size_t)frames * hw->info.bytes_per_frame,
                     (size_t)io->mBuffers[0].mDataByteSize);
    size_t copied = 0;

    pthread_mutex_lock(&core->buf_mutex);
    while (copied < len && hw->pending_emul) {
        size_t start = audio_ring_posb(hw->pos_emul, hw->pending_emul,
                                       hw->size_emul);
        size_t n = MIN(MIN(hw->pending_emul, len - copied),
                       hw->size_emul - start);
        memcpy(out + copied, hw->buf_emul + start, n);
        hw->pending_emul -= n;
        copied += n;
    }
    pthread_mutex_unlock(&core->buf_mutex);

    if (copied < len) {
        memset(out + copied, 0, len - copied);
        if (!copied) {
            *flags |= kAudioUnitRenderAction_OutputIsSilence;
        }
    }
    return noErr;
}

static void coreaudio_dispose(coreaudioVoiceOut *core)
{
    if (core->unit) {
        if (core->running) {
            AudioOutputUnitStop(core->unit);
            core->running = false;
        }
        AudioUnitUninitialize(core->unit);
        AudioComponentInstanceDispose(core->unit);
        core->unit = NULL;
    }
}

static int coreaudio_init_out(HWVoiceOut *hw, struct audsettings *as,
                              void *drv_opaque)
{
    coreaudioVoiceOut *core = (coreaudioVoiceOut *) hw;
    Audiodev *dev = drv_opaque;
    AudiodevCoreaudioPerDirectionOptions *cpdo = dev->u.coreaudio.out;
    struct audsettings obt_as = *as;
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_RemoteIO,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent comp;
    OSStatus st;
    uint32_t frames, count;

    pthread_mutex_init(&core->buf_mutex, NULL);

    obt_as.fmt = AUDIO_FORMAT_F32;
    audio_pcm_init_info(&hw->info, &obt_as);

    /* Generous buffering: the emulated guest delivers audio in bursts. */
    frames = audio_buffer_frames(
        qapi_AudiodevCoreaudioPerDirectionOptions_base(cpdo), &obt_as, 11610);
    count = cpdo->has_buffer_count ? cpdo->buffer_count : 8;
    hw->samples = frames * count;

    comp = AudioComponentFindNext(NULL, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &core->unit) != noErr) {
        dolog("RemoteIO audio unit unavailable\n");
        core->unit = NULL;
        return -1;
    }

    AudioStreamBasicDescription fmt = {
        .mSampleRate = obt_as.freq,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = hw->info.bytes_per_frame,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = hw->info.bytes_per_frame,
        .mChannelsPerFrame = obt_as.nchannels,
        .mBitsPerChannel = 32,
    };
    st = AudioUnitSetProperty(core->unit, kAudioUnitProperty_StreamFormat,
                              kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));
    if (st != noErr) {
        dolog("Could not set stream format (%d)\n", (int)st);
        goto fail;
    }

    AURenderCallbackStruct cb = { .inputProc = coreaudio_render,
                                  .inputProcRefCon = core };
    st = AudioUnitSetProperty(core->unit, kAudioUnitProperty_SetRenderCallback,
                              kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    if (st != noErr) {
        dolog("Could not set render callback (%d)\n", (int)st);
        goto fail;
    }

    /* Allow large slices (iOS uses 4096 frames when the screen is locked). */
    UInt32 max_frames = 4096;
    AudioUnitSetProperty(core->unit, kAudioUnitProperty_MaximumFramesPerSlice,
                         kAudioUnitScope_Global, 0, &max_frames,
                         sizeof(max_frames));

    st = AudioUnitInitialize(core->unit);
    if (st != noErr) {
        dolog("Could not initialize audio unit (%d)\n", (int)st);
        goto fail;
    }
    return 0;

fail:
    coreaudio_dispose(core);
    return -1;
}

static void coreaudio_fini_out(HWVoiceOut *hw)
{
    coreaudioVoiceOut *core = (coreaudioVoiceOut *) hw;

    coreaudio_dispose(core);
    pthread_mutex_destroy(&core->buf_mutex);
}

static void coreaudio_enable_out(HWVoiceOut *hw, bool enable)
{
    coreaudioVoiceOut *core = (coreaudioVoiceOut *) hw;

    if (!core->unit) {
        return;
    }
    if (enable && !core->running) {
        OSStatus st = AudioOutputUnitStart(core->unit);
        if (st == noErr) {
            core->running = true;
        } else {
            dolog("Could not start playback (%d)\n", (int)st);
        }
    } else if (!enable && core->running) {
        AudioOutputUnitStop(core->unit);
        core->running = false;
    }
}

static void *coreaudio_audio_init(Audiodev *dev, Error **errp)
{
    return dev;
}

static void coreaudio_audio_fini(void *opaque)
{
}

static struct audio_pcm_ops coreaudio_pcm_ops = {
    .init_out = coreaudio_init_out,
    .fini_out = coreaudio_fini_out,
    .write    = coreaudio_write,
    .buffer_get_free = coreaudio_buffer_get_free,
    .get_buffer_out = coreaudio_get_buffer_out,
    .put_buffer_out = coreaudio_put_buffer_out,
    .enable_out = coreaudio_enable_out
};

static struct audio_driver coreaudio_audio_driver = {
    .name           = "coreaudio",
    .descr          = "CoreAudio RemoteIO (iOS)",
    .init           = coreaudio_audio_init,
    .fini           = coreaudio_audio_fini,
    .pcm_ops        = &coreaudio_pcm_ops,
    .max_voices_out = 1,
    .max_voices_in  = 0,
    .voice_size_out = sizeof(coreaudioVoiceOut),
    .voice_size_in  = 0
};

static void register_audio_coreaudio(void)
{
    audio_driver_register(&coreaudio_audio_driver);
}
type_init(register_audio_coreaudio);
