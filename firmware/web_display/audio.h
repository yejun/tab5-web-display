// Sound: the PC records what Chromium plays and streams it as UDP datagrams to port 8765 (the same number as the
// TCP port). Each datagram is { "TAUD", u32 pos, u16 frames, u16 0 } (little endian) followed by `frames` frames
// of 48 kHz stereo 16-bit PCM; pos is the stream position of the first frame (it wraps after ~25 hours). The PC
// only sends while there is sound, so a quiet page costs nothing.
//
// UDP rather than TCP: TCP would stall everything behind one lost packet until it is resent. Here a lost packet
// is 7.5 ms of silence. A jitter buffer absorbs WiFi's uneven delivery:
//  - When it runs dry (WiFi held packets back), playback pauses with silence and keeps its place, so nothing that
//    arrives afterwards is lost; the buffer has then grown by the length of the hold-up.
//  - Its target adapts: 10 ms more after each hold-up, 5 ms less after every 20 s without one, within
//    AUDIO_MIN_MS..AUDIO_MAX_MS. A quiet network gets little delay, a busy one no dropouts.
//  - It is kept near the target by playing a few frames in 480 fewer or more (at most 0.6 %, too little to
//    hear), which also follows the small clock difference between the PC and the Tab5.
//
// A task of its own receives and feeds the speaker, so sound keeps going while the main loop decodes a large
// update. The speaker (ES8388 codec and amplifier) is switched on by the main loop when sound starts and off
// again AUDIO_OFF_MS after it ends, since M5Unified's I2C bus is not shared safely between tasks.
#pragma once
#include <atomic>
#include <lwip/sockets.h>
#include <freertos/semphr.h>
#include <freertos/message_buffer.h>

static constexpr int AUDIO_RATE = 48000;
static constexpr int AUDIO_RING = 1 << 15;            // frames buffered at most (683 ms)
static constexpr int AUDIO_MIN_MS = 40, AUDIO_START_MS = 60, AUDIO_MAX_MS = 250;   // jitter buffer target
static constexpr int AUDIO_BLOCK = 480;               // frames handed to the speaker at a time (10 ms)
static constexpr int AUDIO_CH = 0;                    // speaker channel for the stream (tone() uses another)
static constexpr int64_t AUDIO_END_MS = 300;          // no packet for this long: the sound has ended
static constexpr int64_t AUDIO_OFF_MS = 5000;         // speaker off this long after the last sound

#pragma pack(push, 1)
struct AudioHeader { char magic[4]; uint32_t pos; uint16_t frames, pad; };
#pragma pack(pop)
static_assert(sizeof(AudioHeader) == 12, "wire format");

struct AudioStats { uint32_t packets, late, lost_frames, holdups, dropped, repeated; };
static AudioStats g_astats = {};

static std::atomic<uint32_t> g_pc_ip{0};              // only the connected PC may play sound (0: none)
static std::atomic<bool> g_audio_want{false};         // a stream is buffering or playing
static std::atomic<bool> g_spk_ready{false};          // the speaker is on (set by the main loop)
static std::atomic<uint32_t> g_audio_last_ms{0};      // when the last sound packet arrived
static SemaphoreHandle_t g_spk_lock;                  // held around playRaw() and Speaker.end()
// Through the tunnel (tunnel.h) the datagrams come in a message buffer instead of on the socket
static std::atomic<bool> g_audio_tunnel{false};
static MessageBufferHandle_t g_audio_mb;
static StaticMessageBuffer_t g_audio_mb_struct;
static uint8_t g_vol = 180;

// ---- the audio task -----------------------------------------------------------------------------------------

static int16_t* g_ring;                               // AUDIO_RING stereo frames
static int64_t g_rd = 0, g_wr = 0;                    // next frame to play; end of what has arrived
static int16_t* g_blocks[4];                          // rotating buffers handed to playRaw()
static int g_block = 0;
enum { A_IDLE, A_BUFFERING, A_PLAYING };
static int g_astate = A_IDLE;
static int64_t g_alast = 0;                           // ms of the last packet (task-local copy)
static float g_level = 0;                             // smoothed buffer level, frames
static int g_target = AUDIO_RATE * AUDIO_START_MS / 1000;   // frames; kept between streams
static int64_t g_target_t = 0;                        // when the target last changed
static bool g_held = false;                           // ran dry while the stream was still going

static void ring_zero(int64_t from, int64_t to) {
  for (int64_t p = from; p < to; p++) {
    int16_t* f = g_ring + 2 * (p & (AUDIO_RING - 1));
    f[0] = f[1] = 0;
  }
}

