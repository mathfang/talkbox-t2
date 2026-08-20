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
static const uint8_t PULSE_RATE_SETUP  = 2;       // half pace; ~4 s "come configure me"
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
// Echo control
//
// The mic sits inches from the speaker, so every word the peer says reaches
// their own ear again one round trip later.  Their box does the same to us, so
// a single word recirculates — A's speaker -> A's mic -> B's speaker -> B's
// mic -> A's speaker — and a loop gain anywhere near 1 is what turns one echo
// into the three or four that are actually audible.
//
// Breaking the loop at ONE point collapses the whole chain, so this is the
// classic speakerphone trick rather than a real adaptive canceller: hold the
// mic down in proportion to how much of what it hears we are to blame for.  It
// costs one pass of abs() over each block plus a short scan, which is
// affordable on the audio core in a way that a 16 kHz NLMS filter would not be.
//
// It is not half-duplex.  Attenuation is proportional, not a gate, so talking
// over the peer works and the worst case is a quieter mic rather than a dead
// one.  A real canceller subtracts the echo and would do better during
// double-talk; this only turns the gain down, so some echo rides along with
// your voice whenever you and the peer talk at once.  That is the trade.
//
// Levels below are the mean |sample| of one block, i.e. 0..32767.
// ─────────────────────────────────────────────────────────────────────────────
#define ENABLE_ECHO_SUPPRESSION 1

// Playback quieter than this is left alone entirely.  Its only job is to keep
// us from dividing by the noise floor, so it sits just above room tone and no
// higher.  A gate needed this to be large — that was the only thing standing
// between it and latching shut forever — but an expander degrades smoothly, so
// a low value costs nothing: quiet playback yields a ratio near 1, hence a
// gain near 1, hence no attenuation.
//
// Compare against "spk=" in the stats line, which is the loudest block played
// in the last two seconds.  If spk never climbs past this while the peer is
// talking, nothing will ever engage.
static const float ECHO_FAR_ACTIVE = 30.0f;

// Ceiling on the pot's volume boost, and the single most important number
// here — it is a stability limit, not a taste setting.
//
// Per hop the loop multiplies by (coupling x mic_gain x volume), and the mic
// gain has to be near 1 whenever someone is actually talking, or they are not
// heard.  So stability rests on coupling x volume alone.  Measured coupling
// on this hardware — "mic" over "spk" in the stats while only the far end
// talks — runs 0.84 to 1.87, i.e. the mic picks the speaker up LOUDER than
// the samples being sent to it.  At that coupling anything above about 1.0
// oscillates: a single table knock was measured ramping spk 1977 -> 14974
// over eight seconds, which is the ringing heard after someone stops talking.
//
// LEFT AT 5.0 DELIBERATELY, which is roughly five times that limit and so
// does oscillate.  Dropping it to 1.0 stops the howl but makes the handset
// much quieter, and the loudness was worth more than the stability here.  The
// number is parked at the original value; the analysis is kept because it is
// the thing worth knowing if this is ever picked back up.
//
// No echo suppressor rescues a loop this hot: a howl and a loud talker look
// identical by level, so the suppressor opens up and feeds it.  Lowering the
// coupling — more distance or isolation between speaker and mic — is the only
// fix that buys back loudness and duplex at the same time.  Everything in
// firmware below is damage control.
static const float MAX_VOLUME = 5.0f;

// How far back to look before blaming our own speaker for what the mic hears.
// Our audio does NOT reach the mic at the moment we queue it: it waits out the
// I2S TX DMA depth, crosses the room, then waits out the I2S RX DMA depth on
// the way back in.  A few hundred milliseconds, and not observable from here.
//
// Hence a flat max over the whole window rather than a level that decays from
// the moment of playback.  A decaying one stops attenuating while the echo is
// still in flight, so the tail of every utterance gets through — which is
// heard, correctly, as an echo arriving a beat late.
static const uint32_t ECHO_TAIL_MS  = 500;
static const size_t   ECHO_BLOCK_MS = AUDIO_BLOCK / BYTES_PER_MS;    // 16 ms of speech per block
static const size_t   ECHO_HISTORY  = ECHO_TAIL_MS / ECHO_BLOCK_MS;  // blocks of playback remembered

