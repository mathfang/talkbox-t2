/*
 * TalkBox handset — "yellow"
 *
 * Two handsets talk to each other over the public internet by way of a small
 * UDP relay running on a VPS (see ../relay/relay.py).  Neither handset needs a
 * public IP, port forwarding, or to be on the same LAN as the other: each one
 * only ever makes *outbound* UDP connections to the relay, and the relay
 * forwards packets between whichever two handsets registered into the same
 * room.
 *
 *     handset (CA)  --outbound UDP-->  relay VPS  <--outbound UDP--  handset (IL)
 *
 * Everything (audio, potentiometer, registration) shares ONE socket bound to
 * LOCAL_PORT.  That matters: the relay can only reach us by replying to the
 * exact source ip:port our NAT allocated, so we must send and receive on the
 * same socket.  A mutex serialises access because several tasks use it.
 *
 * Debug listener for raw audio (local network only):
 *   nc -l -p 6000 | play -t raw -r 16000 -e signed -b 16 -c 1 -
 */

#include "AudioTools.h"
#include "WiFi.h"
#include "WiFiUdp.h"
#include <FastLED.h>
#include <WiFiManager.h>   // tzapu/WiFiManager, via Library Manager

// ─────────────────────────────────────────────────────────────────────────────
// Per-device configuration.  This block is the ONLY thing that differs between
// os-blue/os-blue.ino and os-yellow/os-yellow.ino.
// ─────────────────────────────────────────────────────────────────────────────
static const char* DEVICE_ID   = "yellow";        // must be unique within the room
static const char* ROOM_ID     = "talkbox";     // both handsets share this
static const char* ROOM_SECRET = "change-me";   // must match the relay's --secret

// Public IP (or DNS name) of the VPS running relay.py.
static const char*    RELAY_HOST = "164.92.82.205";
static const uint16_t RELAY_PORT = 7000;

// Local UDP port.  Any free port works; it is the source port the relay learns.
static const uint16_t LOCAL_PORT = 6000;

#define ENABLE_DIAGNOSTICS 1   // periodic link/audio stats on Serial

// ─────────────────────────────────────────────────────────────────────────────
// Hardware
// ─────────────────────────────────────────────────────────────────────────────
#define NUM_LEDS       18
#define LED_DATA_PIN   23
#define LED_CLOCK_PIN  18

#define MIC_BCK_PIN    32
#define MIC_WS_PIN     15
#define MIC_DATA_PIN   33

#define SPK_BCK_PIN    26
#define SPK_WS_PIN     25   // lrck
#define SPK_DATA_PIN   22   // sd

static const int POT_PIN       = 34;
static const int POT_PERIOD_MS = 33;    // ~30 Hz pot updates
static const int LED_PERIOD_MS = 33;    // ~30 Hz LED refresh

// "Speak up" cue: while the peer has their volume up but ours is still down,
// the LEDs pulse instead of sitting steady, so a knob left at zero is obvious
// from across the room.  The depth ramps rather than snaps, so raising our own
// knob fades the pulse out into a steady glow.
static const float   POT_QUIET_LEVEL   = 0.10f;   // 10% of pot travel
static const uint8_t PULSE_RAMP_STEP   = 8;       // depth per frame; ~1 s full fade
static const uint8_t PULSE_RATE        = 4;       // phase per frame; ~2 s period
static const int PORTAL_PERIOD_MS = 20;   // portal HTTP/DNS service interval

// ─────────────────────────────────────────────────────────────────────────────
// Audio format
// ─────────────────────────────────────────────────────────────────────────────
static const int SAMPLE_RATE     = 16000;
static const int BITS_PER_SAMPLE = 16;
static const int CHANNELS        = 1;

// One audio packet = 512 bytes = 256 samples = 16 ms of speech.
static const size_t AUDIO_BLOCK  = 512;
static const size_t BYTES_PER_MS = (SAMPLE_RATE * (BITS_PER_SAMPLE / 8) * CHANNELS) / 1000;  // 32
static const size_t AUDIO_BLOCK_MS = AUDIO_BLOCK / BYTES_PER_MS;   // 16 ms of speech per block

// ─────────────────────────────────────────────────────────────────────────────
// Jitter buffer sizing.  A cross-country path adds ~60-80 ms of latency plus
// jitter, so incoming audio cannot be written straight to I2S — it has to be
// buffered and drained at a steady rate.
// ─────────────────────────────────────────────────────────────────────────────
static const size_t JITTER_PREFILL_MS  = 100;   // wait for this much before playing
static const size_t JITTER_TARGET_MS   = 120;   // depth we trim back down to
static const size_t JITTER_MAX_MS      = 260;   // above this, we're drifting late
static const size_t JITTER_CAPACITY_MS = 600;   // ring buffer size

static const size_t JITTER_PREFILL  = JITTER_PREFILL_MS  * BYTES_PER_MS;
static const size_t JITTER_TARGET   = JITTER_TARGET_MS   * BYTES_PER_MS;
static const size_t JITTER_MAX      = JITTER_MAX_MS      * BYTES_PER_MS;
static const size_t JITTER_CAPACITY = JITTER_CAPACITY_MS * BYTES_PER_MS;

// ─────────────────────────────────────────────────────────────────────────────
// I2S DMA depth
//
// This sets the latency of the entire call, and it is the largest number in
// this file.  AudioTools multiplies buffer_size by buffer_count, divides by
// the frame size (2 bytes: 16-bit mono), and hands the result to IDF as
// dma_frame_num.  IDF then allocates dma_desc_num of those, and AudioTools
// leaves that at IDF's default of 6.  So the depth, in samples, is
//
//     buffer_size * buffer_count / 2 * 6
//
// The library defaults are 512 and 6, i.e. 9216 samples — 576 ms per
// direction.  i2s_channel_write() blocks until every byte is copied into DMA,
// so in steady state that queue sits full, and a sample handed to the speaker
// reaches the amplifier well over half a second later.
//
// That delay is also the echo path: what the mic hears is what we queued 576
// ms ago plus the couple of ms it takes to cross the box.  It is why the
// previous suppressor needed a 500 ms memory of what it had played — it was
// not measuring the room, it was measuring this buffer.  A canceller has to
// span the delay with an adaptive filter and nobody runs 9216 taps, so
// shrinking this is the prerequisite for cancelling anything at all.
//
// 128 x 2 gives 768 samples, 48 ms per direction.  Watch "under=" in the
// stats line: DMA is what absorbs task-scheduling jitter, and if the speaker
// task is ever held off for longer than this the buffer runs dry and clicks.
// Raise I2S_DMA_CHUNKS to 3 or 4 before reaching for anything else.
//
// (Not named I2S_BUFFER_SIZE / I2S_BUFFER_COUNT: AudioTools already #defines
// both of those as its library-wide defaults, and a constant of the same name
// would be substituted away before the compiler ever saw it.)
// ─────────────────────────────────────────────────────────────────────────────
static const int I2S_DMA_CHUNK_BYTES = 128;
static const int I2S_DMA_CHUNKS      = 2;

