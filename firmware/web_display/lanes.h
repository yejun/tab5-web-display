// Lanes: the update stream spread over two TCP connections. One connection can only have 64 KB in flight (lwIP's
// receive window, fixed in the precompiled libraries), which held it to about 3 MB/s; side by side, two carried about
// 4.4 MB/s and three 4.8 (measured on WiFi 6). Two it is: the router delivers each lane's window in bursts, into
// lwIP's buffers in internal RAM, before any task can take it. Over four minutes of video the lowest free internal RAM
// was 103 KB with one connection, 100 KB with two lanes and 33-56 KB with three, for 3 % more data than two.
//
// Setup, after the hello: the Tab5 offers lanes (event 7, flags 0: x = port, y = token, z = most lanes). The PC
// connects that many times to the port, each connection opening with { "TLAN", u16 lane, token, lanes, 0, u32 0 },
// then sends { "TSTR", u16 lanes, token, 0, 0, u32 0 } on the main connection. Once all lanes have arrived the Tab5
// answers (event 7, flags 1 = from now on the lanes, 2 = refused: the stream stays on the main connection). After
// that the stream continues on the lanes as chunks { u32 seq, u32 len } + len bytes, chunk seq on lane seq % lanes,
// and the main connection only carries the Tab5's events. A lane that fails ends the whole session.
//
// Each lane has a task that moves whatever arrives into a PSRAM ring right away. If lwIP held it instead, every
// lane's window would fill while a frame decodes: measured, three lanes read in turn took the free internal RAM from
// 219 KB to 140 bytes.
#pragma once
#include <lwip/sockets.h>

static constexpr int LANES_MAX = 2;
static constexpr uint16_t LANE_PORT = 8766;
static constexpr uint32_t LANE_RING = 1 << 20;        // per lane, in PSRAM (a power of two)
static constexpr uint32_t LANE_CHUNK_MAX = 1 << 16;
static constexpr int64_t LANE_SETUP_MS = 3000;
static constexpr int64_t TUN_LANE_SETUP_MS = 4500;    // through the tunnel (tunnel.h): TLS handshakes first; the PC waits 5 s

struct Lane {
  std::atomic<int> fd{-1};
  bool tun = false;                                   // a tunnel lane: its task owns the connection
  uint8_t* ring = nullptr;
  std::atomic<uint32_t> wr{0}, rd{0};                 // bytes stored by the lane task / taken by the main loop
  std::atomic<bool> running{false}, failed{false};
};
static Lane g_lanes[LANES_MAX];
static int g_lane_listen = -1;
static uint16_t g_lane_token = 0;
static int g_lanes_on = 0;                            // lanes in use; 0: the stream is on the main connection
static bool g_tun_client = false;                     // the current PC came through the tunnel (tunnel.h)
static std::atomic<uint32_t> g_tun_gen{0};            // moves on when the lanes close: tunnel lane tasks then end
static uint32_t g_lane_seq = 0, g_lane_left = 0;      // the chunk being read, and its bytes still to come
struct LaneStats { uint32_t waits, wait_ms, max_wait_ms, chunks; uint32_t lane_wait_ms[LANES_MAX];
                   uint32_t idle_max_ms, idle_lane, idle_others; };
static LaneStats g_lstats = {};

static void lane_task(void* arg) {
  Lane& l = *(Lane*)arg;
  for (;;) {
    const uint32_t wr = l.wr.load(), used = wr - l.rd.load();
    if (used >= LANE_RING) { vTaskDelay(1); continue; }   // the main loop is behind; TCP holds the PC back meanwhile
    const uint32_t off = wr & (LANE_RING - 1);
    const int r = recv(l.fd, l.ring + off, min(LANE_RING - used, LANE_RING - off), 0);
    if (r <= 0) break;   // closed, or shut down by lanes_close()
    l.wr.store(wr + r);
  }
  l.failed = true;
  l.running = false;
  vTaskDelete(nullptr);
}