// The mic gain is a smooth function of how far the mic is beating the speaker
// rather than an on/off gate, and this is its sharpness.  A hard gate has to
// pick a threshold, and any threshold is wrong in one direction or the other:
// too low and the echo returns, too high and it can never open — the mic level
// needed would exceed what 16 bits can represent, so the peer's voice gets
// flattened along with the echo and the call goes one-way.
//
// An expander has no threshold to get wrong.  Quiet-versus-playback fades
// down, loud-versus-playback passes, and everything between is proportional.
static const int   ECHO_KNEE       = 3;      // gain = (mic / expected echo) ^ KNEE
// Also the thing that decides whether the whole system oscillates, which is
// a stronger constraint than any echo target.  Round-trip loop gain is
//     (coupling x floor x volume)^2
// and it must stay well under 1 or a single thump builds into a howl at the
// round-trip period.  Measured coupling on this hardware is around 1.5 and
// the pot used to boost by up to 5x, so 0.15 gave 1.27 — unstable, which is
// exactly what the logs caught: one table knock ramping spk 1977 -> 14974
// over eight seconds.
//
// This alone cannot buy stability back, because the floor only applies while
// the suppressor believes it is hearing echo, and a howl looks exactly like a
// loud talker.  MAX_VOLUME above is what actually bounds the loop; this just
// keeps the residual quiet once the loop is already stable.
static const float ECHO_GAIN_FLOOR = 0.04f;

// The mic and the speaker are NOT in the same units.  A digital mic's output
// for ordinary speech is far smaller than the sample values we hand the
// amplifier, the more so with the pot's boost applied, so "mic quieter than
// playback" stays true even while you talk straight into it.  What matters is
// the RATIO of the two, which is a property of the box rather than of the
// volume: turning the pot up raises playback and the echo it provokes by the
// same factor, so the ratio holds still while both levels move by 30x.
//
// Which is why it is learned here instead of typed in.  A hand-set constant
// has to be re-derived every time the knob moves; this does not.
static const float    ECHO_MARGIN        = 2.0f;   // how far above the echo your voice must sit
static const uint32_t ECHO_LEARN_BLOCKS  = 64;     // ~1 s of evidence per update
static const float    ECHO_NEAR_FLOOR    = 10.0f;  // a silent mic teaches nothing
static const float    ECHO_COUPLING_INIT = 0.50f;
static const float    ECHO_COUPLING_MIN  = 0.02f;
static const float    ECHO_COUPLING_MAX  = 1.00f;

// Asymmetric on purpose.  Under-estimating only costs some suppression, while
// over-estimating mutes the call outright — and your own voice can only push
// the measurement up, never down.  So: fall quickly, rise grudgingly.
static const float    ECHO_LEARN_DOWN    = 0.80f;  // at most 20% down per update
static const float    ECHO_LEARN_UP      = 1.08f;  // at most  8% up   per update

static const float ECHO_ATTACK  = 1.00f;   // follow downward at once; the in-block ramp avoids the click
static const float ECHO_RELEASE = 0.25f;   // per block, gain -> open (~60 ms, so a syllable onset survives)

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

// Echo-suppressor peaks since the last stats line.  Peaks rather than
// instantaneous samples, because the interesting question is whether playback
// ever got loud enough to matter and whether the mic ever actually ducked —
// a snapshot taken every 2 s would miss both.
static volatile float stat_echo_spk  = 0.0f;   // loudest block we played
static volatile float stat_echo_mic  = 0.0f;   // loudest block the mic heard
static volatile float stat_echo_duck = 1.0f;   // deepest attenuation applied
static volatile float stat_echo_gsum = 0.0f;   // ... and the mean, which is the
static volatile uint32_t stat_echo_gn = 0;     // one that says whether you were heard
static volatile float stat_echo_rmin = 1e9f;   // raw mic/playback ratio, low end
static volatile float stat_echo_rmax = 0.0f;   // ... and high end

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
// Echo suppressor.  taskSpeaker reports what it just handed to the amplifier;
// taskMicTx ducks the mic whenever the block coming in looks like that same
// audio arriving back through the air.
// ─────────────────────────────────────────────────────────────────────────────
#if ENABLE_ECHO_SUPPRESSION

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

