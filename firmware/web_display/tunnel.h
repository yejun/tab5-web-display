// Remote server: the Tab5 connects out to the PC, through a tunnel or reverse proxy that the user sets up (a
// Cloudflare Tunnel, ngrok, Tailscale Funnel, nginx, ...), so it works on any network that reaches that, not only on
// the PC's. On the PC, `server.py --listen PORT` runs a bridge (server/tunnel.py) that takes the Tab5's connection on
// that port and stands in for the Tab5 on 127.0.0.1, so the server drives it as usual.
//
// Remote mode is a setting (the panel, or serial: tunnel <https address> | tunnel on | tunnel off; saved). While it
// is on, the Tab5 takes no connections from the local network: on a public network anyone could take the screen
// and read what is typed. The address must be https: the connection is a WebSocket over TLS (port 443 unless given,
// the server's certificate checked against the core's CA bundle).
//
// Pairing: the Tab5 has a random 128-bit ID (made once, saved) that it sends with every connection, and a six-digit
// code made from it, shown on its screen. The bridge asks on the PC before taking a Tab5 it doesn't know ("Pair the
// Tab5 with code 482 913?"), and keeps the ones allowed. It answers with a text message: "pair" (asking on the PC),
// "ok" (known) or "denied" (then it closes; the Tab5 asks again after a minute).
//
// The Tab5 keeps a WebSocket open to <address>/tab5 while it waits; when server.py connects, the bridge says
// "attach", and from then on binary messages carry the main connection's bytes both ways. After offering lanes, the
// Tab5 opens <address>/lane once per lane, and the bridge joins each to one of the server's lane connections, which
// then open with "TLAN" as on the network.
//
// Sound: the server sends its UDP datagrams to the bridge, which passes each on as a message of its own on the main
// WebSocket. There every message from the bridge opens with a channel byte: 0 = the stream's bytes, 1 = a sound
// datagram. The Tab5 hands those to audio.h through a message buffer (as datagrams from 127.0.0.1, each sendto()
// waited for lwIP's task and held internal RAM), so the jitter buffer and the rest work as on the network (where a
// late datagram is lost; here it comes late, behind the stream, and the buffer grows).
#pragma once
#include <esp_tls.h>
#include <esp_crt_bundle.h>
#include <mbedtls/base64.h>
#include <mbedtls/platform.h>
#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/sha256.h>

static void drop_client();
static void local_listen(bool on);

static constexpr int TUN_OPEN_MS = 10000;
static constexpr int64_t TUN_DENIED_MS = 60000;   // not allowed on the PC: ask again after this
enum : uint8_t { PAIR_NONE, PAIR_ASKING, PAIR_OK, PAIR_DENIED };   // the bridge's answer
// Lanes over the tunnel: off. With them, three connections each held a 16 KB TLS record buffer in internal RAM, and
// their TCP windows (in lwIP's internal RAM) filled whenever a task fell behind: internal RAM ran out (the AES
// engine then failed, and the connection with it). Through a tunnel the Tab5's TCP connection usually ends at the
// tunnel's nearest server, so one connection's 64 KB window covers a short round trip.
static constexpr bool TUN_LANES = false;

// What has arrived on a connection, taken off the socket in large pieces (RawIn below), and read from there.
struct RawIn {
  int fd;
  uint8_t* buf;                         // TUN_RAW bytes, in PSRAM
  uint32_t wr, rd;
  bool eof;                             // closed, or failed
};

// A WebSocket client over TLS, non-blocking once open: ws_read() returns what has arrived, 0 for nothing yet.
struct Ws {
  esp_tls_t* tls = nullptr;
  int fd = -1;
  bool dead = false, attached = false;
  uint8_t pair = PAIR_NONE;             // what the bridge said about this Tab5
  uint8_t hdr[14];
  uint8_t hn = 0;                       // frame header bytes read so far
  uint8_t op = 0;                       // the frame being read: its opcode and payload bytes still to come
  uint64_t left = 0;
  uint8_t ctl[126];                     // a control or text frame's payload
  uint8_t cn = 0;
  uint32_t open_ms = 0;                 // how long opening took (TLS handshake and upgrade)
  int err = 0;                          // why it closed: esp_tls_conn_read()'s answer, or 1 = a close frame
  bool chans = false;                   // messages open with a channel byte (the main WebSocket)
  bool fin = false;                     // the frame being read ends its message
  int ch = 0;                           // the message's channel (-1: its byte not read yet)
  uint8_t snd[1500];                    // a sound datagram being read
  uint16_t sn = 0;
  RawIn* in = nullptr;
};

