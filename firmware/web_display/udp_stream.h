// EXPERIMENT (udp-experiment branch): the update stream over UDP instead of TCP.
//
// The PC asks for it with TCFG y bit 8 (after a hello with flag 8). It then sends each piece of an update (a run of
// the stream's records: "TAB5" / "TABS" rectangles, the end marker) as datagrams to UDP port 8768:
//   { "TUDP", u32 seq, u16 frag, u16 frags, u32 len } + up to UDP_CHUNK bytes of the piece (frag * UDP_CHUNK on)
// seq numbers the pieces (one counter for the session); magic "TUDQ" instead marks the last piece of an update.
// Pieces are taken strictly in order: when a datagram of a newer piece arrives, an incomplete older one is given up
// (lost), and datagrams of older pieces are dropped. So nothing ever waits for a resend; the PC repaints what was
// lost in a later update. Each complete piece is acknowledged at once with event 12 (x | y << 16 = its seq), from
// which the PC learns of losses. An update is drawn whole: once its last piece is in (or nothing more has come for
// UDP_HOLD_MS: that piece was lost), and, if it is timed ("TPTS" at the start of its first piece), when it is due.
// Its "shown" (event 5) then carries y | z << 16 = the seq of its last piece.
#pragma once
#include <lwip/sockets.h>

static constexpr uint16_t UDP_STREAM_PORT = 8768;
static constexpr uint32_t UDP_CHUNK = 1400;
static constexpr uint32_t UDP_RING = 6 << 20;         // pieces waiting to be drawn, back to back (PSRAM)
static constexpr uint32_t UDP_PIECE_MAX = (1 << 20) + 4096;
static constexpr uint32_t UDP_FRAGS_MAX = (UDP_PIECE_MAX + UDP_CHUNK - 1) / UDP_CHUNK;
static constexpr int UDP_QUEUE = 128;                 // complete pieces at most
static constexpr int64_t UDP_HOLD_MS = 100;           // an update without its last piece is drawn after this quiet

#pragma pack(push, 1)
struct UdpHeader { char magic[4]; uint32_t seq; uint16_t frag, frags; uint32_t len; };
#pragma pack(pop)
static_assert(sizeof(UdpHeader) == 16, "wire format");

struct UdpPiece { uint32_t seq, off, len; bool last; int64_t at; };
static uint8_t* g_uring;
static UdpPiece g_uq[UDP_QUEUE];
static std::atomic<uint32_t> g_uwr{0}, g_urd{0};      // complete pieces: put by the task / drawn by the main loop
static uint32_t g_uack = 0;                           // ... and acknowledged (main loop)
static std::atomic<bool> g_udp_on{false};
static std::atomic<uint32_t> g_udp_reset{0};          // bumped by the main loop: the task starts afresh
static int g_udp_fd = -1;
struct UdpStats { uint32_t datagrams, pieces, lost, late, full, bad; };
static UdpStats g_ustat = {};

// Where a piece of len bytes can go in the ring after `tail` (the end of the newest piece), clear of the pieces still
// waiting (from the oldest one's start); -1 if nowhere.
static int64_t udp_place(uint32_t tail, uint32_t len) {
  const uint32_t rd = g_urd.load(), wr = g_uwr.load();
  if (rd == wr) return 0;                             // nothing waiting: start at the beginning
  if (wr - rd >= UDP_QUEUE - 1) return -1;
  const uint32_t head = g_uq[rd % UDP_QUEUE].off;
  if (tail >= head) {
    if (tail + len <= UDP_RING) return tail;
    return len < head ? 0 : -1;
  }
  return tail + len < head ? (int64_t)tail : -1;
}

static void udp_task(void*) {
  static uint8_t pkt[sizeof(UdpHeader) + UDP_CHUNK];
  static uint32_t have[(UDP_FRAGS_MAX + 31) / 32];
  uint32_t reset_seen = 0, cur = 0, off = 0, len = 0, frags = 0, got = 0, tail = 0;
  bool any = false, assembling = false, last = false;
  for (;;) {
    const int n = recv(g_udp_fd, pkt, sizeof(pkt), 0);
    if (const uint32_t r = g_udp_reset.load(); r != reset_seen) { reset_seen = r; any = assembling = false; tail = 0; }
    if (n < (int)sizeof(UdpHeader) || !g_udp_on) continue;
    UdpHeader h;
    memcpy(&h, pkt, sizeof(h));
    const uint32_t body = n - sizeof(h);
    if ((memcmp(h.magic, "TUDP", 4) != 0 && memcmp(h.magic, "TUDQ", 4) != 0) || h.frags == 0 || h.frag >= h.frags || h.len > UDP_PIECE_MAX ||
        h.frags != (h.len + UDP_CHUNK - 1) / UDP_CHUNK || h.frag * UDP_CHUNK + body > h.len) { g_ustat.bad++; continue; }
    g_ustat.datagrams++;
    if (any && (int32_t)(h.seq - cur) < 0) { g_ustat.late++; continue; }
    if (!any || h.seq != cur) {                       // a newer piece: give up the one before, if incomplete
      if (assembling) g_ustat.lost++;
      any = true;
      cur = h.seq;
      assembling = false;
      const int64_t at = udp_place(tail, h.len);
      if (at < 0) { g_ustat.full++; continue; }
      off = (uint32_t)at; len = h.len; frags = h.frags; got = 0;
      last = h.magic[3] == 'Q';
      memset(have, 0, (frags + 31) / 32 * 4);
      assembling = true;
    }
    if (!assembling) continue;
    uint32_t& word = have[h.frag / 32];
    const uint32_t bit = 1u << (h.frag % 32);
    if (word & bit) continue;                         // a duplicate
    word |= bit;
    memcpy(g_uring + off + h.frag * UDP_CHUNK, pkt + sizeof(h), body);
    if (++got == frags) {                             // complete: hand it to the main loop
      assembling = false;
      g_uq[g_uwr.load() % UDP_QUEUE] = {cur, off, len, last, (int64_t)(esp_timer_get_time() / 1000)};
      tail = off + len;
      g_ustat.pieces++;
      g_uwr.fetch_add(1);
    }
  }
}

// From the PC's TCFG: start (or restart) the stream.
static void udp_start() {
  if (g_udp_fd < 0) {
    g_uring = (uint8_t*)heap_caps_malloc(UDP_RING, MALLOC_CAP_SPIRAM);
    g_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    const int rcvbuf = 64 * 1024;
    setsockopt(g_udp_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(UDP_STREAM_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!g_uring || bind(g_udp_fd, (sockaddr*)&a, sizeof(a)) != 0) { Serial.println("ERR udp stream socket"); return; }
    xTaskCreate(udp_task, "udpstream", 4096, nullptr, 6, nullptr);
  }
  g_urd.store(g_uwr.load());
  g_uack = g_uwr.load();
  g_udp_reset.fetch_add(1);
  g_udp_on = true;
  Serial.println("udp stream on");
}

static void udp_stop() {
  g_udp_on = false;
  g_urd.store(g_uwr.load());
  g_uack = g_uwr.load();
  g_udp_reset.fetch_add(1);
}