static void lanes_begin() {
  for (Lane& l : g_lanes) {
    l.ring = (uint8_t*)heap_caps_malloc(LANE_RING, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!l.ring) { Serial.println("lanes: no memory, one connection only"); return; }
  }
}

static void lanes_listen(bool on) {   // (off in remote mode, see tunnel.h)
  if (!on) {
    if (g_lane_listen >= 0) close(g_lane_listen);
    g_lane_listen = -1;
    return;
  }
  if (g_lane_listen >= 0 || !g_lanes[LANES_MAX - 1].ring) return;
  const int s = socket(AF_INET, SOCK_STREAM, 0);
  const int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(LANE_PORT);
  if (s < 0 || bind(s, (sockaddr*)&a, sizeof(a)) || listen(s, LANES_MAX)) {
    Serial.println("lanes: cannot listen, one connection only");
    if (s >= 0) close(s);
    return;
  }
  fcntl(s, F_SETFL, O_NONBLOCK);
  g_lane_listen = s;
}

static void lanes_close() {
  g_tun_gen++;
  for (Lane& l : g_lanes)
    if (l.fd >= 0 && !l.tun) shutdown(l.fd, SHUT_RDWR);   // makes the lane task's recv() return
  for (Lane& l : g_lanes) {
    for (int i = 0; l.running && i < 200; i++) delay(5);
    if (l.running) Serial.println("lanes: a lane task did not stop");
    if (l.fd >= 0 && !l.tun) close(l.fd);
    l.fd = -1;
    l.tun = false;
    l.wr = l.rd = 0;
    l.failed = false;
  }
  g_lanes_on = 0;
  g_lane_seq = g_lane_left = 0;
}