static Ws g_tw;                         // the main WebSocket: waiting for the PC, then its connection
static std::atomic<int> g_tw_state{0};  // 0: none (the connector task opens one), 1: open (the main loop's)
static uint8_t* g_tbuf = nullptr;       // the main WebSocket's ring (what arrived, not yet read)
struct TunStats { uint32_t opens, fails, lane_opens, lane_open_ms; char why[48]; int queued_max[3];
                  uint64_t read_bytes, read_us; uint32_t read_max_us; };
static TunStats g_tstats = {};

// mbedTLS's memory (~40 KB a connection) in PSRAM, where internal RAM is short; except the record buffer that arriving
// data is decrypted in: the AES engine's DMA can't work in PSRAM there, and the copies through internal RAM made
// decrypting a 16 KB record take 3.8 ms instead of 0.4 (serial "tlsbench"). Slower than WiFi, so every connection's
// TCP window (64 KB, in lwIP's internal RAM) filled while it waited: internal RAM ran out. mbedtls_ssl_setup()
// allocates that buffer first, then the one for sending, so opening a connection (one at a time) counts them.
static SemaphoreHandle_t g_tls_open_lock;
static int g_tls_big = 0;   // record-sized buffers allocated by the connection being opened

static void* tun_calloc(size_t n, size_t size) {
  if (n * size >= 16384 && g_tls_big++ == 0) {
    void* p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (p) return p;
  }
  void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// "https://name[:port]/path" -> host "name[:port]", path prefix "/path"
static void tun_split(const char* addr, char* host, size_t hn, char* prefix, size_t pn) {
  const char* p = strstr(addr, "://");
  p = p ? p + 3 : addr;
  const char* slash = strchr(p, '/');
  const size_t n = slash ? (size_t)(slash - p) : strlen(p);
  snprintf(host, hn, "%.*s", (int)n, p);
  snprintf(prefix, pn, "%s", slash ? slash : "");
  const size_t pl = strlen(prefix);
  if (pl && prefix[pl - 1] == '/') prefix[pl - 1] = 0;
}

static void ws_queued(const Ws& w, int i) {   // (stats: the most that waited in lwIP, unread)
  int n = 0;
  if (w.fd >= 0 && ioctl(w.fd, FIONREAD, &n) == 0 && n > g_tstats.queued_max[i]) g_tstats.queued_max[i] = n;
}

// Arriving data waits in lwIP's buffers, in internal RAM that DMA can reach, until it is read; mbedTLS read it a
// record header and then a record at a time, and each recv() waits for lwIP's task, which is busiest just then. So a
// burst (a whole 64 KB TCP window, from Cloudflare's servers) stayed there for a while, and with sound playing too,
// that memory ran out: the AES engine couldn't get its DMA descriptors, and the connection failed. Instead, whatever
// has arrived is taken off the socket in large pieces into a PSRAM ring, as the lanes do, and mbedTLS reads from that.
static constexpr uint32_t TUN_RAW = 1 << 17;   // (a power of two)

static int raw_take(RawIn* in, uint8_t* p, size_t n) {   // > 0 bytes, 0 closed, -1 nothing yet
  if (in->wr == in->rd && !in->eof) {   // take all that has arrived
    for (;;) {
      const uint32_t used = in->wr - in->rd, off = in->wr & (TUN_RAW - 1);
      if (used == TUN_RAW) break;
      const int r = recv(in->fd, in->buf + off, min(TUN_RAW - used, TUN_RAW - off), MSG_DONTWAIT);
      if (r > 0) { in->wr += r; continue; }
      if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) in->eof = true;
      break;
    }
  }
  const uint32_t avail = in->wr - in->rd, off = in->rd & (TUN_RAW - 1);
  if (!avail) return in->eof ? 0 : -1;
  const uint32_t m = min(min((uint32_t)n, avail), TUN_RAW - off);
  memcpy(p, in->buf + off, m);
  in->rd += m;
  return m;
}

static int raw_bio_recv(void* ctx, unsigned char* p, size_t n) {   // mbedTLS's input
  const int r = raw_take((RawIn*)ctx, p, n);
  return r >= 0 ? r : MBEDTLS_ERR_SSL_WANT_READ;
}