// ─────────────────────────────────────────────────────────────────────────────
// Echo delay probe
//
// The mic sits inches from the speaker, so every word the peer says reaches
// their own ear again one round trip later.  Cancelling that means
// subtracting a filtered copy of what we played from what the mic heard, and
// the filter has to span the delay between the two.  So the first thing worth
// knowing is what that delay actually is.
//
// It is measured rather than assumed.  Every block, taskSpeaker records the
// level of what it played and taskMicTx records the level of what it heard;
// correlating those two envelopes at every lag out to PROBE_MAX_LAG gives a
// peak at the lag that lines them up, and that lag is the echo path delay.
//
// Envelopes, not waveforms.  Correlating the samples themselves would resolve
// to a single sample and cost hundreds of times more; a block envelope costs
// three multiply-adds per lag per block and resolves to 16 ms, which is all
// the precision needed to choose a filter length.
//
// This measures only.  It never touches the audio.
// ─────────────────────────────────────────────────────────────────────────────
#define ENABLE_DELAY_PROBE 1

// Longest delay considered.  64 blocks is 1.02 s, comfortably past the 576 ms
// the library defaults produce, so the probe still reads correctly if the DMA
// depth above is put back to 512 x 6 for a before-and-after comparison.
static const size_t PROBE_MAX_LAG = 64;
static const size_t PROBE_RING    = PROBE_MAX_LAG + 1;

// Blocks of evidence behind each estimate.  512 blocks is 8.2 s, long enough
// that a few syllables of near-end speech cannot move the peak.
static const uint32_t PROBE_WINDOW = 512;

// Playback quieter than this teaches nothing about the echo path, so those
// blocks are skipped rather than diluting the correlation with silence.  It
// sits just above room tone; compare against "spk=" in the stats line.
static const float PROBE_FAR_ACTIVE = 30.0f;

// How well the two envelopes have to line up before the answer is worth
// printing.  The correlation at the winning lag is not a nicety here — it is
// what separates a real measurement from an argmax over noise.
//
// Simulated first: a case weak enough to lose the peak wanders the whole
// range and drops below 0.30 whenever it does, while a findable one sits at
// 0.75 and up.  Confirmed since on the handsets, where 30 s of one-way speech
// held delay=48ms across every window that cleared this bar, at r running 45
// to 86%, and reported "weak" at 27 to 36% exactly when both ends were
// talking at once.  0.40 sits in the gap.
//
// Below it the delay is withheld rather than guessed at, because a plausible
// wrong number read off a log is worse than no number.
static const float PROBE_MIN_CORR = 0.40f;

// Reported when the correlation is too weak to believe.
static const uint32_t PROBE_NO_DELAY = 0xFFFFFFFFul;

// Ceiling on the pot's volume boost.
//
// Per hop the loop multiplies by (coupling x volume), where coupling is the
// mic level over the speaker samples that provoked it.
//
// An earlier note here put that coupling at 0.84 to 1.87, inherited from the
// parked suppressor's runtime estimate.  That figure was peak mic over peak
// playback across the same one-second window, so every word the near end
// spoke landed in the numerator — it was measuring the talker, not the room.
// Read off the aligned stats instead, taking only lines where the far end is
// clearly talking and the near end is not, coupling is about 0.25.
//
// Which makes 5.0 a real ceiling rather than a nominal one.  At full pot the
// loop multiplies by 1.25 per hop and 1.56 round trip, and oscillates.  At
// the 20% position, where pot x MAX_VOLUME works out to a volume factor of
// exactly 1.0, it is 0.25 per hop and 0.06 round trip — 24 dB below unity,
// far too quiet to ring, which is why what rings there is the enclosure
// resonance alone rather than the loop as a whole.
//
// Left at 5.0 because the knob belongs to whoever is holding the handset, but
// usable travel currently ends somewhere near the middle, and a canceller is
// what buys the rest of it back.  Worth knowing that VolumeStream clips at
// +/-32767, so a boost this large hard-clips loud passages, and clipping is
// the main thing limiting how much of the echo a linear canceller can remove.
static const float MAX_VOLUME = 5.0f;

// ─────────────────────────────────────────────────────────────────────────────
// Mic filter chain
//
// Feedback does not howl across a band, it howls at the one frequency where
// the loop gain peaks, and everything here conspires to put that peak high:
// a small sealed cavity resonates in the low kHz, small speakers and MEMS
// mics both rise toward the top of their range, and the pot's boost is flat
// so it lifts the peak along with everything else.
//
// The broadband arithmetic on MAX_VOLUME above says this loop should be
// silent at the pot position where the ringing was recorded.  That it rings
// anyway puts the resonance well above the broadband average, which is
// ordinary for a box like this.  So the fix is narrowband, not broadband.
//
// The shelf came first, aimed at a guess of 3 kHz, and it took most of the
// ringing out.  Then hz= named the actual resonance and it is nowhere near
// 3 kHz: across every logged interval where playback was loud and the mic
// was hearing it back, the reading landed between 1781 and 2031 Hz — eleven
// of them, clustered on 1900.  (The scattered 187 to 750 Hz readings in the
// same log are the near end talking.  Voiced speech has a low crossing rate,
// so a low reading means a voice and a repeated one means a resonance.)
//
// Which means the shelf was only ever clipping that resonance's harmonics.
// Enough to take the edge off, not enough to stop it.  A notch aimed at 1900
// finishes the job and costs almost nothing: at Q = 4 it touches roughly 475
// Hz of the spectrum, where dragging the shelf corner down to 1.3 kHz to
// reach the same depth would have flattened every consonant above it.
//
// Both stages stay.  The shelf handles whatever else lives up top, the notch
// handles the one that matters.  If the ring moves rather than stops — kill
// one resonance and the next one down can take over — hz= will name the new
// frequency and a third stage goes in exactly the same way.
//
// Applied to what we transmit rather than what we play.  The loop passes
// through here either way, and this way our own playback stays full range.
// ─────────────────────────────────────────────────────────────────────────────
#define ENABLE_MIC_FILTER 1

// Stage 1, the blunt one: everything above the corner comes down by this much.
static const float MIC_SHELF_HZ = 3000.0f;
static const float MIC_SHELF_DB = -12.0f;

// Stage 2, the aimed one.  Q sets how wide the bucket is — bandwidth at the
// half-power points is roughly MIC_NOTCH_HZ / MIC_NOTCH_Q, so 475 Hz here.
// Widen it (lower Q) if the ring wanders, deepen it (more dB) if it holds
// still and persists.
static const float MIC_NOTCH_HZ = 1900.0f;
static const float MIC_NOTCH_DB = -18.0f;
static const float MIC_NOTCH_Q  = 4.0f;

// Below this the loudest block in an interval is too quiet for its
// zero-crossing rate to mean anything, and hz= reports nothing rather than a
// number made of room tone.
static const float MIC_HZ_FLOOR = 30.0f;

// ─────────────────────────────────────────────────────────────────────────────
// Wire protocol (shared with relay.py — keep the two in sync)
// ─────────────────────────────────────────────────────────────────────────────
static const uint8_t TB_MAGIC0  = 'T';
static const uint8_t TB_MAGIC1  = 'B';
static const uint8_t TB_VERSION = 1;

