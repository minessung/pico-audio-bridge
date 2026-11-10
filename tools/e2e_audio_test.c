#include <CoreAudio/CoreAudio.h>
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define SAMPLE_RATE 48000
#define TEST_SECONDS 2
#define TOTAL_FRAMES (SAMPLE_RATE * TEST_SECONDS)

typedef struct {
    float freq_l;
    float freq_r;
    double phase_l;
    double phase_r;
    uint32_t frames_played;

    int16_t *rec_buf; // interleaved stereo (L, R)
    uint32_t frames_recorded;
    uint32_t max_frames;
} test_ctx_t;

static AudioDeviceID find_audio_device(const char *substr, int is_input) {
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 data_size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, NULL, &data_size);
    int count = data_size / sizeof(AudioDeviceID);
    AudioDeviceID *devs = malloc(data_size);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, NULL, &data_size, devs);

    AudioDeviceID found = kAudioObjectUnknown;
    for (int i = 0; i < count; i++) {
        // Check channels in requested scope
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
        int ch_cnt = 0;
        for (UInt32 b = 0; b < abl->mNumberBuffers; b++) ch_cnt += abl->mBuffers[b].mNumberChannels;
        free(abl);
        if (ch_cnt == 0) continue;

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
                printf("  Found %s device [ID=%u]: %s (%d ch)\n", is_input ? "INPUT " : "OUTPUT", (unsigned)found, name, ch_cnt);
                break;
            }
        }
    }
    free(devs);
    return found;
}

static OSStatus PlayIOProc(AudioDeviceID inDevice, const AudioTimeStamp *inNow,
                           const AudioBufferList *inInputData, const AudioTimeStamp *inInputTime,
                           AudioBufferList *outOutputData, const AudioTimeStamp *inOutputTime,
                           void *inClientData) {
    (void)inDevice; (void)inNow; (void)inInputData; (void)inInputTime; (void)inOutputTime;
    test_ctx_t *ctx = (test_ctx_t *)inClientData;
    float *out = (float *)outOutputData->mBuffers[0].mData;
    UInt32 frames = outOutputData->mBuffers[0].mDataByteSize / (2 * sizeof(float));

    const double step_l = 2.0 * M_PI * ctx->freq_l / SAMPLE_RATE;
    const double step_r = 2.0 * M_PI * ctx->freq_r / SAMPLE_RATE;

    for (UInt32 i = 0; i < frames; i++) {
        out[2 * i]     = (float)(0.5 * sin(ctx->phase_l));
        out[2 * i + 1] = (float)(0.5 * sin(ctx->phase_r));
        ctx->phase_l += step_l;
        ctx->phase_r += step_r;
    }
    ctx->frames_played += frames;
    return noErr;
}

static OSStatus RecIOProc(AudioDeviceID inDevice, const AudioTimeStamp *inNow,
                          const AudioBufferList *inInputData, const AudioTimeStamp *inInputTime,
                          AudioBufferList *outOutputData, const AudioTimeStamp *inOutputTime,
                          void *inClientData) {
    (void)inDevice; (void)inNow; (void)inInputTime; (void)outOutputData; (void)inOutputTime;
    test_ctx_t *ctx = (test_ctx_t *)inClientData;
    const float *in = (const float *)inInputData->mBuffers[0].mData;
    UInt32 frames = inInputData->mBuffers[0].mDataByteSize / (2 * sizeof(float));

    for (UInt32 i = 0; i < frames; i++) {
        if (ctx->frames_recorded < ctx->max_frames) {
            float l = in[2 * i];
            float r = in[2 * i + 1];
            if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
            if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
            ctx->rec_buf[2 * ctx->frames_recorded]     = (int16_t)(l * 32767.0f);
            ctx->rec_buf[2 * ctx->frames_recorded + 1] = (int16_t)(r * 32767.0f);
            ctx->frames_recorded++;
        }
    }
    return noErr;
}