static int raw_bio_send(void* ctx, const unsigned char* p, size_t n) {
  const int r = send(((RawIn*)ctx)->fd, p, n, MSG_DONTWAIT);
  if (r >= 0) return r;
  return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int tls_rd(Ws& w, void* p, size_t n) { return esp_tls_conn_read(w.tls, p, n); }
static int tls_wr(Ws& w, const void* p, size_t n) { return esp_tls_conn_write(w.tls, p, n); }

static bool ws_wait(const Ws& w, int ms) {   // until something arrives (or the socket fails)
  fd_set r;
  FD_ZERO(&r);
  FD_SET(w.fd, &r);
  timeval tv = {ms / 1000, (ms % 1000) * 1000};
  return select(w.fd + 1, &r, nullptr, nullptr, &tv) != 0;
}

static bool tls_write_all(Ws& w, const uint8_t* p, size_t n) {
  const int64_t end = now_ms() + 2000;
  while (n) {
    const int r = tls_wr(w, p, n);
    if (r > 0) { p += r; n -= r; continue; }
    if ((r != ESP_TLS_ERR_SSL_WANT_WRITE && r != ESP_TLS_ERR_SSL_WANT_READ) || now_ms() > end) { w.dead = true; return false; }
    delay(1);
  }
  return true;
}

static bool ws_send(Ws& w, uint8_t op, const uint8_t* p, size_t n) {   // one masked frame (a client must mask)
  if (w.dead || !w.tls || n > 1024) return false;
  uint8_t f[1024 + 8];
  size_t h = 0;
  f[h++] = 0x80 | op;
  if (n < 126) {
    f[h++] = 0x80 | n;
  } else {
    f[h++] = 0x80 | 126;
    f[h++] = n >> 8;
    f[h++] = n & 0xFF;
  }
  const uint32_t key = esp_random();
  memcpy(f + h, &key, 4);
  const uint8_t* k = f + h;
  h += 4;
  for (size_t i = 0; i < n; i++) f[h + i] = p[i] ^ k[i & 3];
  return tls_write_all(w, f, h + n);
}

static void ws_close(Ws& w) {
  if (w.tls) esp_tls_conn_destroy(w.tls);   // (closes the socket)
  if (w.in) {
    heap_caps_free(w.in->buf);
    heap_caps_free(w.in);
  }
  w.in = nullptr;
  w.tls = nullptr;
  w.fd = -1;
  w.dead = true;
}

static void tun_sound(const uint8_t* p, int n) {   // a sound datagram, on to audio_task() (dropped if it's behind)
  if (g_audio_mb) xMessageBufferSend(g_audio_mb, p, n, 0);
}

// Payload bytes of binary messages (with chans, of channel 0), as they come: > 0 bytes, 0 nothing yet, -1 closed.
// Answers pings and passes sound on along the way.
static int ws_read(Ws& w, uint8_t* p, size_t n) {
  for (;;) {
    if (w.dead || !w.tls) return -1;
    const bool data = w.op == 0 || w.op == 2;
    int r;
    if (w.left && data && w.ch < 0) {
      uint8_t c;
      r = tls_rd(w, &c, 1);
      if (r > 0) { w.ch = c; w.sn = 0; w.left--; continue; }
    } else if (w.left && data && w.ch == 1) {   // sound (anything past a datagram's size dropped)
      uint8_t sink[64];
      const bool keep = w.sn < sizeof(w.snd);
      r = tls_rd(w, keep ? w.snd + w.sn : sink,
                            (size_t)min((uint64_t)(keep ? sizeof(w.snd) - w.sn : sizeof(sink)), w.left));
      if (r > 0) {
        if (keep) w.sn += r;
        if (!(w.left -= r) && w.fin) tun_sound(w.snd, w.sn);
        continue;
      }
    } else if (w.left && data) {
      r = tls_rd(w, p, (size_t)min((uint64_t)n, w.left));
      if (r > 0) { w.left -= r; return r; }
    } else if (w.left) {   // a control or text frame (only the first 125 bytes kept)
      uint8_t sink[64];
      const bool keep = w.cn < sizeof(w.ctl) - 1;
      r = tls_rd(w, keep ? w.ctl + w.cn : sink,
                            (size_t)min((uint64_t)(keep ? sizeof(w.ctl) - 1 - w.cn : sizeof(sink)), w.left));
      if (r > 0) {
        if (keep) w.cn += r;
        if (!(w.left -= r)) goto frame_done;
        continue;
      }
    } else {               // between frames: the header
      size_t need = 2;
      if (w.hn >= 2) need += ((w.hdr[1] & 127) == 126 ? 2 : (w.hdr[1] & 127) == 127 ? 8 : 0) + (w.hdr[1] & 128 ? 4 : 0);
      if (w.hn < need) {
        r = tls_rd(w, w.hdr + w.hn, need - w.hn);
        if (r > 0) w.hn += r;
      } else {
        r = 1;
        if (w.hdr[1] & 128) { w.dead = true; return -1; }   // a server's frames are never masked
        const uint8_t l7 = w.hdr[1] & 127;
        uint64_t len = l7;
        if (l7 == 126) len = (uint64_t)w.hdr[2] << 8 | w.hdr[3];
        if (l7 == 127) { len = 0; for (int i = 2; i < 10; i++) len = len << 8 | w.hdr[i]; }
        w.op = w.hdr[0] & 15;
        w.fin = w.hdr[0] & 0x80;
        if (w.op == 2) w.ch = w.chans ? -1 : 0;   // (a continuation keeps its message's channel)
        w.left = len;
        w.hn = w.cn = 0;
        if (!len && w.op >= 8) goto frame_done;
      }
      if (r > 0) continue;
    }
    if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) return 0;
    w.err = r;
    w.dead = true;   // closed (0) or failed
    return -1;
  frame_done:
    if (w.op == 9) ws_send(w, 10, w.ctl, w.cn);                                      // ping: pong
    else if (w.op == 8) { w.err = 1; w.dead = true; return -1; }                     // close
    else if (w.op == 1) {                                                             // the bridge's word
      w.ctl[w.cn] = 0;
      if (!strcmp((char*)w.ctl, "attach")) w.attached = true;
      else if (!strcmp((char*)w.ctl, "pair")) w.pair = PAIR_ASKING;
      else if (!strcmp((char*)w.ctl, "ok")) w.pair = PAIR_OK;
      else if (!strcmp((char*)w.ctl, "denied")) w.pair = PAIR_DENIED;
    }
    w.op = 2;   // (the next frame's header sets it)
  }
}