enum : uint8_t {
    TB_REGISTER     = 1,   // handset -> relay   (payload: TbRegister)
    TB_REGISTER_ACK = 2,   // relay   -> handset (payload: TbRegisterAck)
    TB_AUDIO        = 3,   // handset -> relay -> peer (payload: PCM bytes)
    TB_POT          = 4,   // handset -> relay -> peer (payload: float 0..1)
    TB_BYE          = 5,   // handset -> relay   (no payload)
    TB_ERROR        = 6,   // relay   -> handset (payload: ASCII reason)
};

#pragma pack(push, 1)
struct TbHeader {
    uint8_t  magic0;
    uint8_t  magic1;
    uint8_t  version;
    uint8_t  type;
    uint16_t seq;      // little-endian: native on ESP32, '<H' on the relay
};

struct TbRegister {
    char room[16];
    char device[16];
    char secret[32];
};

struct TbRegisterAck {
    uint8_t peer_present;
    char    peer_device[16];
};
#pragma pack(pop)

static const size_t MAX_PACKET = sizeof(TbHeader) + AUDIO_BLOCK;

// How long an ACK's "peer is present" answer is trusted before we go quiet
// again.  Comfortably longer than the heartbeat so a single lost ACK is
// invisible.
static const uint32_t PEER_TTL_MS        = 5000;
static const uint32_t REGISTER_PERIOD_MS = 1000;   // also keeps the NAT hole open
static const uint32_t WIFI_RETRY_MS      = 15000;  // how often to force a fresh WiFi attempt
static const uint32_t WIFI_GRACE_MS      = 10000;  // an attempt this young still reads as "connecting"

// ─────────────────────────────────────────────────────────────────────────────
// Link state
// ─────────────────────────────────────────────────────────────────────────────
enum LinkState : uint8_t {
    LINK_SETUP,         // unconfigured: setup hotspot open, waiting for a browser
    LINK_CONNECTING,    // configured, association attempt still young
    LINK_WIFI_DOWN,     // association gone and not coming back on its own
    LINK_NO_RELAY,      // WiFi up, relay unresolved / no ACK yet
    LINK_WAITING_PEER,  // registered with the relay, peer not online
    LINK_UP,            // both handsets registered — audio flows
};

static volatile LinkState link_state = LINK_SETUP;
static volatile uint32_t  peer_seen_until_ms = 0;   // set by rx task, read by senders

static inline bool peerPresent() {
    return (int32_t)(peer_seen_until_ms - millis()) > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Globals
// ─────────────────────────────────────────────────────────────────────────────
CRGB leds[NUM_LEDS];

I2SStream    in;
I2SStream    out;
VolumeStream volume(out);   // scales, then forwards to `out` for us

WiFiUDP          udp;
static IPAddress relay_ip;
static bool      relay_ip_valid = false;
static SemaphoreHandle_t sock_mutex;

static uint16_t tx_audio_seq = 0;
static uint16_t tx_pot_seq   = 0;

static volatile float remote_pot = 0.0f;   // 0..1, drives our LEDs
static volatile float local_pot  = 0.0f;   // 0..1, our own knob, published by taskPotTx

// Diagnostics counters, reset each time the stats task prints them.  Several
// tasks increment these without locking: a torn count would only ever skew a
// log line, never the audio path, and that isn't worth a mutex per packet.
static volatile uint32_t stat_tx_bytes   = 0;
static volatile uint32_t stat_rx_bytes   = 0;
static volatile uint32_t stat_lost_audio = 0;
static volatile uint32_t stat_late_audio = 0;
static volatile uint32_t stat_underruns  = 0;
static volatile uint32_t stat_overruns   = 0;

// Peak levels since the last stats line.  Peaks rather than instantaneous
// samples, because the interesting question is whether playback ever got loud
// enough to matter and whether the mic ever heard it — a snapshot taken every
// 2 s would miss both.
static volatile float stat_echo_spk = 0.0f;   // loudest block we played
static volatile float stat_echo_mic = 0.0f;   // loudest block the mic heard
static volatile float stat_mic_hz   = 0.0f;   // ... and its dominant frequency

// Latest output of the delay probe.  Written by taskMicTx once per window,
// read by taskStats; probe_blocks stays 0 until the first window completes,
// which is what the stats line reads as "measuring".
static volatile uint32_t probe_delay_ms = 0;
static volatile float    probe_corr     = 0.0f;
static volatile uint32_t probe_blocks   = 0;

// Times the jitter buffer skipped forward to claw back latency.  Each one
// splices two unrelated points of the waveform together, which is heard as a
// click, so a steady trickle of these is an audio fault in its own right and
// has nothing to do with echo.
static volatile uint32_t stat_trims = 0;

// Audio blocks that never reached the wire.  taskLink re-registers once a
// second and takes the same socket mutex, so a periodic artifact at roughly
// that rate would show up here.
static volatile uint32_t stat_tx_fail = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Jitter buffer — byte ring written by the network task, drained by the
// speaker task at the I2S clock rate.
// ─────────────────────────────────────────────────────────────────────────────
class JitterBuffer {
public:
    void begin(size_t capacity) {
        buf_  = (uint8_t*)malloc(capacity);
        cap_  = buf_ ? capacity : 0;
        head_ = tail_ = count_ = 0;
        lock_ = xSemaphoreCreateMutex();
        if (!buf_) Serial.println("FATAL: jitter buffer allocation failed");
    }

    // Appends `len` bytes, discarding the oldest audio if the ring is full.
    void push(const uint8_t* data, size_t len) {
        if (!buf_ || len == 0 || len > cap_) return;
        xSemaphoreTake(lock_, portMAX_DELAY);

        if (count_ + len > cap_) {
            dropOldest(count_ + len - cap_);
            stat_overruns++;
        }
        size_t first = min(len, cap_ - head_);
        memcpy(buf_ + head_, data, first);
        if (len > first) memcpy(buf_, data + first, len - first);
        head_ = (head_ + len) % cap_;
        count_ += len;

        // The two handsets' 16 kHz clocks are never exactly equal, so over a
        // long call the buffer slowly grows.  Skip forward instead of letting
        // latency creep up for the rest of the conversation.
        if (count_ > JITTER_MAX) {
            dropOldest(count_ - JITTER_TARGET);
            stat_trims++;
        }

        xSemaphoreGive(lock_);
    }

    // Removes exactly `len` bytes, or nothing at all if the buffer is short.
    bool pop(uint8_t* dst, size_t len) {
        if (!buf_) return false;
        xSemaphoreTake(lock_, portMAX_DELAY);
        bool ok = count_ >= len;
        if (ok) {
            size_t first = min(len, cap_ - tail_);
            memcpy(dst, buf_ + tail_, first);
            if (len > first) memcpy(dst + first, buf_, len - first);
            tail_ = (tail_ + len) % cap_;
            count_ -= len;
        }
        xSemaphoreGive(lock_);
        return ok;
    }

    size_t available() {
        if (!buf_) return 0;
        xSemaphoreTake(lock_, portMAX_DELAY);
        size_t n = count_;
        xSemaphoreGive(lock_);
        return n;
    }

    void reset() {
        if (!buf_) return;
        xSemaphoreTake(lock_, portMAX_DELAY);
        head_ = tail_ = count_ = 0;
        xSemaphoreGive(lock_);
    }

private:
    void dropOldest(size_t len) {   // caller holds lock_
        if (len > count_) len = count_;
        tail_ = (tail_ + len) % cap_;
        count_ -= len;
    }

    uint8_t*          buf_   = nullptr;
    size_t            cap_   = 0;
    size_t            head_  = 0;
    size_t            tail_  = 0;
    size_t            count_ = 0;
    SemaphoreHandle_t lock_  = nullptr;
};

static JitterBuffer jitter;

// ─────────────────────────────────────────────────────────────────────────────
// Echo delay probe.  taskSpeaker reports the level of every block it hands the
// amplifier; taskMicTx reports the level of every block the mic returns, and
// correlates the two to find the lag between them.
// ─────────────────────────────────────────────────────────────────────────────
#if ENABLE_DELAY_PROBE

// Mean |sample| over a PCM block: the cheapest level estimate that still
// tracks speech, and it needs no sqrt.
static float blockLevel(const uint8_t* data, size_t len) {
    const int16_t* s = (const int16_t*)data;
    const size_t   n = len / 2;
    if (n == 0) return 0.0f;

    uint32_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = s[i];
        sum += (v < 0) ? -v : v;
    }
    return (float)sum / (float)n;
}

