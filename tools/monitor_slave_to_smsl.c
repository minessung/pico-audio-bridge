#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>

#define RING_FRAMES 32768
#define RING_MASK   (RING_FRAMES - 1)
#define TARGET_FILL 4096

typedef struct {
    float buf_l[RING_FRAMES];
    float buf_r[RING_FRAMES];
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
    int prebuffering;
    double frac_pos;
    double in_rate;
    double out_rate;
    volatile float peak_l;
    volatile float peak_r;
    volatile uint64_t total_in_frames;
    volatile uint64_t total_out_frames;
} monitor_ctx_t;

static volatile int g_running = 1;
static void sig_handler(int sig) { (void)sig; g_running = 0; }

static AudioDeviceID find_device(const char *substr, int is_input, double *out_rate) {
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, NULL, &size);
    int count = size / sizeof(AudioDeviceID);
    AudioDeviceID *devs = malloc(size);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, NULL, &size, devs);

    AudioDeviceID found = kAudioObjectUnknown;
    for (int i = 0; i < count; i++) {
        AudioObjectPropertyAddress ch_prop = {
            kAudioDevicePropertyStreamConfiguration,
            is_input ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        UInt32 ch_size = 0;
        if (AudioObjectGetPropertyDataSize(devs[i], &ch_prop, 0, NULL, &ch_size) != noErr || ch_size == 0)
            continue;
        AudioBufferList *abl = malloc(ch_size);
        AudioObjectGetPropertyData(devs[i], &ch_prop, 0, NULL, &ch_size, abl);
        int ch = 0;
        for (UInt32 b = 0; b < abl->mNumberBuffers; b++) ch += abl->mBuffers[b].mNumberChannels;
        free(abl);
        if (ch == 0) continue;

        CFStringRef name_ref = NULL;
        UInt32 name_size = sizeof(name_ref);
        AudioObjectPropertyAddress name_prop = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(devs[i], &name_prop, 0, NULL, &name_size, &name_ref) == noErr && name_ref) {
            char name[256];
            CFStringGetCString(name_ref, name, sizeof(name), kCFStringEncodingUTF8);
            CFRelease(name_ref);
            if (strstr(name, substr) != NULL) {
                found = devs[i];
                AudioObjectPropertyAddress sr_prop = {
                    kAudioDevicePropertyNominalSampleRate,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                Float64 sr = 48000.0;
                UInt32 sr_size = sizeof(sr);
                if (!is_input) {
                    Float64 target_sr = 48000.0;
                    AudioObjectSetPropertyData(found, &sr_prop, 0, NULL, sizeof(target_sr), &target_sr);
                    usleep(100000);
                }
                AudioObjectGetPropertyData(found, &sr_prop, 0, NULL, &sr_size, &sr);
                if (out_rate) *out_rate = sr;
                printf("Matched %s Device [ID=%u]: '%s' (%d ch @ %.0f Hz)\n",
                       is_input ? "INPUT " : "OUTPUT", (unsigned)found, name, ch, sr);
                break;
            }
        }
    }
    free(devs);
    return found;
}

static OSStatus InputIOProc(AudioDeviceID inDevice, const AudioTimeStamp *inNow,
                            const AudioBufferList *inInputData, const AudioTimeStamp *inInputTime,
                            AudioBufferList *outOutputData, const AudioTimeStamp *inOutputTime,
                            void *inClientData) {
    (void)inDevice; (void)inNow; (void)inInputTime; (void)outOutputData; (void)inOutputTime;
    monitor_ctx_t *ctx = (monitor_ctx_t *)inClientData;
    const float *in = (const float *)inInputData->mBuffers[0].mData;
    UInt32 frames = inInputData->mBuffers[0].mDataByteSize / (2 * sizeof(float));

    uint32_t w = ctx->write_idx;
    uint32_t r = ctx->read_idx;
    float pl = ctx->peak_l, pr = ctx->peak_r;

    for (UInt32 i = 0; i < frames; i++) {
        float l  = in[2 * i];
        float rt = in[2 * i + 1];

        float al = fabsf(l), ar = fabsf(rt);
        if (al > pl) pl = al;
        if (ar > pr) pr = ar;

        if ((uint32_t)(w - r) < (RING_FRAMES - 4)) {
            ctx->buf_l[w & RING_MASK] = l;
            ctx->buf_r[w & RING_MASK] = rt;
            w++;
        }
    }
    ctx->write_idx = w;
    ctx->peak_l = pl;
    ctx->peak_r = pr;
    ctx->total_in_frames += frames;
    return noErr;
}