// ---- the Tab5's ID ----

static char g_dev_id[33] = "";      // 128 random bits in hex: sent with every connection, so kept secret
static char g_dev_code[8] = "";     // "482 913", made from it, shown on both sides when pairing
static char g_dev_hash[65] = "";    // SHA-256 of the ID (what the bridge keeps), for pairing over USB

static void tun_identity() {   // (made once, the first time remote mode is on)
  if (!g_dev_id[0]) snprintf(g_dev_id, sizeof(g_dev_id), "%s", prefs.getString("devid", "").c_str());
  if (strlen(g_dev_id) != 32) {
    uint8_t r[16];
    esp_fill_random(r, sizeof(r));
    for (int i = 0; i < 16; i++) snprintf(g_dev_id + 2 * i, 3, "%02x", r[i]);
    prefs.putString("devid", g_dev_id);
  }
  uint8_t h[32];
  mbedtls_sha256((const uint8_t*)g_dev_id, 32, h, 0);
  for (int i = 0; i < 32; i++) snprintf(g_dev_hash + 2 * i, 3, "%02x", h[i]);
  const uint32_t c = ((uint32_t)h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3]) % 1000000;
  snprintf(g_dev_code, sizeof(g_dev_code), "%03u %03u", (unsigned)(c / 1000), (unsigned)(c % 1000));
}

// ---- WebSocket ----