static void audio_packet(const AudioHeader& h, const int16_t* pcm) {
  const int64_t now = now_ms();
  // widen the 32-bit position against the stream so far
  int64_t pos = g_astate == A_IDLE ? h.pos : g_wr + (int32_t)(h.pos - (uint32_t)g_wr);
  if (g_astate == A_IDLE || pos > g_wr + AUDIO_RING || pos + AUDIO_RING < g_rd) {   // a new stream (or a jump)
    g_rd = g_wr = pos;
    g_level = 0;
    g_held = false;   // the last stream ending is not a hold-up
    g_astate = A_BUFFERING;
    g_audio_want = true;
  }
  g_alast = now;
  g_audio_last_ms = (uint32_t)now;
  g_astats.packets++;
  if (g_held) {   // packets again after running dry: it was a hold-up (and not the end), so buffer more
    g_held = false;
    g_astats.holdups++;
    if (now - g_target_t > 200) {
      g_target = min(g_target + AUDIO_RATE / 100, AUDIO_RATE * AUDIO_MAX_MS / 1000);
      g_target_t = now;
    }
  }
  int64_t end = pos + h.frames;
  if (end <= g_rd) { g_astats.late++; return; }
  if (pos > g_wr) { g_astats.lost_frames += pos - g_wr; ring_zero(g_wr, pos); }   // a gap: silence unless it turns up
  if (end - g_rd > AUDIO_RING) g_rd = end - AUDIO_RING;   // far behind (the speaker was slow to start): skip ahead
  for (int64_t p = max(pos, g_rd); p < end; p++) {
    const int16_t* s = pcm + 2 * (p - pos);
    int16_t* d = g_ring + 2 * (p & (AUDIO_RING - 1));
    d[0] = s[0];
    d[1] = s[1];
  }
  if (end > g_wr) g_wr = end;
}

// Hand 10 ms blocks to the speaker while it has room (it queues two per channel).
static void audio_feed() {
  const int64_t now = now_ms();
  if (g_astate == A_IDLE) return;
  if (now - g_target_t > 20000 && g_target > AUDIO_RATE * AUDIO_MIN_MS / 1000) {
    g_target -= AUDIO_RATE / 200;
    g_target_t = now;
  }
  const int target = g_target;
  if (g_astate == A_BUFFERING) {
    if (now - g_alast > AUDIO_END_MS) { g_astate = A_IDLE; g_audio_want = false; return; }
    if (g_wr - g_rd < target || !g_spk_ready) {
      if (g_wr - g_rd > target) g_rd = g_wr - target;   // waiting for the speaker: keep only the newest
      return;
    }
    g_level = target;
    g_astate = A_PLAYING;
  }
  xSemaphoreTake(g_spk_lock, portMAX_DELAY);
  int queued;
  while (g_spk_ready && (queued = M5.Speaker.isPlaying(AUDIO_CH)) < 2) {
    const int64_t level = g_wr - g_rd;
    if (level <= 0 && now - g_alast > AUDIO_END_MS) {   // played out
      g_astate = A_IDLE;
      g_audio_want = false;
      break;
    }
    g_level += (level + queued * AUDIO_BLOCK - g_level) / 32;   // what the speaker holds counts too
    // steer towards the target: drop up to 3 frames in 480 when far above it, repeat 1 when below
    const int over = (int)g_level - target, band = AUDIO_RATE / 125;   // 8 ms
    int16_t* out = g_blocks[g_block];
    g_block = (g_block + 1) % 4;
    int step;   // frames consumed for this block
    if (level < AUDIO_BLOCK) {   // ran dry: play what there is, then silence, and wait for the rest
      step = (int)max(level, (int64_t)0);
      for (int i = 0; i < AUDIO_BLOCK; i++) {
        const int16_t* s = g_ring + 2 * ((g_rd + i) & (AUDIO_RING - 1));
        out[2 * i] = i < step ? s[0] : 0;
        out[2 * i + 1] = i < step ? s[1] : 0;
      }
      if (now - g_alast <= AUDIO_END_MS) g_held = true;
    } else {
      step = AUDIO_BLOCK;
      if (over > band) { step += min(3, 1 + over / (AUDIO_RATE * 30 / 1000)); g_astats.dropped++; }
      else if (over < -band) { step--; g_astats.repeated++; }
      step = (int)min((int64_t)step, level);
      for (int i = 0; i < AUDIO_BLOCK; i++) {   // resample step frames into AUDIO_BLOCK (nearest frame)
        const int16_t* s = g_ring + 2 * ((g_rd + i * step / AUDIO_BLOCK) & (AUDIO_RING - 1));
        out[2 * i] = s[0];
        out[2 * i + 1] = s[1];
      }
    }
    g_rd += step;
    M5.Speaker.playRaw(out, AUDIO_BLOCK * 2, AUDIO_RATE, true, 1, AUDIO_CH);
  }
  xSemaphoreGive(g_spk_lock);
}

