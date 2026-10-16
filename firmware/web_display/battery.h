// Battery: voltage and current from the Tab5's power monitor, a charge estimate, and a line of text for the settings
// panel. Sampled once a second from the main loop, so the state is known by the time the panel opens.
#pragma once

// With no pack fitted the charger cycles its output capacitor between ~4.2 V and ~8.4 V, so a single high reading
// must not count as a full battery: a pack is "present" only after its voltage has stayed in range for 8 s.
struct BatteryPresence {
  enum State { Unknown, Absent, Present };
  State state = Unknown;
  int64_t high_since = -1;
  void update(int mv, int64_t now) {
    if (mv < 5500 || mv > 9000) { state = Absent; high_since = -1; return; }
    if (high_since < 0) high_since = now;
    if (now - high_since >= 8000) state = Present;
  }
};

// Resting-voltage state of charge for one Li-ion cell (typical curve); the pack is 2 cells in series.
static int battery_percent(int pack_mv) {
  static const int16_t curve[][2] = {
    {3270, 0}, {3610, 5}, {3690, 10}, {3710, 15}, {3730, 20}, {3750, 25}, {3770, 30},
    {3790, 35}, {3800, 40}, {3820, 45}, {3840, 50}, {3850, 55}, {3870, 60}, {3910, 65},
    {3950, 70}, {3980, 75}, {4020, 80}, {4080, 85}, {4110, 90}, {4150, 95}, {4200, 100}};
  const int n = sizeof(curve) / sizeof(curve[0]);
  const int mv = pack_mv / 2;
  if (mv <= curve[0][0]) return 0;
  if (mv >= curve[n - 1][0]) return 100;
  int i = 1;
  while (mv > curve[i][0]) ++i;
  return curve[i - 1][1] + (mv - curve[i - 1][0]) * (curve[i][1] - curve[i - 1][1]) / (curve[i][0] - curve[i - 1][0]);
}

// Charge current makes the terminal voltage read high and load current low; 0.22 ohm (measured on a Tab5 pack)
// gives the resting voltage the curve above expects.
static constexpr int PACK_R_MOHM = 220;
static constexpr int BATT_MAH = 2000;   // the Tab5's NP-F550-type pack, for the time estimate

struct Battery {
  BatteryPresence presence;
  int mv = 0, ma = 0, level = 0;   // ma: + charging, - discharging
  float ma_avg = NAN;              // smoothed, for the time estimate (the reading jumps with WiFi and CPU load)
  bool charging = false, external = false;
};
static Battery g_batt;

static void battery_poll() {
  static int64_t next = 0;
  const int64_t now = now_ms();
  if (now < next) return;
  next = now + 1000;
  Battery& b = g_batt;
  b.mv = M5.Power.getBatteryVoltage();
  b.presence.update(b.mv, now);
  if (b.presence.state != BatteryPresence::Present) { b.ma_avg = NAN; return; }
  b.ma = M5.Power.getBatteryCurrent();
  b.ma_avg = isnan(b.ma_avg) ? b.ma : b.ma_avg + (b.ma - b.ma_avg) * 0.1f;
  b.level = battery_percent(b.mv - (int)((int64_t)b.ma * PACK_R_MOHM / 1000));
  // CHG_STAT (what isCharging() reads) floats "charging" with USB unplugged, so go by the pack current instead:
  // about +680 mA charging, ~0 mA full on USB, -150 mA and more running on the battery.
  b.charging = b.ma > 20;
  b.external = b.ma > -30;
}

// e.g. "On battery  •  8.02 V  -420 mA  •  ~3 h 10 min left     [battery] 63%"
static void battery_text(char* out, size_t n) {
  const Battery& b = g_batt;
  if (b.presence.state != BatteryPresence::Present) {
    snprintf(out, n, b.presence.state == BatteryPresence::Absent ? LV_SYMBOL_USB "  USB power, no battery"
                                                                 : "Checking battery ...");
    return;
  }
  float hours = -1;
  if (b.charging && b.ma_avg > 50) hours = (100 - b.level) / 100.0f * BATT_MAH / b.ma_avg;
  else if (!b.external && b.ma_avg < -30) hours = b.level / 100.0f * BATT_MAH / -b.ma_avg;
  char est[40] = "";
  if (hours >= 0) {
    const int m = (int)(hours * 60 + 0.5f);
    snprintf(est, sizeof(est), "  \xe2\x80\xa2  ~%d h %02d min %s", m / 60, m % 60, b.charging ? "to full" : "left");
  }
  const char* state = b.charging ? "Charging" : b.external ? (b.level >= 95 ? "Full" : "Plugged in") : "On battery";
  const char* icon = b.level > 85 ? LV_SYMBOL_BATTERY_FULL : b.level > 60 ? LV_SYMBOL_BATTERY_3
                   : b.level > 35 ? LV_SYMBOL_BATTERY_2 : b.level > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
  snprintf(out, n, "%s  \xe2\x80\xa2  %.2f V  %+d mA%s     %s%s  %d%%", state, b.mv / 1000.0f, b.ma, est,
           b.external ? LV_SYMBOL_CHARGE " " : "", icon, b.level);
}
