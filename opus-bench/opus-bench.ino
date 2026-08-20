/*
 * opus-bench — how much Opus can an ESP32 actually afford?
 *
 * The handset both encodes (mic -> peer) and decodes (peer -> speaker), so the
 * number that matters is encode+decode per frame versus the frame's own
 * duration.  A 20 ms frame that costs 8 ms of encode and 2 ms of decode leaves
 * 50% of one core for I2S, WiFi and the LEDs; a 20 ms frame that costs 19 ms
 * leaves nothing and will stutter the moment the radio gets busy.
 *
 * Flash this on its own (no mic, speaker or network needed), open the serial
 * monitor at 115200, and it prints a table of every candidate configuration on
 * the real silicon.  Pick a row, copy its numbers into the handset sketch.
 */

#include "opus.h"
#include "esp_timer.h"

// ─────────────────────────────────────────────────────────────────────────────
// Test signal
//
// Opus is adaptive: its cost depends on what you feed it.  Silence is cheap,
// steady tones are cheap, and real speech — pitched voicing, noisy fricatives,
// transients at word onsets — is expensive.  Benchmarking a sine wave would
// flatter every configuration here, so this synthesises a voice-shaped signal
// instead: a glottal pulse train with drifting pitch, run through three formant
// resonators, interrupted by fricative noise and by pauses.
// ─────────────────────────────────────────────────────────────────────────────
static const int   MAX_FRAME_SAMPLES = 48000 / 1000 * 60;   // 60 ms at 48 kHz
static const int   MAX_PACKET_BYTES  = 512;

struct Formant {
    float y1 = 0, y2 = 0, a1 = 0, a2 = 0, gain = 0;

    void set(float freq_hz, float bw_hz, float sample_rate) {
        float r = expf(-PI * bw_hz / sample_rate);
        a1   = 2.0f * r * cosf(2.0f * PI * freq_hz / sample_rate);
        a2   = -r * r;
        gain = (1.0f - a1 - a2);
    }
    float step(float x) {
        float y = gain * x + a1 * y1 + a2 * y2;
        y2 = y1;
        y1 = y;
        return y;
    }
};

class VoiceLikeSource {
public:
    void begin(int sample_rate) {
        fs_ = sample_rate;
        f1_.set(700,  90,  fs_);   // roughly an /a/
        f2_.set(1220, 110, fs_);
        f3_.set(2600, 170, fs_);
        phase_ = 0;
        t_     = 0;
        rng_   = 22222;
    }

    // Fills `n` samples of int16 mono.
    void fill(int16_t* dst, int n) {
        for (int i = 0; i < n; i++) {
            // A 2.4 s cycle: 1.2 s voiced, 0.6 s fricative, 0.6 s silence.
            float cycle = fmodf((float)t_ / fs_, 2.4f);
            float x;

            if (cycle < 1.2f) {
                float f0 = 110.0f + 40.0f * sinf(2.0f * PI * 0.8f * cycle);   // intonation
                phase_ += f0 / fs_;
                if (phase_ >= 1.0f) phase_ -= 1.0f;
                x = (phase_ < 0.04f) ? 1.0f : -0.03f;      // glottal pulse train
            } else if (cycle < 1.8f) {
                x = 0.25f * noise();                        // /s/-like turbulence
            } else {
                x = 0.002f * noise();                       // room floor
            }

            float y = f3_.step(f2_.step(f1_.step(x)));
            int   s = (int)(y * 9000.0f);
            dst[i] = (int16_t)constrain(s, -32000, 32000);
            t_++;
        }
    }

private:
    float noise() {
        rng_ = rng_ * 1664525u + 1013904223u;
        return (int32_t)(rng_ >> 8) / 8388608.0f - 1.0f;
    }