// Accept a lane connection, if one is waiting: from the connected PC, with this session's token.
static void lanes_accept() {
  if (g_lane_listen < 0) return;
  sockaddr_in a = {};
  socklen_t al = sizeof(a);
  const int fd = accept(g_lane_listen, (sockaddr*)&a, &al);
  if (fd < 0) return;
  fcntl(fd, F_SETFL, 0);   // blocking
  timeval tv = {1, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  RectHeader h;
  int got = 0;
  while (got < (int)sizeof(h)) {
    const int r = recv(fd, (uint8_t*)&h + got, sizeof(h) - got, 0);
    if (r <= 0) break;
    got += r;
  }
  if (got != sizeof(h) || memcmp(h.magic, "TLAN", 4) || !g_have_client || g_tun_client || a.sin_addr.s_addr != g_pc_ip.load() ||
      h.y != g_lane_token || h.x >= LANES_MAX || g_lanes[h.x].fd >= 0 || g_lanes_on) {
    close(fd);
    return;
  }
  tv = {0, 0};   // no timeout from now on: a lane is quiet whenever the page is
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  Lane& l = g_lanes[h.x];
  l.fd = fd;
  l.wr = l.rd = 0;
  l.failed = false;
  l.running = true;
  if (xTaskCreate(lane_task, "lane", 4096, &l, 5, nullptr) != pdPASS) {
    l.running = false;
    l.failed = true;
  }
}

// On a new connection: offer lanes (event 7, flags 0) with a fresh token.
static void lanes_offer() {
  lanes_close();
  if (g_lane_listen < 0) return;
  g_lane_token = (uint16_t)(esp_random() | 1);
  send_event(EV_LANES, LANE_PORT, g_lane_token, 0, LANES_MAX);
}

// "TSTR" from the PC: wait for its lanes, then switch the stream over (or refuse and stay on the main connection).
static void lanes_start(const RectHeader& h) {
  const int n = h.x;
  bool ok = !g_lanes_on && n >= 1 && n <= LANES_MAX && h.y == g_lane_token && g_lane_listen >= 0;
  int have = 0;
  for (const int64_t end = now_ms() + (g_tun_client ? TUN_LANE_SETUP_MS : LANE_SETUP_MS); ok; delay(2)) {
    lanes_accept();
    have = 0;
    for (int i = 0; i < n; i++) have += g_lanes[i].fd >= 0 && !g_lanes[i].failed;
    if (have == n || now_ms() > end) break;
  }
  ok = ok && have == n;
  if (ok) {
    g_lanes_on = n;
    g_lane_seq = g_lane_left = 0;
  } else {
    lanes_close();
  }
  send_event(EV_LANES, LANE_PORT, g_lane_token, ok ? 1 : 2, n);
  Serial.printf("lanes: %d %s\n", n, ok ? "in use" : "refused, one connection");
}

static bool lane_take(Lane& l, uint8_t* p, uint32_t n, int64_t deadline) {
  int64_t wait_from = 0;
  while (n) {
    const uint32_t rd = l.rd.load(), avail = l.wr.load() - rd;
    if (!avail) {
      if (l.failed || now_ms() > deadline) return false;
      if (!wait_from) wait_from = now_ms();
      delay(1);
      continue;
    }
    if (wait_from) {
      const uint32_t w = now_ms() - wait_from;
      g_lstats.waits++;
      g_lstats.wait_ms += w;
      g_lstats.lane_wait_ms[&l - g_lanes] += w;
      if (w > g_lstats.max_wait_ms) g_lstats.max_wait_ms = w;
      wait_from = 0;
    }
    const uint32_t off = rd & (LANE_RING - 1), m = min(min(n, avail), LANE_RING - off);
    memcpy(p, l.ring + off, m);
    l.rd.store(rd + m);
    p += m;
    n -= m;
  }
  return true;
}

// n bytes of the stream from the lanes, chunk by chunk in order.
static bool lanes_read(uint8_t* p, size_t n) {
  const int64_t deadline = now_ms() + 3000;
  while (n) {
    Lane& l = g_lanes[g_lane_seq % g_lanes_on];
    if (!g_lane_left) {
      uint32_t hdr[2];
      if (!lane_take(l, (uint8_t*)hdr, sizeof(hdr), deadline)) return false;
      if (hdr[0] != g_lane_seq || hdr[1] == 0 || hdr[1] > LANE_CHUNK_MAX) {
        Serial.printf("ERR lane chunk %u, expected %u (%u bytes)\n", (unsigned)hdr[0], (unsigned)g_lane_seq, (unsigned)hdr[1]);
        return false;
      }
      g_lane_left = hdr[1];
      g_lstats.chunks++;
    }
    const uint32_t m = min((uint32_t)n, g_lane_left);
    if (!lane_take(l, p, m, deadline)) return false;
    p += m;
    n -= m;
    if (!(g_lane_left -= m)) g_lane_seq++;
  }
  return true;
}

static bool lanes_ready() {   // some of the stream has arrived
  static int64_t idle_from = 0;
  const Lane& l = g_lanes[g_lane_seq % g_lanes_on];
  const bool ready = l.wr.load() != l.rd.load();
  if (!ready && !idle_from) idle_from = now_ms();
  if (ready && idle_from) {
    const uint32_t w = now_ms() - idle_from;
    if (w > g_lstats.idle_max_ms) {
      g_lstats.idle_max_ms = w;
      g_lstats.idle_lane = g_lane_seq % g_lanes_on;
      uint32_t others = 0;
      for (int i = 0; i < g_lanes_on; i++) if (&g_lanes[i] != &l) others += g_lanes[i].wr.load() - g_lanes[i].rd.load();
      g_lstats.idle_others = others;
    }
    idle_from = 0;
  }
  return ready;
}

static bool lanes_failed() {
  for (int i = 0; i < g_lanes_on; i++)
    if (g_lanes[i].failed && g_lanes[i].wr.load() == g_lanes[i].rd.load()) return true;
  return false;
}