static bool ws_open(Ws& w, const char* addr, const char* path, char* why, size_t whyn) {
  w = Ws{};
  w.dead = true;
  char hostport[96], prefix[64];
  tun_split(addr, hostport, sizeof(hostport), prefix, sizeof(prefix));
  char host[96];
  snprintf(host, sizeof(host), "%s", hostport);
  char* colon = strrchr(host, ':');
  const int port = colon ? atoi(colon + 1) : 443;
  if (colon) *colon = 0;
  const int64_t t0 = now_ms();
  esp_tls_cfg_t cfg = {};
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = TUN_OPEN_MS;
  // (not cfg.non_block: esp-tls then retries the handshake's reads in a busy loop, which starved the other tasks
  // until the task watchdog fired; the socket turns non-blocking below, once the handshake is done)
  w.tls = esp_tls_init();
  if (!w.tls) { snprintf(why, whyn, "no memory"); return false; }
  xSemaphoreTake(g_tls_open_lock, portMAX_DELAY);
  g_tls_big = 0;
  const int ok = esp_tls_conn_new_sync(host, strlen(host), port, &cfg, w.tls);
  g_tls_big = 1;   // (nothing else goes to internal RAM)
  xSemaphoreGive(g_tls_open_lock);
  if (ok != 1) {
    snprintf(why, whyn, "TLS connection to %s failed", host);
    ws_close(w);
    return false;
  }
  w.dead = false;
  esp_tls_get_conn_sockfd(w.tls, &w.fd);
  fcntl(w.fd, F_SETFL, fcntl(w.fd, F_GETFL, 0) | O_NONBLOCK);
  w.in = (RawIn*)heap_caps_calloc(1, sizeof(RawIn), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (w.in) w.in->buf = (uint8_t*)heap_caps_malloc(TUN_RAW, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!w.in || !w.in->buf) { snprintf(why, whyn, "no memory"); ws_close(w); return false; }
  w.in->fd = w.fd;
  mbedtls_ssl_set_bio((mbedtls_ssl_context*)esp_tls_get_ssl_context(w.tls), w.in, raw_bio_send, raw_bio_recv, nullptr);
  const int one = 1;
  setsockopt(w.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  uint8_t key[16], key64[32];
  size_t kl = 0;
  esp_fill_random(key, sizeof(key));
  mbedtls_base64_encode(key64, sizeof(key64), &kl, key, sizeof(key));
  char req[448];
  const int n = snprintf(req, sizeof(req), "GET %s%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Tab5\r\nUpgrade: websocket\r\n"
                         "Connection: Upgrade\r\nSec-WebSocket-Key: %.*s\r\nSec-WebSocket-Version: 13\r\n"
                         "X-Tab5-Device: %s\r\n\r\n", prefix, path, hostport, (int)kl, key64, g_dev_id);
  if (!tls_write_all(w, (const uint8_t*)req, n)) { snprintf(why, whyn, "upgrade not sent"); ws_close(w); return false; }
  // the answer's head, a byte at a time (whatever follows it is the first frame)
  char head[512];
  size_t hl = 0;
  const int64_t end = now_ms() + TUN_OPEN_MS;
  while (hl < sizeof(head) - 1 && !(hl >= 4 && !memcmp(head + hl - 4, "\r\n\r\n", 4))) {
    const int r = tls_rd(w, head + hl, 1);
    if (r == 1) { hl++; continue; }
    if ((r != ESP_TLS_ERR_SSL_WANT_READ && r != ESP_TLS_ERR_SSL_WANT_WRITE) || now_ms() > end) break;
    ws_wait(w, 100);
  }
  head[hl] = 0;
  if (strncmp(head, "HTTP/1.1 101", 12)) {
    char* eol = strchr(head, '\r');
    if (eol) *eol = 0;
    snprintf(why, whyn, "%s", hl ? head : "no answer");
    ws_close(w);
    return false;
  }
  w.open_ms = now_ms() - t0;
  return true;
}

static const char* ws_cipher(const Ws& w) {
  if (!w.tls) return "-";
  return mbedtls_ssl_get_ciphersuite((mbedtls_ssl_context*)esp_tls_get_ssl_context(w.tls));
}

// ---- the main connection ----

// The connector task keeps a WebSocket open and, like a lane task, moves whatever arrives on it into a PSRAM ring
// at once. Left in lwIP until the main loop came for it, it took up to 56 KB of internal RAM. g_tw_lock guards
// g_tw, g_tw_state and g_tun_client between this task and the main loop (sending events, attaching, closing).
static SemaphoreHandle_t g_tw_lock;
static constexpr uint32_t TUN_RING = 1 << 20;   // (a power of two)
static std::atomic<uint32_t> g_twr{0}, g_trd{0};   // bytes stored by the task / taken by the main loop

static int64_t g_tun_retry_at = 0;   // not before then (denied on the PC)
static uint8_t g_tun_pair = PAIR_NONE;   // the bridge's last answer, kept while reconnecting

static void tun_connect_task(void*) {
  bool failing = false;
  for (;;) {
    if (g_tw_state.load() == 1) {
      const uint32_t wr = g_twr.load(), used = wr - g_trd.load(), off = wr & (TUN_RING - 1);
      if (used >= TUN_RING) { vTaskDelay(1); continue; }   // the main loop is behind
      int r = -1;
      xSemaphoreTake(g_tw_lock, portMAX_DELAY);
      if (g_tw_state.load() == 1) {
        ws_queued(g_tw, 0);
        const int64_t t0 = esp_timer_get_time();
        r = ws_read(g_tw, g_tbuf + off, min(TUN_RING - used, TUN_RING - off));
        if (r > 0) {
          const uint32_t us = esp_timer_get_time() - t0;
          g_tstats.read_bytes += r;
          g_tstats.read_us += us;
          g_tstats.read_max_us = max(g_tstats.read_max_us, us);
        }
        if (g_tw.pair != PAIR_NONE && g_tw.pair != g_tun_pair) {
          g_tun_pair = g_tw.pair;
          Serial.printf("tunnel: %s\n", g_tun_pair == PAIR_OK ? "paired with the PC" : g_tun_pair == PAIR_ASKING ?
                        "the PC is asking whether to pair" : "not allowed on the PC; asking again in a minute");
          if (g_tun_pair == PAIR_DENIED) g_tun_retry_at = now_ms() + TUN_DENIED_MS;
        }
        if (r < 0 && !g_tun_client) {   // closed while waiting for the PC: open another (with a PC, the main loop does)
          if (g_tun_pair != PAIR_DENIED) Serial.printf("tunnel: the waiting connection closed (%d)\n", g_tw.err);
          ws_close(g_tw);
          g_tw_state = 0;
        }
      }
      xSemaphoreGive(g_tw_lock);
      if (r > 0) g_twr.store(wr + r);
      else if (r == 0) ws_wait(g_tw, 50);
      else delay(10);
      continue;
    }
    char addr[sizeof(g_tun)];
    memcpy(addr, g_tun, sizeof(addr));
    // (not while the settings are open: the PC is refused then, and would be over and over)
    if (g_remote && addr[0] && g_wifi == WIFI_UP && !g_panel_open && now_ms() >= g_tun_retry_at) {
      Ws w;
      char why[48];
      if (ws_open(w, addr, "/tab5", why, sizeof(why))) {
        if (strcmp(addr, g_tun)) { ws_close(w); continue; }   // the address changed meanwhile
        w.chans = true;
        xSemaphoreTake(g_tw_lock, portMAX_DELAY);
        g_tw = w;
        g_twr = g_trd = 0;
        g_tw_state = 1;
        xSemaphoreGive(g_tw_lock);
        g_tstats.opens++;
        g_tstats.why[0] = 0;
        if (failing) Serial.println("tunnel: reached");
        failing = false;
        Serial.printf("tunnel: open in %u ms (%s), waiting for the PC\n", (unsigned)w.open_ms, ws_cipher(w));
      } else {
        g_tstats.fails++;
        Serial.printf("tunnel: %s; retrying in 5 s\n", why);
        snprintf(g_tstats.why, sizeof(g_tstats.why), "%s", why);
        failing = true;
        delay(5000);
      }
    }
    delay(100);
  }
}

static void tun_close() {   // (the connector task then opens the next one)
  xSemaphoreTake(g_tw_lock, portMAX_DELAY);
  if (g_tw_state.load() == 1) ws_close(g_tw);
  g_tw_state = 0;
  g_tun_client = false;
  xSemaphoreGive(g_tw_lock);
}

// The waiting WebSocket: has the PC attached? Then (tun_attach()) it is the PC's connection from now on.
static bool tun_waiting_pc() { return !g_tun_client && g_tw_state.load() == 1 && g_tw.attached; }

static bool tun_attach() {
  xSemaphoreTake(g_tw_lock, portMAX_DELAY);
  const bool ok = g_tw_state.load() == 1 && g_tw.attached && !g_tw.dead;
  g_tun_client = ok;
  xSemaphoreGive(g_tw_lock);
  return ok;
}

// The PC connection, through the tunnel or straight over the network
static bool link_connected() {
  return g_tun_client ? g_tw_state.load() == 1 && (!g_tw.dead || g_twr.load() != g_trd.load()) : g_client.connected();
}
static int link_available() { return g_tun_client ? g_twr.load() - g_trd.load() : g_client.available(); }
static int link_read(uint8_t* p, size_t n) {
  if (!g_tun_client) return g_client.read(p, n);
  const uint32_t rd = g_trd.load(), off = rd & (TUN_RING - 1);
  const uint32_t m = min(min((uint32_t)n, g_twr.load() - rd), TUN_RING - off);
  memcpy(p, g_tbuf + off, m);
  g_trd.store(rd + m);
  return m;
}
static void link_write(const uint8_t* p, size_t n) {
  if (!g_tun_client) { g_client.write(p, n); return; }
  xSemaphoreTake(g_tw_lock, portMAX_DELAY);
  if (g_tw_state.load() == 1) ws_send(g_tw, 2, p, n);
  xSemaphoreGive(g_tw_lock);
}
static void link_stop() {
  if (g_tun_client) tun_close();
  else g_client.stop();
}

// ---- lanes ----

// One lane: open a WebSocket, take the PC's "TLAN" on it, and from then on move what arrives into that lane's ring,
// like lane_task(). Ends when the lanes close (g_tun_gen moves on) or the connection does.
static void tun_lane(const uint32_t gen) {
  char addr[sizeof(g_tun)], why[48];
  memcpy(addr, g_tun, sizeof(addr));
  Ws w;
  Lane* l = nullptr;
  if (ws_open(w, addr, "/lane", why, sizeof(why))) {
    g_tstats.lane_opens++;
    g_tstats.lane_open_ms = max(g_tstats.lane_open_ms, w.open_ms);
    RectHeader h;
    size_t got = 0;
    for (const int64_t end = now_ms() + TUN_LANE_SETUP_MS; got < sizeof(h) && gen == g_tun_gen && now_ms() < end;) {
      const int r = ws_read(w, (uint8_t*)&h + got, sizeof(h) - got);
      if (r < 0) break;
      if (r == 0) ws_wait(w, 50);
      got += max(r, 0);
    }
    if (got == sizeof(h) && !memcmp(h.magic, "TLAN", 4) && gen == g_tun_gen && h.y == g_lane_token && h.x < LANES_MAX &&
        g_lanes[h.x].fd < 0 && !g_lanes_on) {
      l = &g_lanes[h.x];
      l->wr = l->rd = 0;
      l->failed = false;
      l->running = true;
      l->tun = true;
      l->fd = w.fd;   // (last: lanes_start() counts it from here)
      while (gen == g_tun_gen) {
        const uint32_t wr = l->wr.load(), used = wr - l->rd.load();
        if (used >= LANE_RING) { vTaskDelay(1); continue; }
        const uint32_t off = wr & (LANE_RING - 1);
        ws_queued(w, 1 + (l - g_lanes));
        const int r = ws_read(w, l->ring + off, min(LANE_RING - used, LANE_RING - off));
        if (r < 0) break;
        if (r == 0) { ws_wait(w, 100); continue; }
        l->wr.store(wr + r);
      }
    } else if (gen == g_tun_gen) {
      Serial.printf("tunnel: lane not taken (%u bytes of its header)\n", (unsigned)got);
    }
  } else {
    Serial.printf("tunnel: lane: %s\n", why);
  }
  ws_close(w);
  if (l) {
    l->failed = true;
    l->running = false;
  }
}

// The lane tasks keep their stacks (12 KB each, for the TLS handshake) in PSRAM, where internal RAM is short. Such a
// task can't end itself, so they stay and wait for their next lane. (If lanes come back: see tun_setup() on stacks.)
static TaskHandle_t g_tun_lane_tasks[LANES_MAX];

static void tun_lane_task(void*) {
  for (;;) {
    ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
    tun_lane(g_tun_gen.load());
  }
}

static void tun_lanes_open() {
  for (TaskHandle_t t : g_tun_lane_tasks)
    if (t) xTaskNotifyGive(t);
}

// Serial "tlsbench": AES-128-GCM decryption speed, as for a 16 KB TLS record in PSRAM and in internal RAM.
#include <mbedtls/gcm.h>
static void tun_bench() {
  for (int where = 0; where < 2; where++) {
    const uint32_t caps = where ? MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT : MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(16384 + 64, caps);
    if (!buf) { Serial.println("tlsbench: no memory"); return; }
    uint8_t* p = buf + 13;   // (where a record's payload sits in mbedTLS's buffer)
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    uint8_t key[16] = {1}, iv[12] = {2}, tag[16];
    mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128);
    const int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < 32; i++) mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_DECRYPT, 16384, iv, 12, nullptr, 0, p, p, 16, tag);
    const int64_t us = esp_timer_get_time() - t0;
    uint8_t aad[13] = {3};   // as TLS 1.2 does, from the stack
    const int64_t t1 = esp_timer_get_time();
    for (int i = 0; i < 32; i++) mbedtls_gcm_auth_decrypt(&g, 16384, iv, 12, aad, 13, tag, 16, p, p);
    Serial.printf("tlsbench: %s, with a header from the stack: %.2f MB/s\n", where ? "internal" : "psram",
                  32 * 16384.0 / (esp_timer_get_time() - t1));
    mbedtls_gcm_free(&g);
    heap_caps_free(buf);
    Serial.printf("tlsbench: %s: %.2f MB/s\n", where ? "internal" : "psram", 32 * 16384.0 / us);
  }
}