    int      fs_    = 48000;
    uint32_t t_     = 0;
    float    phase_ = 0;
    uint32_t rng_   = 22222;
    Formant  f1_, f2_, f3_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Configurations under test
// ─────────────────────────────────────────────────────────────────────────────
struct Candidate {
    const char* label;
    int32_t     sample_rate;
    int         application;   // OPUS_APPLICATION_*
    int32_t     bitrate;
    int         complexity;
    int         frame_ms;
    int         fec;           // in-band FEC (needs SILK; ignored in LOWDELAY)
};

static const Candidate CANDIDATES[] = {
    // label                      Fs      application                        bitrate  cplx  ms  fec
    { "16k voip  24k c0     ",  16000, OPUS_APPLICATION_VOIP,                 24000,   0,  20,  1 },
    { "16k voip  24k c3     ",  16000, OPUS_APPLICATION_VOIP,                 24000,   3,  20,  1 },
    { "24k voip  32k c0     ",  24000, OPUS_APPLICATION_VOIP,                 32000,   0,  20,  1 },
    { "24k voip  32k c3     ",  24000, OPUS_APPLICATION_VOIP,                 32000,   3,  20,  1 },
    { "24k voip  40k c5     ",  24000, OPUS_APPLICATION_VOIP,                 40000,   5,  20,  1 },
    { "48k voip  32k c0     ",  48000, OPUS_APPLICATION_VOIP,                 32000,   0,  20,  1 },
    { "48k voip  48k c0     ",  48000, OPUS_APPLICATION_VOIP,                 48000,   0,  20,  1 },
    { "48k voip  48k c1     ",  48000, OPUS_APPLICATION_VOIP,                 48000,   1,  20,  1 },
    { "48k voip  48k c3     ",  48000, OPUS_APPLICATION_VOIP,                 48000,   3,  20,  1 },
    { "48k voip  48k c5     ",  48000, OPUS_APPLICATION_VOIP,                 48000,   5,  20,  1 },
    { "48k voip  64k c0     ",  48000, OPUS_APPLICATION_VOIP,                 64000,   0,  20,  1 },
    { "48k voip  64k c3     ",  48000, OPUS_APPLICATION_VOIP,                 64000,   3,  20,  1 },
    { "48k audio 64k c0     ",  48000, OPUS_APPLICATION_AUDIO,                64000,   0,  20,  1 },
    { "48k audio 64k c3     ",  48000, OPUS_APPLICATION_AUDIO,                64000,   3,  20,  1 },
    { "48k audio 96k c3     ",  48000, OPUS_APPLICATION_AUDIO,                96000,   3,  20,  1 },
    { "48k lowdly 64k c0    ",  48000, OPUS_APPLICATION_RESTRICTED_LOWDELAY,  64000,   0,  20,  0 },
    { "48k lowdly 64k c3    ",  48000, OPUS_APPLICATION_RESTRICTED_LOWDELAY,  64000,   3,  20,  0 },
    { "48k lowdly 64k c5    ",  48000, OPUS_APPLICATION_RESTRICTED_LOWDELAY,  64000,   5,  20,  0 },
    { "48k lowdly 96k c3    ",  48000, OPUS_APPLICATION_RESTRICTED_LOWDELAY,  96000,   3,  20,  0 },
    { "48k lowdly 64k c3 10m",  48000, OPUS_APPLICATION_RESTRICTED_LOWDELAY,  64000,   3,  10,  0 },
    { "48k voip  48k c0 10ms",  48000, OPUS_APPLICATION_VOIP,                 48000,   0,  10,  1 },
    { "48k voip  48k c0 40ms",  48000, OPUS_APPLICATION_VOIP,                 48000,   0,  40,  1 },
};

static const int WARMUP_FRAMES = 10;
static const int TEST_FRAMES   = 120;   // ~2.4 s of audio: one full signal cycle

static int16_t       pcm_in[MAX_FRAME_SAMPLES];
static int16_t       pcm_out[MAX_FRAME_SAMPLES];
static uint8_t       packet[MAX_PACKET_BYTES];
static VoiceLikeSource source;

// ─────────────────────────────────────────────────────────────────────────────
static void runCandidate(const Candidate& c) {
    int frame_samples = (int)(c.sample_rate / 1000) * c.frame_ms;
    if (frame_samples > MAX_FRAME_SAMPLES) return;

    uint32_t heap_before = ESP.getFreeHeap();

    int err = 0;
    OpusEncoder* enc = opus_encoder_create(c.sample_rate, 1, c.application, &err);
    if (!enc || err != OPUS_OK) {
        Serial.printf("%s  encoder_create failed (%s)\n", c.label, opus_strerror(err));
        return;
    }
    OpusDecoder* dec = opus_decoder_create(c.sample_rate, 1, &err);
    if (!dec || err != OPUS_OK) {
        Serial.printf("%s  decoder_create failed (%s)\n", c.label, opus_strerror(err));
        opus_encoder_destroy(enc);
        return;
    }

    uint32_t heap_used = heap_before - ESP.getFreeHeap();

    opus_encoder_ctl(enc, OPUS_SET_BITRATE(c.bitrate));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(c.complexity));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));
    opus_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(1));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(c.fec));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(c.fec ? 10 : 0));
    opus_encoder_ctl(enc, OPUS_SET_DTX(0));
    opus_encoder_ctl(enc, OPUS_SET_LSB_DEPTH(16));

    source.begin(c.sample_rate);

    for (int i = 0; i < WARMUP_FRAMES; i++) {
        source.fill(pcm_in, frame_samples);
        int n = opus_encode(enc, pcm_in, frame_samples, packet, sizeof(packet));
        if (n > 0) opus_decode(dec, packet, n, pcm_out, frame_samples, 0);
    }

    uint64_t enc_total = 0, dec_total = 0, plc_total = 0;
    uint32_t enc_max = 0, dec_max = 0;
    uint32_t bytes_total = 0;
    int      bytes_max = 0;
    int      failures = 0;

    for (int i = 0; i < TEST_FRAMES; i++) {
        source.fill(pcm_in, frame_samples);

        uint32_t t0 = (uint32_t)esp_timer_get_time();
        int n = opus_encode(enc, pcm_in, frame_samples, packet, sizeof(packet));
        uint32_t t1 = (uint32_t)esp_timer_get_time();

        if (n <= 0) { failures++; continue; }

        int m = opus_decode(dec, packet, n, pcm_out, frame_samples, 0);
        uint32_t t2 = (uint32_t)esp_timer_get_time();

        // Concealment cost: what the speaker task pays for a lost packet.
        opus_decode(dec, NULL, 0, pcm_out, frame_samples, 0);
        uint32_t t3 = (uint32_t)esp_timer_get_time();

        if (m <= 0) failures++;

        uint32_t e = t1 - t0, d = t2 - t1;
        enc_total += e;  dec_total += d;  plc_total += (t3 - t2);
        if (e > enc_max) enc_max = e;
        if (d > dec_max) dec_max = d;
        bytes_total += n;
        if (n > bytes_max) bytes_max = n;
    }

    float enc_us  = (float)enc_total / TEST_FRAMES;
    float dec_us  = (float)dec_total / TEST_FRAMES;
    float plc_us  = (float)plc_total / TEST_FRAMES;
    float budget  = c.frame_ms * 1000.0f;
    float load    = 100.0f * (enc_us + dec_us) / budget;
    float avg_kbps = (bytes_total * 8.0f) / (TEST_FRAMES * c.frame_ms);   // bytes*8 / ms = kbit/s

    // On the wire each packet also carries the 6-byte TalkBox header plus
    // 28 bytes of UDP/IPv4, and those do not shrink with the payload.
    float wire_kbps = ((bytes_total / (float)TEST_FRAMES) + 6 + 28) * 8.0f / c.frame_ms;

    Serial.printf("%s | %6.2f %6.2f | %6.2f %6.2f | %5.2f | %5.1f%% | %5.1f %5.1f | %3d %3d | %5lu | %d\n",
                  c.label,
                  enc_us / 1000.0f, enc_max / 1000.0f,
                  dec_us / 1000.0f, dec_max / 1000.0f,
                  plc_us / 1000.0f,
                  load,
                  avg_kbps, wire_kbps,
                  (int)(bytes_total / TEST_FRAMES), bytes_max,
                  (unsigned long)heap_used,
                  failures);

    opus_encoder_destroy(enc);
    opus_decoder_destroy(dec);
}