// Levels of what we recently played, written by taskSpeaker and read by
// taskMicTx.  probe_ref_pos points at the slot the NEXT block will occupy, so
// the newest entry is the one before it.  32-bit aligned floats, so a race
// costs at most one stale block — cheaper than a mutex on the audio path, and
// one block of noise is invisible under a 512-block average.
static volatile float  probe_ref[PROBE_RING];
static volatile size_t probe_ref_pos = 0;

// Sums accumulating toward the next estimate.  Owned by taskMicTx alone.
// x is the mic envelope, y the playback envelope at each lag; correlating the
// two needs Sxy, Sx, Sxx, and per-lag Sy and Syy.
static float    probe_sxy[PROBE_RING];
static float    probe_sy [PROBE_RING];
static float    probe_syy[PROBE_RING];
static float    probe_sx  = 0.0f;
static float    probe_sxx = 0.0f;
static uint32_t probe_n   = 0;

// Called once per played block, including the silent ones — the window only
// slides forward if it keeps being fed.
static void probeNotePlayed(const uint8_t* data, size_t len) {
    const float level = blockLevel(data, len);
    if (level > stat_echo_spk) stat_echo_spk = level;

    const size_t pos = probe_ref_pos;
    probe_ref[pos]   = level;
    probe_ref_pos    = (pos + 1 == PROBE_RING) ? 0 : pos + 1;
}

// Called once per captured block.  Reads the mic, never writes it.
static void probeNoteCaptured(const uint8_t* data, size_t len) {
    const float level = blockLevel(data, len);

    // One read of the writer's index, so the scan below sees a view that is at
    // worst one block stale rather than one that slides underneath it.
    const size_t pos = probe_ref_pos;

    // Nothing played inside the whole lag window, so nothing in this block can
    // be echo of ours at any lag.  Skipping keeps silence out of the sums
    // instead of burying the correlation under it.
    float far_peak = 0.0f;
    for (size_t i = 0; i < PROBE_RING; i++) {
        const float v = probe_ref[i];
        if (v > far_peak) far_peak = v;
    }
    if (far_peak <= PROBE_FAR_ACTIVE) return;

    // Scaled to roughly 0..1 so the sums below stay in the range where a
    // float still carries every bit of them.
    static const float SCALE = 1.0f / 32768.0f;

    const float x = level * SCALE;
    probe_sx  += x;
    probe_sxx += x * x;

    size_t idx = (pos == 0) ? PROBE_RING - 1 : pos - 1;   // newest block played
    for (size_t lag = 0; lag < PROBE_RING; lag++) {
        const float y = probe_ref[idx] * SCALE;
        probe_sxy[lag] += x * y;
        probe_sy [lag] += y;
        probe_syy[lag] += y * y;
        idx = (idx == 0) ? PROBE_RING - 1 : idx - 1;
    }

    if (++probe_n < PROBE_WINDOW) return;

    // Pearson correlation per lag, which is what makes the peak comparable
    // between lags and between windows: a raw dot product rewards whichever
    // lag happens to line up with the loudest playback, and a loud call would
    // read as a confident one no matter where the peak sat.
    const float n    = (float)probe_n;
    const float varx = n * probe_sxx - probe_sx * probe_sx;

    float  best_corr = 0.0f;
    size_t best_lag  = 0;
    if (varx > 0.0f) {
        for (size_t lag = 0; lag < PROBE_RING; lag++) {
            const float vary = n * probe_syy[lag] - probe_sy[lag] * probe_sy[lag];
            if (vary <= 0.0f) continue;
            const float cov  = n * probe_sxy[lag] - probe_sx * probe_sy[lag];
            const float corr = cov / sqrtf(varx * vary);
            if (corr > best_corr) {
                best_corr = corr;
                best_lag  = lag;
            }
        }
    }

    probe_delay_ms = (best_corr >= PROBE_MIN_CORR)
                   ? (uint32_t)(best_lag * AUDIO_BLOCK_MS)
                   : PROBE_NO_DELAY;
    probe_corr     = best_corr;
    probe_blocks   = probe_n;

    probe_sx  = 0.0f;
    probe_sxx = 0.0f;
    probe_n   = 0;
    for (size_t lag = 0; lag < PROBE_RING; lag++) {
        probe_sxy[lag] = 0.0f;
        probe_sy [lag] = 0.0f;
        probe_syy[lag] = 0.0f;
    }
}

#else   // ENABLE_DELAY_PROBE

static inline void probeNotePlayed(const uint8_t*, size_t) {}
static inline void probeNoteCaptured(const uint8_t*, size_t) {}

#endif

// ─────────────────────────────────────────────────────────────────────────────
// Mic front end.  micAnalyse() measures the raw mic and never changes it;
// micFilter() changes it and never measures it.
// ─────────────────────────────────────────────────────────────────────────────
// Level and dominant frequency of the loudest block in the interval.
//
// Zero-crossing rate is a crude pitch estimate on speech, where it reads more
// like brightness than pitch.  On a ring it is exactly right: a loop
// oscillating at its resonance is very nearly a sine wave, and a sine wave
// crosses its own centre line twice per cycle.  Since it is only ever
// reported for the loudest block of an interval, and a ring is by some margin
// the loudest thing in the box, the number that comes out during ringing is
// the ringing frequency.
//
// Crossings are counted against the block's own mean rather than against
// zero.  A MEMS mic can sit on a DC offset larger than the signal riding on
// it, and crossings of zero would then count nothing at all.
static void micAnalyse(const uint8_t* data, size_t len) {
    const int16_t* s = (const int16_t*)data;
    const size_t   n = len / 2;
    if (n < 2) return;

    int32_t sum     = 0;   // signed, for the DC the mic is sitting on
    int32_t sum_abs = 0;   // unsigned, for the level
    for (size_t i = 0; i < n; i++) {
        const int32_t v = s[i];
        sum     += v;
        sum_abs += (v < 0) ? -v : v;
    }

    const float level = (float)sum_abs / (float)n;
    if (level <= stat_echo_mic) return;   // a louder block already spoke for this interval
    stat_echo_mic = level;

    // Peaks only ever climb within an interval, so a new peak below the floor
    // means nothing louder has happened yet and there is no frequency to
    // report.  Clearing rather than leaving the old one keeps hz= honest.
    if (level < MIC_HZ_FLOOR) {
        stat_mic_hz = 0.0f;
        return;
    }

    const int32_t dc = sum / (int32_t)n;
    uint32_t crossings = 0;
    bool     prev      = (s[0] > dc);
    for (size_t i = 1; i < n; i++) {
        const bool cur = (s[i] > dc);
        if (cur != prev) crossings++;
        prev = cur;
    }

    stat_mic_hz = (float)crossings * (float)SAMPLE_RATE / (2.0f * (float)n);
}