// Compute dominant frequency via Goertzel/DFT scan and RMS level on steady-state window
static void analyze_channel(const int16_t *rec_buf, int ch_offset, uint32_t start_frame, uint32_t num_frames,
                            double *out_rms_db, double *out_peak_freq, int16_t *out_peak_amp) {
    double sum_sq = 0.0;
    int16_t max_abs = 0;
    for (uint32_t i = 0; i < num_frames; i++) {
        int16_t s = rec_buf[2 * (start_frame + i) + ch_offset];
        int16_t a = s < 0 ? -s : s;
        if (a > max_abs) max_abs = a;
        double v = (double)s / 32768.0;
        sum_sq += v * v;
    }
    double rms = sqrt(sum_sq / num_frames);
    *out_rms_db = (rms > 1e-9) ? (20.0 * log10(rms)) : -120.0;
    *out_peak_amp = max_abs;

    // Scan frequencies from 100 Hz to 4000 Hz in 10 Hz steps
    double best_mag = -1.0;
    double best_freq = 0.0;
    uint32_t dft_N = 4800; // 100ms window -> exact 10Hz bins
    for (int f = 100; f <= 4000; f += 10) {
        double re = 0.0, im = 0.0;
        double w = 2.0 * M_PI * f / SAMPLE_RATE;
        for (uint32_t n = 0; n < dft_N; n++) {
            double s = (double)rec_buf[2 * (start_frame + n) + ch_offset] / 32768.0;
            re += s * cos(w * n);
            im -= s * sin(w * n);
        }
        double mag = sqrt(re * re + im * im);
        if (mag > best_mag) {
            best_mag = mag;
            best_freq = f;
        }
    }
    *out_peak_freq = best_freq;
}

static int run_direction_test(const char *tx_name, const char *rx_name, float freq_l, float freq_r) {
    printf("\n============================================================\n");
    printf(" Testing Audio Direction: [%s] ===(I2S)===> [%s]\n", tx_name, rx_name);
    printf(" Test Signal: Left = %.0f Hz, Right = %.0f Hz (-6 dBFS Sine)\n", freq_l, freq_r);
    printf("============================================================\n");

    AudioDeviceID dev_out = find_audio_device(tx_name, 0);
    AudioDeviceID dev_in  = find_audio_device(rx_name, 1);
    if (dev_out == kAudioObjectUnknown || dev_in == kAudioObjectUnknown) {
        printf("ERROR: Could not find audio devices!\n");
        return -1;
    }

    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.freq_l = freq_l;
    ctx.freq_r = freq_r;
    ctx.max_frames = TOTAL_FRAMES;
    ctx.rec_buf = calloc(TOTAL_FRAMES * 2, sizeof(int16_t));

    AudioDeviceIOProcID play_proc = NULL, rec_proc = NULL;
    AudioDeviceCreateIOProcID(dev_out, PlayIOProc, &ctx, &play_proc);
    AudioDeviceCreateIOProcID(dev_in,  RecIOProc,  &ctx, &rec_proc);

    AudioDeviceStart(dev_out, play_proc);
    AudioDeviceStart(dev_in,  rec_proc);

    usleep(1600000); // Run for 1.6 seconds

    AudioDeviceStop(dev_in,  rec_proc);
    AudioDeviceStop(dev_out, play_proc);
    AudioDeviceDestroyIOProcID(dev_in,  rec_proc);
    AudioDeviceDestroyIOProcID(dev_out, play_proc);

    printf("  Frames Played  : %u\n", ctx.frames_played);
    printf("  Frames Recorded: %u\n", ctx.frames_recorded);

    if (ctx.frames_recorded < SAMPLE_RATE) {
        printf("ERROR: Not enough frames recorded!\n");
        free(ctx.rec_buf);
        return -1;
    }

    // Analyze steady-state window starting at 0.5s (frame 24000) for 0.5s (24000 frames)
    double rms_l, rms_r, freq_det_l, freq_det_r;
    int16_t peak_l, peak_r;
    analyze_channel(ctx.rec_buf, 0, 24000, 24000, &rms_l, &freq_det_l, &peak_l);
    analyze_channel(ctx.rec_buf, 1, 24000, 24000, &rms_r, &freq_det_r, &peak_r);

    printf("  [Left  Channel] Peak=%5d/32767, RMS=%6.2f dBFS, Detected Freq=%6.1f Hz (Expected %.1f Hz)\n",
           peak_l, rms_l, freq_det_l, freq_l);
    printf("  [Right Channel] Peak=%5d/32767, RMS=%6.2f dBFS, Detected Freq=%6.1f Hz (Expected %.1f Hz)\n",
           peak_r, rms_r, freq_det_r, freq_r);

    free(ctx.rec_buf);
    return 0;
}