// What we have played recently, one level per block, owned by taskSpeaker.
static float  echo_far_hist[ECHO_HISTORY];
static size_t echo_far_pos = 0;

// Loudest of the above.  Written by taskSpeaker, read by taskMicTx: a 32-bit
// aligned float, so a race can only ever cost one stale block.
static volatile float echo_far_ref = 0.0f;

// Learned speaker-to-mic ratio, reported as "c=" so it can be sanity-checked
// from the serial log rather than taken on faith.
static volatile float echo_coupling = ECHO_COUPLING_INIT;

// Evidence accumulating toward the next update to it.
static float    echo_learn_spk = 0.0f;
static float    echo_learn_mic = 0.0f;
static uint32_t echo_learn_n   = 0;

// Called once per played block, including the silent ones — the window only
// slides forward if it keeps being fed.
static void echoNotePlayed(const uint8_t* data, size_t len) {
    echo_far_hist[echo_far_pos] = blockLevel(data, len);
    echo_far_pos = (echo_far_pos + 1) % ECHO_HISTORY;

    // A rescan every 16 ms, which is cheaper than maintaining the running
    // structure that would avoid it.
    float peak = 0.0f;
    for (size_t i = 0; i < ECHO_HISTORY; i++) {
        if (echo_far_hist[i] > peak) peak = echo_far_hist[i];
    }
    echo_far_ref = peak;
}

// Scales a mic block in place, holding it down in proportion to how much of it
// is our own speaker coming back around.  The gain ramps across the block
// rather than stepping at the boundary, which is what keeps it from clicking.
static void echoSuppress(uint8_t* data, size_t len) {
    static float gain = 1.0f;

    const float far_level  = echo_far_ref;
    const float near_level = blockLevel(data, len);

    // Below 1 the mic has no more than the echo we would predict, so most of
    // what it holds is our own audio coming back; above 1 someone is genuinely
    // talking over it.  Raising that to ECHO_KNEE sharpens the distinction
    // without ever turning it into a cliff.
    // Learn the room from the PEAKS of a whole second, not block against
    // block.  The echo of a block arrives a few hundred ms after we queue it,
    // so dividing an instantaneous mic level by a windowed playback level
    // compares two different moments: in the gaps between words the mic falls
    // silent while the playback window is still latched high, the quotient
    // collapses, and a low-watermark estimator would ratchet itself down to
    // nothing.  Two peaks measured over the same interval need no alignment.
    if (far_level  > echo_learn_spk) echo_learn_spk = far_level;
    if (near_level > echo_learn_mic) echo_learn_mic = near_level;

    if (++echo_learn_n >= ECHO_LEARN_BLOCKS) {
        if (echo_learn_spk > ECHO_FAR_ACTIVE && echo_learn_mic > ECHO_NEAR_FLOOR) {
            const float observed = echo_learn_mic / echo_learn_spk;
            const float c        = echo_coupling;
            const float moved    = (observed < c)
                                 ? max(c * ECHO_LEARN_DOWN, observed)
                                 : min(c * ECHO_LEARN_UP,   observed);
            echo_coupling = constrain(moved, ECHO_COUPLING_MIN, ECHO_COUPLING_MAX);
        }
        echo_learn_spk = 0.0f;
        echo_learn_mic = 0.0f;
        echo_learn_n   = 0;
    }

    float target = 1.0f;
    if (far_level > ECHO_FAR_ACTIVE) {
        const float raw = near_level / far_level;   // reported as "r="
        if (raw < stat_echo_rmin) stat_echo_rmin = raw;
        if (raw > stat_echo_rmax) stat_echo_rmax = raw;

        const float ratio    = raw / (echo_coupling * ECHO_MARGIN);
        float       expanded = ratio;
        for (int i = 1; i < ECHO_KNEE; i++) expanded *= ratio;
        target = constrain(expanded, ECHO_GAIN_FLOOR, 1.0f);
    }

    const float prev = gain;
    gain += (target - gain) * ((target < gain) ? ECHO_ATTACK : ECHO_RELEASE);
    if (far_level  > stat_echo_spk)  stat_echo_spk  = far_level;
    if (near_level > stat_echo_mic)  stat_echo_mic  = near_level;
    if (gain       < stat_echo_duck) stat_echo_duck = gain;
    stat_echo_gsum += gain;
    stat_echo_gn++;

    int16_t*     s = (int16_t*)data;
    const size_t n = len / 2;
    if (n == 0) return;

    const float step = (gain - prev) / (float)n;
    float       g    = prev;
    for (size_t i = 0; i < n; i++, g += step) {
        s[i] = (int16_t)(s[i] * g);
    }
}