// ---- setup and settings ----

// The second line of the status screen in remote mode
static void tun_status_line(char* b, size_t n) {
  if (g_tun_pair == PAIR_ASKING) snprintf(b, n, "Allow this Tab5 on the PC: pairing code %s", g_dev_code);
  else if (g_tun_pair == PAIR_DENIED) snprintf(b, n, "Not allowed on the PC (code %s); asking again in a minute", g_dev_code);
  else if (g_tw_state.load() == 1) snprintf(b, n, "Connected, waiting for server.py (pairing code %s)", g_dev_code);
  else if (g_tstats.why[0]) snprintf(b, n, "Can't reach it: %.60s", g_tstats.why);
  else snprintf(b, n, "Connecting ...  (pairing code %s)", g_dev_code);
}

static bool tun_ui_changed() {   // has anything in that line changed?
  static int last = -1;
  const int now = g_remote ? 1 + g_tw_state.load() * 2 + g_tun_pair * 4 + (g_tstats.why[0] ? 32 : 0) : 0;
  const bool changed = now != last;
  last = now;
  return changed && g_remote;
}

// Remote mode on or off (serial "tunnel ...", the panel), with a new address if one is given (https only).
static bool tun_set_remote(bool on, const char* addr = nullptr) {
  if (addr) {
    if (strncmp(addr, "https://", 8) || !addr[8]) return false;
    snprintf(g_tun, sizeof(g_tun), "%s", addr);
    prefs.putString("tun", g_tun);
  }
  if (on && !g_tun[0]) return false;
  g_remote = on;
  prefs.putBool("remote", on);
  if (on) tun_identity();
  drop_client();
  tun_close();
  g_tun_pair = PAIR_NONE;
  g_tun_retry_at = 0;
  g_tstats.why[0] = 0;
  if (g_wifi == WIFI_UP) local_listen(!on);
  status_screen();
  return true;
}

