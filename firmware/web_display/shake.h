#pragma once
// Shaking the Tab5 (firmware 1.6): the accelerometer is read 5 times a second and a shake goes to the server as
// EV_SHAKE, which hands it to the page as a "tab5shake" event (the built-in apps' holiday theme makes it snow).
// Older servers ignore the event. One back-and-forth is enough: two samples within SHAKE_MS that each moved - |a|
// more than SHAKE_G away from 1 g, or a change of more than SHAKE_DG since the last sample (a swing turns the vector
// even when it is gentle; tilting the Tab5 slowly changes it far less per 200 ms). The same rule as the fancy clock's.
// Each sample is two I2C transfers on the internal bus; only the accelerometer is powered (no gyro).

static constexpr int SHAKE_POLL_MS = 200, SHAKE_MS = 1000, SHAKE_QUIET_MS = 1500;
static constexpr float SHAKE_G = 0.3f, SHAKE_DG = 0.35f;
static bool g_imu_ok = false;
static bool g_imu_print = false;   // serial "imu": print the samples

static void shake_begin() {
  g_imu_ok = M5.Imu.isEnabled();
  if (!g_imu_ok) { Serial.println("no IMU: shaking does nothing"); return; }
  // M5Unified switches the gyro (~0.9 mA) on too; only the accelerometer is needed
  for (uint8_t a : {0x68, 0x69})
    if (M5.In_I2C.readRegister8(a, 0x00, 400000) == 0x24)   // BMI270 CHIP_ID
      M5.In_I2C.writeRegister8(a, 0x7D, 0x04, 400000);       // PWR_CTRL: accelerometer only
}

static void shake_send() {
  Serial.println("shake");
  send_event(EV_SHAKE, 0, 0);
}

static void shake_poll() {
  static int64_t next = 0, hit_at = -SHAKE_MS, sent_at = -SHAKE_QUIET_MS;
  static float last[3] = {0, 0, 0};
  const int64_t t = now_ms();
  if (!g_imu_ok || t < next) return;
  next = t + SHAKE_POLL_MS;
  float a[3];
  if (!M5.Imu.getAccel(&a[0], &a[1], &a[2])) return;
  const float dx = a[0] - last[0], dy = a[1] - last[1], dz = a[2] - last[2];
  const bool had = last[0] || last[1] || last[2];
  last[0] = a[0]; last[1] = a[1]; last[2] = a[2];
  if (g_imu_print) Serial.printf("imu %.2f %.2f %.2f g\n", a[0], a[1], a[2]);
  if (!had) return;
  const float mag = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  if (fabsf(mag - 1) < SHAKE_G && dx * dx + dy * dy + dz * dz < SHAKE_DG * SHAKE_DG) return;
  if (t - hit_at < SHAKE_MS) {
    hit_at = -SHAKE_MS;
    if (t - sent_at > SHAKE_QUIET_MS) { sent_at = t; shake_send(); }   // one per shake, not per swing
  } else {
    hit_at = t;
  }
}