#else   // ENABLE_ECHO_SUPPRESSION

static inline void echoNotePlayed(const uint8_t*, size_t) {}
static inline void echoSuppress(uint8_t*, size_t) {}
static const float echo_coupling = 0.0f;   // the stats line reports it either way

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
    in.begin(cfg_in);
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
        // Runs even with the link down, so the gate is already settled by the
        // time a call comes up.
        echoSuppress(tx_buffer, n);
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
                echoNotePlayed(silence, AUDIO_BLOCK);
                continue;
            }
        }

        if (jitter.pop(play_buffer, AUDIO_BLOCK)) {
            volume.write(play_buffer, AUDIO_BLOCK);
            // Measured after the write because VolumeStream scales in place:
            // this is what actually reached the amplifier, pot position and
            // all, which is exactly the level the mic will hear.
            echoNotePlayed(play_buffer, AUDIO_BLOCK);
        } else {
            // Ran dry: emit silence and re-prime rather than stuttering on
            // whatever trickles in next.
            stat_underruns++;
            primed = false;
            volume.write(silence, AUDIO_BLOCK);
            echoNotePlayed(silence, AUDIO_BLOCK);
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
        // Free-running, so changing pace slides rather than jumping.  The slow
        // pace means "unconfigured"; speeding up is the first visible sign that
        // credentials took and the box is working on the connection.
        breathe += (link_state == LINK_SETUP) ? PULSE_RATE_SETUP : PULSE_RATE;

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
            uint8_t hue = 160, sat = 200, cap = 60;   // blue: needs setup, or peer offline
            switch (link_state) {
            case LINK_WIFI_DOWN:  hue = 0;            break;   // red:   no WiFi
            case LINK_CONNECTING: sat = 0;  cap = 40; break;   // white: joining the network
            case LINK_NO_RELAY:   hue = 96;           break;   // green: waiting on the relay
            default:                                  break;
            }
            fill_solid(leds, NUM_LEDS, CHSV(hue, sat, scale8(sin8(breathe), cap)));
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
        float    educk = stat_echo_duck;  stat_echo_duck  = 1.0f;
        float    egsum = stat_echo_gsum;  stat_echo_gsum  = 0.0f;
        uint32_t egn   = stat_echo_gn;    stat_echo_gn    = 0;
        unsigned long emean = egn ? (unsigned long)(egsum / egn * 100.0f) : 100;
        float    ermin = stat_echo_rmin;  stat_echo_rmin  = 1e9f;
        float    ermax = stat_echo_rmax;  stat_echo_rmax  = 0.0f;
        uint32_t trim  = stat_trims;      stat_trims      = 0;
        uint32_t txf   = stat_tx_fail;    stat_tx_fail    = 0;

        // rmin stays at its sentinel if playback never crossed the threshold.
        unsigned long rlo = (ermin <= ermax) ? (unsigned long)(ermin * 100.0f) : 0;
        unsigned long rhi = (unsigned long)(ermax * 100.0f);

        Serial.printf(
            "[link=%s rssi=%d] tx=%lu B/s rx=%lu B/s | jitter=%lu ms trim=%lu "
            "lost=%lu late=%lu under=%lu over=%lu txfail=%lu | "
            "echo spk=%lu mic=%lu r=%lu-%lu%% c=%lu%% gain=%lu%% (min %lu%%) | heap=%lu\n",
            linkStateName(link_state), WiFi.RSSI(),
            (unsigned long)(tx * 1000 / period_ms),
            (unsigned long)(rx * 1000 / period_ms),
            (unsigned long)(jitter.available() / BYTES_PER_MS),
            (unsigned long)trim,
            (unsigned long)lost, (unsigned long)late,
            (unsigned long)under, (unsigned long)over, (unsigned long)txf,
            (unsigned long)espk, (unsigned long)emic,
            rlo, rhi,
            (unsigned long)(echo_coupling * 100.0f),
            emean, (unsigned long)(educk * 100.0f),
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