static void audio_task(void*) {
  const int s = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(PORT);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  const int rcvbuf = 32 * 1024;
  setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  if (s < 0 || bind(s, (sockaddr*)&a, sizeof(a)) != 0) {
    Serial.println("ERR audio socket");
    vTaskDelete(nullptr);
  }
  static uint8_t pkt[1500];
  int timeout_ms = -1;
  for (;;) {
    const int want = g_astate == A_IDLE ? 200 : 4;   // idle: wake rarely
    if (want != timeout_ms) {
      timeout_ms = want;
      timeval tv = {0, timeout_ms * 1000};
      setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    sockaddr_in from = {};
    socklen_t fl = sizeof(from);
    int n;
    bool ok;   // from the connected PC
    if (g_audio_tunnel) {
      n = (int)xMessageBufferReceive(g_audio_mb, pkt, sizeof(pkt), pdMS_TO_TICKS(timeout_ms));
      ok = true;
    } else {
      n = recvfrom(s, pkt, sizeof(pkt), 0, (sockaddr*)&from, &fl);
      ok = from.sin_addr.s_addr == g_pc_ip.load() && g_pc_ip.load() != 0;
    }
    if (n >= (int)sizeof(AudioHeader) && ok) {
      AudioHeader h;
      memcpy(&h, pkt, sizeof(h));
      if (memcmp(h.magic, "TAUD", 4) == 0 && n == (int)(sizeof(h) + h.frames * 4u) && h.frames > 0)
        audio_packet(h, (const int16_t*)(pkt + sizeof(h)));
    }
    audio_feed();
  }
}

// ---- main-loop side -----------------------------------------------------------------------------------------

static void audio_setup() {
  auto sc = M5.Speaker.config();
  sc.sample_rate = AUDIO_RATE;
  sc.stereo = true;
  sc.dma_buf_len = 240;   // 4 x 5 ms of DMA buffering
  sc.dma_buf_count = 4;
  sc.task_priority = 5;
  sc.magnification = 8;   // M5Unified uses 4 on the Tab5; pages' sound is far quieter than its full-scale tones
  M5.Speaker.config(sc);
  g_vol = prefs.getUChar("vol", 180);
  g_spk_lock = xSemaphoreCreateMutex();
  g_ring = (int16_t*)heap_caps_calloc(AUDIO_RING * 2, sizeof(int16_t), MALLOC_CAP_SPIRAM);
  constexpr size_t MB = 32 * 1024;   // ~20 datagrams
  if (uint8_t* mb = (uint8_t*)heap_caps_malloc(MB + 1, MALLOC_CAP_SPIRAM))
    g_audio_mb = xMessageBufferCreateStatic(MB, mb, &g_audio_mb_struct);
  for (auto& b : g_blocks) b = (int16_t*)heap_caps_calloc(AUDIO_BLOCK * 2, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// Start the receiver once there is a network (called when WiFi first comes up).
static void audio_start() {
  static bool started = false;
  if (started || !g_ring) return;
  started = true;
  xTaskCreate(audio_task, "audio", 4096, nullptr, 6, nullptr);
}

static int64_t g_spk_keep = 0;   // keep the speaker on until then (volume preview)

static bool speaker_on() {
  if (g_spk_ready) return true;
  if (!M5.Speaker.begin()) { Serial.println("ERR speaker"); return false; }
  M5.Speaker.setVolume(g_vol);
  g_spk_ready = true;
  return true;
}

// Switch the speaker on when sound starts and off a while after it ends.
static void speaker_poll() {
  static int64_t quiet_since = 0;
  const int64_t now = now_ms();
  if (g_audio_want) { speaker_on(); quiet_since = now; return; }
  if (g_spk_ready && now > g_spk_keep && now - quiet_since > AUDIO_OFF_MS) {
    xSemaphoreTake(g_spk_lock, portMAX_DELAY);
    g_spk_ready = false;
    M5.Speaker.end();
    xSemaphoreGive(g_spk_lock);
  }
}

static void set_volume(uint8_t v, bool preview) {
  g_vol = v;
  if (g_spk_ready) M5.Speaker.setVolume(g_vol);
  if (preview && g_vol && speaker_on()) {
    g_spk_keep = now_ms() + 1000;
    M5.Speaker.tone(880, 120, 1);
  }
}

static bool audio_recent(int64_t ms) { return (uint32_t)now_ms() - g_audio_last_ms.load() < (uint32_t)ms; }