static OSStatus OutputIOProc(AudioDeviceID inDevice, const AudioTimeStamp *inNow,
                             const AudioBufferList *inInputData, const AudioTimeStamp *inInputTime,
                             AudioBufferList *outOutputData, const AudioTimeStamp *inOutputTime,
                             void *inClientData) {
    (void)inDevice; (void)inNow; (void)inInputData; (void)inInputTime; (void)inOutputTime;
    monitor_ctx_t *ctx = (monitor_ctx_t *)inClientData;
    float *out = (float *)outOutputData->mBuffers[0].mData;
    UInt32 frames = outOutputData->mBuffers[0].mDataByteSize / (2 * sizeof(float));

    uint32_t w = ctx->write_idx;
    uint32_t r = ctx->read_idx;
    uint32_t avail = w - r;

    if (ctx->prebuffering) {
        if (avail < TARGET_FILL) {
            memset(out, 0, frames * 2 * sizeof(float));
            return noErr;
        }
        ctx->prebuffering = 0;
    } else if (avail < frames + 4) {
        ctx->prebuffering = 1;
        memset(out, 0, frames * 2 * sizeof(float));
        return noErr;
    }

    double base_step = ctx->in_rate / ctx->out_rate;
    int32_t err = (int32_t)avail - TARGET_FILL;
    // Ultra-gentle drift tracking: max +/- 500 ppm (+/- 0.05%), completely inaudible pitch stability
    double step = base_step + (double)err * 0.0000002;
    if (step < base_step * 0.9995) step = base_step * 0.9995;
    if (step > base_step * 1.0005) step = base_step * 1.0005;

    double pos = ctx->frac_pos;
    for (UInt32 i = 0; i < frames; i++) {
        uint32_t idx0 = (r + (uint32_t)pos) & RING_MASK;
        uint32_t idx1 = (idx0 + 1) & RING_MASK;
        float frac = (float)(pos - (uint32_t)pos);

        out[2 * i]     = ctx->buf_l[idx0] + frac * (ctx->buf_l[idx1] - ctx->buf_l[idx0]);
        out[2 * i + 1] = ctx->buf_r[idx0] + frac * (ctx->buf_r[idx1] - ctx->buf_r[idx0]);

        pos += step;
    }

    uint32_t advance = (uint32_t)pos;
    if (advance > avail - 2) advance = avail - 2;
    ctx->read_idx = r + advance;
    ctx->frac_pos = pos - (double)advance;
    ctx->total_out_frames += frames;
    return noErr;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    monitor_ctx_t *ctx = calloc(1, sizeof(monitor_ctx_t));
    ctx->in_rate = 48000.0;
    ctx->out_rate = 48000.0;
    ctx->prebuffering = 1;

    AudioDeviceID dev_in  = find_device("Slave", 1, &ctx->in_rate);
    if (dev_in == kAudioObjectUnknown) {
        dev_in = find_device("Pico UAC1", 1, &ctx->in_rate);
    }
    AudioDeviceID dev_out = find_device("SMSL",  0, &ctx->out_rate);

    if (dev_in == kAudioObjectUnknown || dev_out == kAudioObjectUnknown) {
        printf("ERROR: Could not find Slave Input or SMSL Output device!\n");
        return 1;
    }

    AudioDeviceIOProcID in_proc = NULL, out_proc = NULL;
    AudioDeviceCreateIOProcID(dev_in,  InputIOProc,  ctx, &in_proc);
    AudioDeviceCreateIOProcID(dev_out, OutputIOProc, ctx, &out_proc);

    AudioDeviceStart(dev_in,  in_proc);
    AudioDeviceStart(dev_out, out_proc);

    printf("==> Live Monitoring ACTIVE: [Android Phone -> Pico #1 -> I2S -> Pico #2 (Slave)] ===> [SMSL USB AUDIO]\n");

    while (g_running) {
        usleep(1000000);
        float pl = ctx->peak_l;
        float pr = ctx->peak_r;
        ctx->peak_l = 0.0f;
        ctx->peak_r = 0.0f;
        float db_l = (pl > 1e-6f) ? 20.0f * log10f(pl) : -120.0f;
        float db_r = (pr > 1e-6f) ? 20.0f * log10f(pr) : -120.0f;
        uint32_t fill = ctx->write_idx - ctx->read_idx;
        printf("[Monitor] In=%llu Out=%llu Buf=%u | Peak L=%6.1f dBFS, R=%6.1f dBFS\n",
               (unsigned long long)ctx->total_in_frames,
               (unsigned long long)ctx->total_out_frames,
               fill, db_l, db_r);
    }

    AudioDeviceStop(dev_out, out_proc);
    AudioDeviceStop(dev_in,  in_proc);
    AudioDeviceDestroyIOProcID(dev_out, out_proc);
    AudioDeviceDestroyIOProcID(dev_in,  in_proc);
    free(ctx);
    return 0;
}