void setup() {
    Serial.begin(115200);
    delay(1500);

    Serial.println();
    Serial.println("opus-bench — libopus on ESP32");
    Serial.printf("cpu=%lu MHz  heap=%lu  encoder_size(mono)=%d  decoder_size(mono)=%d\n",
                  (unsigned long)getCpuFrequencyMhz(),
                  (unsigned long)ESP.getFreeHeap(),
                  opus_encoder_get_size(1), opus_decoder_get_size(1));
    Serial.printf("libopus %s\n", opus_get_version_string());
    Serial.println();
    Serial.println("enc/dec/plc are milliseconds per frame; load is (enc+dec) as a share of");
    Serial.println("the frame's own duration, i.e. the fraction of ONE 240 MHz core needed to");
    Serial.println("keep up in real time.  wire kbps includes the 6-byte TB header + 28 B UDP/IP.");
    Serial.println();
    Serial.println("config                | enc avg   max | dec avg   max |  plc  |  load | kbps  wire | avg max B | heap  | err");
    Serial.println("----------------------+---------------+---------------+-------+-------+------------+-----------+-------+----");

    for (const Candidate& c : CANDIDATES) {
        runCandidate(c);
        delay(20);
    }

    Serial.println();
    Serial.println("done.");
}

void loop() {
    delay(1000);
}