static void tun_command(const String& arg) {   // serial "tunnel [<address> | on | off | id]"
  if (arg == "on" || arg == "off") {
    if (!tun_set_remote(arg == "on")) Serial.println("tunnel: no address yet (tunnel <https address>)");
  } else if (arg == "id") {   // for pairing over USB: the ID's hash, which is what the bridge keeps
    tun_identity();
    Serial.printf("tunnel: device %s code %s\n", g_dev_hash, g_dev_code);
    return;
  } else if (arg.length() && !tun_set_remote(true, arg.c_str())) {
    Serial.println("tunnel: the address must start with https://");
    return;
  }
  Serial.printf("tunnel: remote mode %s, address %s, pairing code %s\n", g_remote ? "on" : "off",
                g_tun[0] ? g_tun : "none", g_dev_code[0] ? g_dev_code : "(made when remote mode is first on)");
}

static void tun_setup() {
  g_tls_open_lock = xSemaphoreCreateMutex();
  g_tls_big = 1;
  mbedtls_platform_set_calloc_free(tun_calloc, heap_caps_free);
  g_tbuf = (uint8_t*)heap_caps_malloc(TUN_RING, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  g_tw_lock = xSemaphoreCreateMutex();
  snprintf(g_tun, sizeof(g_tun), "%s", prefs.getString("tun", "").c_str());
  g_remote = g_tun[0] && prefs.getBool("remote", false);
  if (g_remote || prefs.isKey("devid")) tun_identity();
  // (its stack in internal RAM: mbedTLS passes the AES engine a record's header from the stack, and with that in
  // PSRAM the engine only did part of the work, decrypting at 3 MB/s instead of 40)
  if (xTaskCreate(tun_connect_task, "tunnel", 12288, nullptr, 5, nullptr) != pdPASS) Serial.println("tunnel: no task");
  constexpr UBaseType_t PSRAM = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  for (TaskHandle_t& t : g_tun_lane_tasks)
    if (xTaskCreateWithCaps(tun_lane_task, "tunlane", 12288, nullptr, 5, &t, PSRAM) != pdPASS) t = nullptr;
}

static void tun_stats() {
  char host[96], prefix[64];
  tun_split(g_tun, host, sizeof(host), prefix, sizeof(prefix));
  Serial.printf("tunnel | queued unread at most %d / %d / %d bytes (main / lanes) | read %llu bytes, %.2f MB/s while reading, longest read %u us\n",
                g_tstats.queued_max[0], g_tstats.queued_max[1], g_tstats.queued_max[2], g_tstats.read_bytes,
                g_tstats.read_us ? (double)g_tstats.read_bytes / g_tstats.read_us : 0.0, (unsigned)g_tstats.read_max_us);
  g_tstats.read_bytes = g_tstats.read_us = g_tstats.read_max_us = 0;
  g_tstats.queued_max[0] = g_tstats.queued_max[1] = g_tstats.queued_max[2] = 0;
  Serial.printf("tunnel | %s %s | %s, pairing %s, code %s | opened %u, failed %u | lanes opened %u, slowest %u ms | open took %u ms, %s\n",
                g_remote ? "remote" : "local, remote server", g_tun[0] ? host : "none",
                g_tun_client ? "PC attached" : g_tw_state.load() ? "waiting" : "not open",
                g_tun_pair == PAIR_OK ? "paired" : g_tun_pair == PAIR_ASKING ? "asking" : g_tun_pair == PAIR_DENIED ? "denied" : "-",
                g_dev_code[0] ? g_dev_code : "-",
                (unsigned)g_tstats.opens, (unsigned)g_tstats.fails, (unsigned)g_tstats.lane_opens,
                (unsigned)g_tstats.lane_open_ms, (unsigned)g_tw.open_ms, ws_cipher(g_tw));
  g_tstats.lane_opens = g_tstats.lane_open_ms = 0;
}