#if ENABLE_MIC_FILTER

// One second-order section, direct form I, with the two designers we need.
// Coefficients follow the RBJ audio EQ cookbook.
struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;

    void normalise(float nb0, float nb1, float nb2, float a0, float na1, float na2) {
        b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0;
        a1 = na1 / a0; a2 = na2 / a0;
    }

    // Everything above f0 down by db, everything below it untouched.
    void highShelf(float f0, float db, float fs) {
        const float A     = powf(10.0f, db / 40.0f);
        const float w0    = 2.0f * PI * f0 / fs;
        const float cosw  = cosf(w0);
        const float alpha = sinf(w0) * 0.70710678f;   // shelf slope S = 1
        const float tsa   = 2.0f * sqrtf(A) * alpha;
        const float ap1   = A + 1.0f;
        const float am1   = A - 1.0f;
        normalise(        A * (ap1 + am1 * cosw + tsa),
                  -2.0f * A * (am1 + ap1 * cosw),
                           A * (ap1 + am1 * cosw - tsa),
                                ap1 - am1 * cosw + tsa,
                   2.0f *      (am1 - ap1 * cosw),
                                ap1 - am1 * cosw - tsa);
    }

    // A bucket of depth db centred on f0, width set by q.
    void peaking(float f0, float db, float q, float fs) {
        const float A     = powf(10.0f, db / 40.0f);
        const float w0    = 2.0f * PI * f0 / fs;
        const float cosw  = cosf(w0);
        const float alpha = sinf(w0) / (2.0f * q);
        normalise(1.0f + alpha * A,
                  -2.0f * cosw,
                  1.0f - alpha * A,
                  1.0f + alpha / A,
                  -2.0f * cosw,
                  1.0f - alpha / A);
    }

    inline float step(float x) {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }
};

static Biquad mic_stage[2];

static void micFilterBegin() {
    mic_stage[0].highShelf(MIC_SHELF_HZ, MIC_SHELF_DB, (float)SAMPLE_RATE);
    mic_stage[1].peaking(MIC_NOTCH_HZ, MIC_NOTCH_DB, MIC_NOTCH_Q, (float)SAMPLE_RATE);
    Serial.printf("Mic filter: %.0f dB above %.0f Hz, %.0f dB notch at %.0f Hz (Q %.1f)\n",
                  MIC_SHELF_DB, MIC_SHELF_HZ, MIC_NOTCH_DB, MIC_NOTCH_HZ, MIC_NOTCH_Q);
}

static void micFilter(uint8_t* data, size_t len) {
    int16_t*     s = (int16_t*)data;
    const size_t n = len / 2;
    const size_t stages = sizeof(mic_stage) / sizeof(mic_stage[0]);

    for (size_t i = 0; i < n; i++) {
        float v = (float)s[i];
        for (size_t k = 0; k < stages; k++) v = mic_stage[k].step(v);

        // Each stage keeps its own unclamped state.  Feeding a clipped sample
        // back into a recursive filter is how one loud moment turns into a
        // permanent rattle; the clamp belongs here, once, on the way out.
        s[i] = (int16_t)((v > 32767.0f) ? 32767.0f : ((v < -32768.0f) ? -32768.0f : v));
    }
}

#else   // ENABLE_MIC_FILTER

static inline void micFilterBegin() {}
static inline void micFilter(uint8_t*, size_t) {}

#endif