static int run_full_duplex_test(void) {
    printf("\n============================================================\n");
    printf(" Testing SIMULTANEOUS FULL-DUPLEX (Master <===(I2S)===> Slave)\n");
    printf("   Path A (Master -> Slave): L=1000 Hz, R=2000 Hz\n");
    printf("   Path B (Slave -> Master): L=1500 Hz, R=2500 Hz\n");
    printf("============================================================\n");

    AudioDeviceID master_out = find_audio_device("Master", 0);
    AudioDeviceID master_in  = find_audio_device("Master", 1);
    AudioDeviceID slave_out  = find_audio_device("Slave",  0);
    AudioDeviceID slave_in   = find_audio_device("Slave",  1);

    if (!master_out || !master_in || !slave_out || !slave_in) {
        printf("ERROR: Missing audio devices!\n");
        return -1;
    }

    test_ctx_t ctx_a = { .freq_l = 1000.0f, .freq_r = 2000.0f, .max_frames = TOTAL_FRAMES, .rec_buf = calloc(TOTAL_FRAMES * 2, sizeof(int16_t)) };
    test_ctx_t ctx_b = { .freq_l = 1500.0f, .freq_r = 2500.0f, .max_frames = TOTAL_FRAMES, .rec_buf = calloc(TOTAL_FRAMES * 2, sizeof(int16_t)) };

    AudioDeviceIOProcID p_a = NULL, r_a = NULL, p_b = NULL, r_b = NULL;
    AudioDeviceCreateIOProcID(master_out, PlayIOProc, &ctx_a, &p_a);
    AudioDeviceCreateIOProcID(slave_in,   RecIOProc,  &ctx_a, &r_a);
    AudioDeviceCreateIOProcID(slave_out,  PlayIOProc, &ctx_b, &p_b);
    AudioDeviceCreateIOProcID(master_in,  RecIOProc,  &ctx_b, &r_b);

    AudioDeviceStart(master_out, p_a);
    AudioDeviceStart(slave_in,   r_a);
    AudioDeviceStart(slave_out,  p_b);
    AudioDeviceStart(master_in,  r_b);

    usleep(1600000); // Run all 4 streams simultaneously for 1.6s

    AudioDeviceStop(master_in,  r_b);
    AudioDeviceStop(slave_out,  p_b);
    AudioDeviceStop(slave_in,   r_a);
    AudioDeviceStop(master_out, p_a);

    AudioDeviceDestroyIOProcID(master_in,  r_b);
    AudioDeviceDestroyIOProcID(slave_out,  p_b);
    AudioDeviceDestroyIOProcID(slave_in,   r_a);
    AudioDeviceDestroyIOProcID(master_out, p_a);

    double rms_l, rms_r, freq_l, freq_r;
    int16_t peak_l, peak_r;

    printf("  --- Path A: Master Speaker -> Slave Mic (Played %u, Recorded %u) ---\n", ctx_a.frames_played, ctx_a.frames_recorded);
    analyze_channel(ctx_a.rec_buf, 0, 24000, 24000, &rms_l, &freq_l, &peak_l);
    analyze_channel(ctx_a.rec_buf, 1, 24000, 24000, &rms_r, &freq_r, &peak_r);
    printf("    [Left ] Peak=%5d/32767, RMS=%6.2f dBFS, Freq=%6.1f Hz (Expected 1000.0 Hz)\n", peak_l, rms_l, freq_l);
    printf("    [Right] Peak=%5d/32767, RMS=%6.2f dBFS, Freq=%6.1f Hz (Expected 2000.0 Hz)\n", peak_r, rms_r, freq_r);

    printf("  --- Path B: Slave Speaker -> Master Mic (Played %u, Recorded %u) ---\n", ctx_b.frames_played, ctx_b.frames_recorded);
    analyze_channel(ctx_b.rec_buf, 0, 24000, 24000, &rms_l, &freq_l, &peak_l);
    analyze_channel(ctx_b.rec_buf, 1, 24000, 24000, &rms_r, &freq_r, &peak_r);
    printf("    [Left ] Peak=%5d/32767, RMS=%6.2f dBFS, Freq=%6.1f Hz (Expected 1500.0 Hz)\n", peak_l, rms_l, freq_l);
    printf("    [Right] Peak=%5d/32767, RMS=%6.2f dBFS, Freq=%6.1f Hz (Expected 2500.0 Hz)\n", peak_r, rms_r, freq_r);

    free(ctx_a.rec_buf);
    free(ctx_b.rec_buf);
    return 0;
}

int main(void) {
    // Test 1: Pico #1 (Master / Android Phone side) Speaker -> I2S -> Pico #2 (Slave / PC side) Mic
    run_direction_test("Master", "Slave", 1000.0f, 2000.0f);

    // Test 2: Pico #2 (Slave / PC side) Speaker -> I2S -> Pico #1 (Master / Android Phone side) Mic
    run_direction_test("Slave", "Master", 1500.0f, 2500.0f);

    // Test 3: Simultaneous Full-Duplex in both directions at the same time
    run_full_duplex_test();

    return 0;
}