// ─────────────────────────────────────────────────────────────────────────────
// Socket helpers.  Every touch of `udp` goes through these so the mutex can't
// be forgotten.
// ─────────────────────────────────────────────────────────────────────────────
static bool sendToRelay(uint8_t type, uint16_t seq, const void* payload, size_t len) {
    if (!relay_ip_valid || WiFi.status() != WL_CONNECTED) return false;
    if (len > AUDIO_BLOCK) return false;

    TbHeader header = { TB_MAGIC0, TB_MAGIC1, TB_VERSION, type, seq };

    if (xSemaphoreTake(sock_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    bool ok = udp.beginPacket(relay_ip, RELAY_PORT) == 1;
    if (ok) {
        udp.write((const uint8_t*)&header, sizeof(header));
        if (len) udp.write((const uint8_t*)payload, len);
        ok = udp.endPacket() == 1;
    }
    xSemaphoreGive(sock_mutex);

    if (ok) stat_tx_bytes += sizeof(header) + len;
    return ok;
}

// Returns the number of bytes read, or 0 if no packet was waiting.
static int receiveFromRelay(uint8_t* dst, size_t cap) {
    if (xSemaphoreTake(sock_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    int n = 0;
    if (udp.parsePacket() > 0) n = udp.read(dst, cap);
    xSemaphoreGive(sock_mutex);
    return n > 0 ? n : 0;
}

static void rebindSocket() {
    if (xSemaphoreTake(sock_mutex, portMAX_DELAY) != pdTRUE) return;
    udp.stop();
    udp.begin(LOCAL_PORT);
    xSemaphoreGive(sock_mutex);
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────────
// Normalized pot position, 0 = fully down, 1 = fully up.  The wiper reads high
// when the knob is down, so the raw value is inverted here rather than at each
// call site.
static float readPot() {
    return constrain(1.0f - (analogRead(POT_PIN) / 4095.0f), 0.0f, 1.0f);
}

static WiFiManager wm;

// SSID of the setup hotspot, e.g. "talkbox-blue".
static void setupApName(char* dst, size_t cap) {
    snprintf(dst, cap, "%s-%s", ROOM_ID, DEVICE_ID);
}

// Connects using the last-saved network, falling back to a captive-portal
// hotspot ("<ROOM_ID>-<DEVICE_ID>") whenever there's nothing saved yet or
// the saved credentials no longer work. Once associated, taskPortal keeps that
// same hotspot up so the network can be changed without a re-flash. Holding the
// pot fully down through power-on wipes the saved network outright, which is
// the way back in if the portal itself is ever unreachable.
static void connectToWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(WIFI_PS_NONE);   // modem sleep adds tens of ms of jitter

    wm.setConfigPortalTimeout(180);   // give up and reboot rather than hang forever
    wm.setAPCallback([](WiFiManager*) {
        if (WiFi.status() != WL_CONNECTED) link_state = LINK_SETUP;
    });

    if (readPot() < 0.02f) {
        Serial.println("Pot held down at boot — clearing saved WiFi");
        wm.resetSettings();
    }

    char ap_name[40];
    setupApName(ap_name, sizeof(ap_name));

    // A box with a saved network is connecting, not waiting to be set up, so it
    // never shows the slow "needs setup" pace unless that network fails and
    // autoConnect falls back to the portal.
    if (wm.getWiFiIsSaved()) link_state = LINK_CONNECTING;

    Serial.printf("Connecting to WiFi (portal \"%s\" if nothing saved)\n", ap_name);
    if (!wm.autoConnect(ap_name)) {
        Serial.println("WiFi setup timed out — restarting");
        ESP.restart();
    }

    WiFi.setAutoReconnect(true);
    link_state = LINK_NO_RELAY;   // taskLink refines this once it is running
    Serial.printf("WiFi connected, ip=%s rssi=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());

    // From here on taskPortal owns the portal, and it must never block the
    // audio tasks waiting for a browser that may never arrive.
    wm.setConfigPortalBlocking(false);
    wm.setConfigPortalTimeout(0);      // lifetime is driven by link state instead
}

// Resolves RELAY_HOST; accepts either a literal IP or a DNS name.
static bool resolveRelay() {
    if (WiFi.status() != WL_CONNECTED) return false;

    IPAddress resolved;
    if (resolved.fromString(RELAY_HOST)) {
        relay_ip       = resolved;
        relay_ip_valid = true;
        return true;
    }
    if (WiFi.hostByName(RELAY_HOST, resolved) == 1) {
        relay_ip       = resolved;
        relay_ip_valid = true;
        Serial.printf("Relay %s resolved to %s\n", RELAY_HOST, relay_ip.toString().c_str());
        return true;
    }
    Serial.printf("Could not resolve relay host \"%s\"\n", RELAY_HOST);
    relay_ip_valid = false;
    return false;
}

static void setupLeds() {
    FastLED.setMaxPowerInVoltsAndMilliamps(5, 400);
    FastLED.addLeds<APA102, LED_DATA_PIN, LED_CLOCK_PIN, BGR, DATA_RATE_MHZ(1)>(leds, NUM_LEDS);
    FastLED.setBrightness(80);
    FastLED.setDither(1);
}

static void setupMic() {
    auto cfg_in = in.defaultConfig(RX_MODE);
    cfg_in.sample_rate     = SAMPLE_RATE;
    cfg_in.bits_per_sample = BITS_PER_SAMPLE;
    cfg_in.channels        = CHANNELS;
    cfg_in.pin_bck         = MIC_BCK_PIN;
    cfg_in.pin_ws          = MIC_WS_PIN;
    cfg_in.pin_data        = MIC_DATA_PIN;
    cfg_in.port_no         = 0;
    cfg_in.buffer_size     = I2S_DMA_CHUNK_BYTES;
    cfg_in.buffer_count    = I2S_DMA_CHUNKS;
    in.begin(cfg_in);
    micFilterBegin();
    Serial.println("Mic is running");
}

static void setupSpeaker() {
    auto cfg_out = out.defaultConfig(TX_MODE);
    cfg_out.sample_rate     = SAMPLE_RATE;
    cfg_out.bits_per_sample = BITS_PER_SAMPLE;
    cfg_out.channels        = CHANNELS;
    cfg_out.pin_bck         = SPK_BCK_PIN;
    cfg_out.pin_ws          = SPK_WS_PIN;
    cfg_out.pin_data        = SPK_DATA_PIN;
    cfg_out.port_no         = 1;
    cfg_out.buffer_size     = I2S_DMA_CHUNK_BYTES;
    cfg_out.buffer_count    = I2S_DMA_CHUNKS;

    auto vcfg_out = volume.defaultConfig();
    vcfg_out.copyFrom(cfg_out);
    vcfg_out.allow_boost = true;

    out.begin(cfg_out);
    volume.begin(vcfg_out);
    volume.setVolume(1.0);

    Serial.println("Speaker is running");
}

// ─────────────────────────────────────────────────────────────────────────────
// Tasks — core 1: audio path
// ─────────────────────────────────────────────────────────────────────────────

// Mic -> relay -> peer.  The mic is drained even when the link is down so the
// I2S RX DMA never overflows; those samples are simply discarded.
static void taskMicTx(void*) {
    static uint8_t tx_buffer[AUDIO_BLOCK];
    for (;;) {
        int n = in.readBytes(tx_buffer, AUDIO_BLOCK);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        // Order matters.  Both diagnostics read the raw mic so that what they
        // report describes the box rather than our own filtering, and the
        // filter goes last so only the wire sees it.  A canceller belongs
        // between the probe and the filter, where the echo path it has to
        // model is still the untouched one.
        //
        // All of this runs even with the link down.  Nothing is playing then,
        // so the probe skips those blocks itself rather than being told to.
        micAnalyse(tx_buffer, n);
        probeNoteCaptured(tx_buffer, n);
        micFilter(tx_buffer, n);
        if (link_state == LINK_UP) {
            if (!sendToRelay(TB_AUDIO, tx_audio_seq++, tx_buffer, n)) stat_tx_fail++;
        }
    }
}

// Single reader for the socket: validates, classifies, and hands off.
static void taskNetRx(void*) {
    static uint8_t rx_buffer[MAX_PACKET];
    uint16_t last_audio_seq = 0;
    uint16_t last_pot_seq   = 0;
    bool     audio_seq_init = false;
    bool     pot_seq_init   = false;

    for (;;) {
        int n = receiveFromRelay(rx_buffer, sizeof(rx_buffer));
        if (n < (int)sizeof(TbHeader)) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        TbHeader header;
        memcpy(&header, rx_buffer, sizeof(header));
        if (header.magic0 != TB_MAGIC0 || header.magic1 != TB_MAGIC1 ||
            header.version != TB_VERSION) {
            continue;   // stray packet, not ours
        }

        const uint8_t* payload     = rx_buffer + sizeof(TbHeader);
        const size_t   payload_len = n - sizeof(TbHeader);
        stat_rx_bytes += n;

        switch (header.type) {
        case TB_AUDIO: {
            if (payload_len == 0) break;

            // Signed difference handles the 16-bit wraparound correctly.
            int16_t gap = audio_seq_init ? (int16_t)(header.seq - last_audio_seq) : 1;
            if (gap <= 0) {              // duplicate, or a reordered straggler
                stat_late_audio++;
                break;
            }
            if (gap > 1) stat_lost_audio += (gap - 1);
            last_audio_seq = header.seq;
            audio_seq_init = true;

            jitter.push(payload, payload_len);
            break;
        }

        case TB_POT: {
            if (payload_len < sizeof(float)) break;
            int16_t gap = pot_seq_init ? (int16_t)(header.seq - last_pot_seq) : 1;
            if (gap <= 0) break;         // stale reading, a newer one already won
            last_pot_seq = header.seq;
            pot_seq_init = true;

            float value;
            memcpy(&value, payload, sizeof(float));
            if (isnan(value)) break;
            remote_pot = constrain(value, 0.0f, 1.0f);
            break;
        }

        case TB_REGISTER_ACK: {
            if (payload_len < sizeof(TbRegisterAck)) break;
            TbRegisterAck ack;
            memcpy(&ack, payload, sizeof(ack));

            if (ack.peer_present) {
                peer_seen_until_ms = millis() + PEER_TTL_MS;
                if (link_state != LINK_UP) {
                    ack.peer_device[sizeof(ack.peer_device) - 1] = '\0';
                    Serial.printf("Peer \"%s\" online — call is up\n", ack.peer_device);
                    jitter.reset();          // start the new call cleanly
                    audio_seq_init = false;
                    pot_seq_init   = false;
                    link_state     = LINK_UP;
                }
            } else if (link_state != LINK_WAITING_PEER) {
                Serial.println("Registered with relay — waiting for peer");
                link_state = LINK_WAITING_PEER;
            }
            break;
        }

        case TB_ERROR: {
            char reason[64] = {0};
            size_t len = min(payload_len, sizeof(reason) - 1);
            memcpy(reason, payload, len);
            Serial.printf("Relay error: %s\n", reason);
            link_state = LINK_NO_RELAY;      // re-register on the next heartbeat
            break;
        }

        default:
            break;
        }
    }
}

// Jitter buffer -> speaker.  The blocking I2S write paces this loop: it drains
// exactly one sample period per sample, so no timer is needed.
static void taskSpeaker(void*) {
    static uint8_t play_buffer[AUDIO_BLOCK];

    // Deliberately NOT const: VolumeStream::write() takes a const pointer but
    // casts it away and scales the samples in place, so a const array would be
    // placed in flash and the store would panic with a LoadStoreError.  Scaling
    // zeros still gives zeros, so this stays silent no matter the volume.
    static uint8_t silence[AUDIO_BLOCK] = {0};

    bool primed = false;

    for (;;) {
        if (!primed) {
            // Hold playback until enough audio has piled up to ride out jitter.
            if (jitter.available() >= JITTER_PREFILL) {
                primed = true;
            } else {
                volume.write(silence, AUDIO_BLOCK);
                probeNotePlayed(silence, AUDIO_BLOCK);
                continue;
            }
        }

        if (jitter.pop(play_buffer, AUDIO_BLOCK)) {
            volume.write(play_buffer, AUDIO_BLOCK);
            // Measured after the write because VolumeStream scales in place:
            // this is what actually reached the amplifier, pot position and
            // all, which is exactly what the mic will hear.  Keeping the tap
            // on this side of the volume means the echo path the probe sees —
            // and the canceller after it — does not move when the knob does.
            probeNotePlayed(play_buffer, AUDIO_BLOCK);
        } else {
            // Ran dry: emit silence and re-prime rather than stuttering on
            // whatever trickles in next.
            stat_underruns++;
            primed = false;
            volume.write(silence, AUDIO_BLOCK);
            probeNotePlayed(silence, AUDIO_BLOCK);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Tasks — core 0: control path
// ─────────────────────────────────────────────────────────────────────────────

// Local pot sets our own speaker volume and is mirrored to the peer's LEDs.
static void taskPotTx(void*) {
    for (;;) {
        float pot_norm = readPot();
        local_pot = pot_norm;

        volume.setVolume(pot_norm * MAX_VOLUME);

        if (link_state == LINK_UP) {
            sendToRelay(TB_POT, tx_pot_seq++, &pot_norm, sizeof(pot_norm));
        }
        vTaskDelay(pdMS_TO_TICKS(POT_PERIOD_MS));
    }
}

// LEDs show the peer's pot while the call is up, and the link state otherwise,
// so a failure 2000 miles away is still visible from the handset itself.
static void taskLeds(void*) {
    uint8_t breathe     = 0;
    uint8_t pulse_depth = 0;   // 0 = steady, 255 = full swing down to black

    for (;;) {
        breathe += PULSE_RATE;   // free-running, so state changes never jump phase

        if (link_state == LINK_UP) {
            // Pulse only while they are audible to us and we are not to them.
            bool want_pulse = (remote_pot > POT_QUIET_LEVEL) && (local_pot < POT_QUIET_LEVEL);
            pulse_depth = want_pulse ? qadd8(pulse_depth, PULSE_RAMP_STEP)
                                     : qsub8(pulse_depth, PULSE_RAMP_STEP);

            // Remote pot still sets the ceiling; the pulse swings below it.
            uint8_t peak   = (uint8_t)(remote_pot * 255);
            uint8_t factor = 255 - scale8(pulse_depth, 255 - sin8(breathe));
            fill_solid(leds, NUM_LEDS, CHSV(24, 200, scale8(peak, factor)));
        } else {
            // One dim pulse, recolored per state.  White lights all three
            // channels, so it reads far brighter than a saturated hue at the
            // same value and gets its own cap to keep the states one family.
            uint8_t hue = 160, sat = 200, cap = 60;   // blue: needs setup
            uint8_t wave = sin8(breathe);
            switch (link_state) {
            case LINK_WIFI_DOWN:     hue = 0;            break;   // red:   no WiFi
            case LINK_CONNECTING:    sat = 0;  cap = 40; break;   // white: joining the network
            case LINK_NO_RELAY:      hue = 96;           break;   // green: waiting on the relay
            case LINK_WAITING_PEER:  wave = 255;         break;   // blue, steady: peer offline
            default:                                     break;
            }
            fill_solid(leds, NUM_LEDS, CHSV(hue, sat, scale8(wave, cap)));
        }
        FastLED.show();
        vTaskDelay(pdMS_TO_TICKS(LED_PERIOD_MS));
    }
}

// Keeps the setup portal reachable whenever a call is not up, so switching the
// box to a new network never needs a re-flash.
//
// The portal is torn down for the duration of a call: the AP and the station
// share one radio, and beacons plus probe responses inject exactly the kind of
// jitter that WIFI_PS_NONE above exists to avoid.
//
// It is also only ever started while the station is associated. WiFiManager
// disables STA when bringing the AP up on an unassociated radio (a hanging
// connect starves the softAP), which would fight taskLink's reconnect and leave
// the box stranded after something as ordinary as a router reboot.
static void taskPortal(void*) {
    for (;;) {
        bool want_portal = (WiFi.status() == WL_CONNECTED) && (link_state != LINK_UP);

        if (want_portal && !wm.getConfigPortalActive()) {
            char ap_name[40];
            setupApName(ap_name, sizeof(ap_name));
            Serial.printf("Setup portal open: \"%s\"\n", ap_name);
            wm.startConfigPortal(ap_name);
        } else if (!want_portal && wm.getConfigPortalActive()) {
            Serial.println("Setup portal closed");
            wm.stopConfigPortal();
        }

        if (wm.getConfigPortalActive()) wm.process();
        vTaskDelay(pdMS_TO_TICKS(PORTAL_PERIOD_MS));
    }
}

// Keeps WiFi up, keeps the relay registration (and therefore the NAT mapping)
// alive, and re-resolves the relay whenever things go wrong.
static void taskLink(void*) {
    bool     was_connected  = false;
    uint16_t register_seq   = 0;
    uint32_t last_wifi_kick = 0;
    uint32_t down_since     = 0;   // 0 = currently associated

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            if (was_connected) {
                Serial.println("WiFi lost — reconnecting");
                was_connected = false;
            }
            if (!down_since) down_since = millis();
            link_state     = (millis() - down_since < WIFI_GRACE_MS) ? LINK_CONNECTING
                                                                    : LINK_WIFI_DOWN;
            relay_ip_valid = false;

            // Let the driver's own auto-reconnect work; only force a fresh
            // attempt occasionally, since association itself takes seconds and
            // restarting it too eagerly would never let it finish.
            if (millis() - last_wifi_kick > WIFI_RETRY_MS) {
                last_wifi_kick = millis();
                Serial.println("Retrying WiFi");
                WiFi.reconnect();   // reuses creds WiFiManager already saved to NVS
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        down_since = 0;

        if (!was_connected) {
            Serial.printf("WiFi up, ip=%s\n", WiFi.localIP().toString().c_str());
            was_connected = true;
            rebindSocket();          // fresh socket for the fresh NAT mapping
            link_state = LINK_NO_RELAY;
        }

        if (!relay_ip_valid && !resolveRelay()) {
            link_state = LINK_NO_RELAY;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // Peer presence is only known from ACKs; if they stop arriving, stop
        // sending audio instead of blasting the relay into a void.
        if (link_state == LINK_UP && !peerPresent()) {
            Serial.println("Peer went quiet — call down");
            link_state = LINK_WAITING_PEER;
            jitter.reset();
        }

        TbRegister reg = {0};
        strncpy(reg.room,   ROOM_ID,     sizeof(reg.room)   - 1);
        strncpy(reg.device, DEVICE_ID,   sizeof(reg.device) - 1);
        strncpy(reg.secret, ROOM_SECRET, sizeof(reg.secret) - 1);
        sendToRelay(TB_REGISTER, register_seq++, &reg, sizeof(reg));

        vTaskDelay(pdMS_TO_TICKS(REGISTER_PERIOD_MS));
    }
}

#if ENABLE_DIAGNOSTICS
static const char* linkStateName(LinkState state) {
    switch (state) {
    case LINK_SETUP:        return "setup";
    case LINK_CONNECTING:   return "connecting";
    case LINK_WIFI_DOWN:    return "wifi-down";
    case LINK_NO_RELAY:     return "no-relay";
    case LINK_WAITING_PEER: return "waiting-peer";
    case LINK_UP:           return "up";
    }
    return "?";
}

static void taskStats(void*) {
    const uint32_t period_ms = 2000;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(period_ms));

        uint32_t tx    = stat_tx_bytes;   stat_tx_bytes   = 0;
        uint32_t rx    = stat_rx_bytes;   stat_rx_bytes   = 0;
        uint32_t lost  = stat_lost_audio; stat_lost_audio = 0;
        uint32_t late  = stat_late_audio; stat_late_audio = 0;
        uint32_t under = stat_underruns;  stat_underruns  = 0;
        uint32_t over  = stat_overruns;   stat_overruns   = 0;
        float    espk  = stat_echo_spk;   stat_echo_spk   = 0.0f;
        float    emic  = stat_echo_mic;   stat_echo_mic   = 0.0f;
        float    mhz   = stat_mic_hz;     stat_mic_hz     = 0.0f;
        uint32_t trim  = stat_trims;      stat_trims      = 0;
        uint32_t txf   = stat_tx_fail;    stat_tx_fail    = 0;

        // The probe's window is 8 s and this line comes every 2 s, so the same
        // estimate is reported four times over.  Left that way on purpose: a
        // reading that holds still across repeats is a reading you can trust,
        // and one that jumps around is telling you the correlation is weak.
        // Dominant frequency of the loudest mic block.  During a ring this is
        // the ring; during speech it reads as brightness.  Aim MIC_SHELF_HZ
        // with it: what matters is where it settles while the box is howling,
        // not what it does while someone is talking.
        char hz_str[16];
        if (mhz > 0.0f) snprintf(hz_str, sizeof(hz_str), "%luHz", (unsigned long)mhz);
        else            snprintf(hz_str, sizeof(hz_str), "-");

        char delay_str[40];
#if ENABLE_DELAY_PROBE
        if (!probe_blocks) {
            snprintf(delay_str, sizeof(delay_str), "measuring");
        } else if (probe_delay_ms == PROBE_NO_DELAY) {
            // Far end too quiet, or a near-end talker drowning the echo.
            snprintf(delay_str, sizeof(delay_str), "weak r=%ld%%",
                     (long)(probe_corr * 100.0f));
        } else {
            snprintf(delay_str, sizeof(delay_str), "%lums r=%ld%% n=%lu",
                     (unsigned long)probe_delay_ms,
                     (long)(probe_corr * 100.0f),
                     (unsigned long)probe_blocks);
        }
#else
        snprintf(delay_str, sizeof(delay_str), "off");
#endif

        Serial.printf(
            "[link=%s rssi=%d] tx=%lu B/s rx=%lu B/s | jitter=%lu ms trim=%lu "
            "lost=%lu late=%lu under=%lu over=%lu txfail=%lu | "
            "echo spk=%lu mic=%lu hz=%s delay=%s | heap=%lu\n",
            linkStateName(link_state), WiFi.RSSI(),
            (unsigned long)(tx * 1000 / period_ms),
            (unsigned long)(rx * 1000 / period_ms),
            (unsigned long)(jitter.available() / BYTES_PER_MS),
            (unsigned long)trim,
            (unsigned long)lost, (unsigned long)late,
            (unsigned long)under, (unsigned long)over, (unsigned long)txf,
            (unsigned long)espk, (unsigned long)emic,
            hz_str, delay_str,
            (unsigned long)ESP.getFreeHeap());
    }
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("\nTalkBox handset \"%s\" (room \"%s\")\n", DEVICE_ID, ROOM_ID);

    sock_mutex = xSemaphoreCreateMutex();
    jitter.begin(JITTER_CAPACITY);

    setupLeds();
    setupMic();
    setupSpeaker();

    // Started before connectToWiFi: that call blocks in the setup portal until
    // someone configures the box, and an unlit strip is indistinguishable from
    // a dead one.
    xTaskCreatePinnedToCore(taskLeds,    "leds",    4096, NULL, 1, NULL, 0);

    connectToWiFi();
    udp.begin(LOCAL_PORT);
    resolveRelay();

    // Core 1: the real-time audio path.
    xTaskCreatePinnedToCore(taskNetRx,   "net rx",  8192, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(taskSpeaker, "speaker", 8192, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(taskMicTx,   "mic tx",  8192, NULL, 3, NULL, 1);

    // Core 0: control path, shares the core with the WiFi stack.
    xTaskCreatePinnedToCore(taskLink,    "link",    8192, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(taskPotTx,   "pot tx",  4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(taskPortal,  "portal",  8192, NULL, 1, NULL, 0);
#if ENABLE_DIAGNOSTICS
    xTaskCreatePinnedToCore(taskStats,   "stats",   4096, NULL, 1, NULL, 0);
#endif
}

void loop() {
    vTaskDelete(NULL);   // everything runs in the tasks above
}
