// Shared helpers for the built-in apps.
// window.TAB5 is set by server.py on every page: { city, units, lat, lon } (all optional).
const TAB5 = window.TAB5 || {};
const $ = id => document.getElementById(id);

// Fill every <a class="home"> with the house icon + label ("Home", or its data-label).
document.querySelectorAll("a.home").forEach(a => {
  a.href = "home.html";
  a.innerHTML = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round">
    <path d="M3 11 12 3l9 8M5 9.5V21h5v-6h4v6h5V9.5"/></svg>${a.dataset.label || "Home"}`;
});

// Call fn now and then at the start of every minute (one screen update per minute, not per second).
function everyMinute(fn) {
  fn();
  setTimeout(() => { fn(); setInterval(fn, 60000); }, 60000 - Date.now() % 60000 + 50);
}

async function getJSON(url, timeoutMs = 10000) {
  const r = await fetch(url, { signal: AbortSignal.timeout(timeoutMs) });
  if (!r.ok) throw new Error(`${r.status} ${url}`);
  return r.json();
}

// Where are we? --lat/--lon, --city (geocoded by Open-Meteo), --city auto (the PC's IP address, via GeoJS), or
// New York by default. Cached for the page.
let _place;
async function place() {
  if (_place) return _place;
  let p;
  if (TAB5.lat != null && TAB5.lon != null) {
    p = { name: TAB5.city || "", lat: +TAB5.lat, lon: +TAB5.lon, country: TAB5.country || "" };
  } else if (!TAB5.city) {
    p = { name: "New York", lat: 40.7128, lon: -74.006, country: "US" };
  } else if (TAB5.city !== "auto") {
    const g = await getJSON(`https://geocoding-api.open-meteo.com/v1/search?count=1&name=${encodeURIComponent(TAB5.city)}`);
    if (!g.results?.length) throw new Error(`city not found: ${TAB5.city}`);
    const r = g.results[0];
    p = { name: r.name, lat: r.latitude, lon: r.longitude, country: r.country_code };
  } else {
    const g = await getJSON("https://get.geojs.io/v1/ip/geo.json");
    p = { name: g.city || g.region || "", lat: +g.latitude, lon: +g.longitude, country: g.country_code };
  }
  // Fahrenheit where people use it, unless --units says otherwise
  p.imperial = TAB5.units ? TAB5.units === "imperial" : ["US", "LR", "MM"].includes(p.country);
  return (_place = p);
}

// WMO weather codes (Open-Meteo) -> [text, icon kind]
function weatherInfo(code) {
  const t = {
    0: ["Clear", "sun"], 1: ["Mostly clear", "sun-cloud"], 2: ["Partly cloudy", "sun-cloud"], 3: ["Cloudy", "cloud"],
    45: ["Fog", "fog"], 48: ["Fog", "fog"], 51: ["Light drizzle", "rain"], 53: ["Drizzle", "rain"], 55: ["Drizzle", "rain"],
    56: ["Freezing drizzle", "rain"], 57: ["Freezing drizzle", "rain"], 61: ["Light rain", "rain"], 63: ["Rain", "rain"],
    65: ["Heavy rain", "rain"], 66: ["Freezing rain", "rain"], 67: ["Freezing rain", "rain"], 71: ["Light snow", "snow"],
    73: ["Snow", "snow"], 75: ["Heavy snow", "snow"], 77: ["Snow grains", "snow"], 80: ["Showers", "rain"],
    81: ["Showers", "rain"], 82: ["Heavy showers", "rain"], 85: ["Snow showers", "snow"], 86: ["Snow showers", "snow"],
    95: ["Thunderstorm", "storm"], 96: ["Thunderstorm", "storm"], 99: ["Thunderstorm", "storm"] };
  return t[code] || ["—", "cloud"];
}

// Gradients for the weather icons, put in the page once
const ICON_DEFS = `<svg width="0" height="0" style="position:absolute" aria-hidden="true"><defs>
  <radialGradient id="sunG" cx=".38" cy=".35" r=".7"><stop offset="0" stop-color="#fff3b8"/>
    <stop offset=".55" stop-color="#ffcc4d"/><stop offset="1" stop-color="#ff9a3c"/></radialGradient>
  <radialGradient id="glowG"><stop offset="0" stop-color="#ffc24d" stop-opacity=".55"/>
    <stop offset="1" stop-color="#ffc24d" stop-opacity="0"/></radialGradient>
  <linearGradient id="moonG" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#f1efff"/>
    <stop offset="1" stop-color="#a9a3f5"/></linearGradient>
  <radialGradient id="moonGlow"><stop offset="0" stop-color="#b3a8ff" stop-opacity=".45"/>
    <stop offset="1" stop-color="#b3a8ff" stop-opacity="0"/></radialGradient>
  <linearGradient id="cloudG" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffffff"/>
    <stop offset="1" stop-color="#c3cde0"/></linearGradient>
  <linearGradient id="cloudD" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#a7b2c8"/>
    <stop offset="1" stop-color="#6c7890"/></linearGradient>
  <linearGradient id="dropG" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#b6e2ff"/>
    <stop offset="1" stop-color="#4aa8ff"/></linearGradient>
  <linearGradient id="boltG" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff0a0"/>
    <stop offset="1" stop-color="#ffb020"/></linearGradient>
</defs></svg>`;

// A weather icon (kind from weatherInfo) as an SVG string: a glowing sun or moon, soft clouds, rain, snow, a bolt
function weatherIcon(kind, day = true, size = 64) {
  if (!document.getElementById("sunG")) document.body.insertAdjacentHTML("afterbegin", ICON_DEFS);
  const sun = (cx = 32, cy = 32, r = 13) => day
    ? `<circle cx="${cx}" cy="${cy}" r="${r * 2.1}" fill="url(#glowG)"/><circle cx="${cx}" cy="${cy}" r="${r}" fill="url(#sunG)"/>`
    : `<circle cx="${cx}" cy="${cy}" r="${r * 2}" fill="url(#moonGlow)"/>
       <path d="M${cx + 3} ${cy - r}a${r} ${r} 0 1 0 ${r - 1} ${r * 1.55}a${r * .82} ${r * .82} 0 0 1 -${r - 1} -${r * 1.55}z"
             fill="url(#moonG)"/>`;
  const cloud = (dx = 0, dy = 0, s = 1, fill = "url(#cloudG)") =>
    `<path transform="translate(${dx} ${dy}) scale(${s})" fill="${fill}"
           d="M17 48h29a11 11 0 0 0 1.5-21.9A15 15 0 0 0 18.6 24 12 12 0 0 0 17 48z"/>`;
  const drops = [20, 31, 42].map((x, i) =>
    `<path d="M${x} ${51 + (i % 2) * 3}l-3 7a3 3 0 1 0 5.6 1.6z" fill="url(#dropG)"/>`).join("");
  const flakes = [20, 32, 44].map((x, i) =>
    `<g transform="translate(${x} ${56 + (i % 2) * 3})" stroke="#e8f2ff" stroke-width="2" stroke-linecap="round">
     <line x1="-4" y1="0" x2="4" y2="0"/><line x1="-2" y1="-3.5" x2="2" y2="3.5"/><line x1="-2" y1="3.5" x2="2" y2="-3.5"/></g>`).join("");
  const body = {
    "sun": sun(32, 32, 14),
    "sun-cloud": sun(24, 24, 11) + cloud(4, 6, .92),
    "cloud": cloud(-6, -6, .8, "url(#cloudD)") + cloud(4, 4, .95),
    "fog": cloud(0, -8, 1, "url(#cloudD)") + `<g stroke="#c3cde0" stroke-width="3.5" stroke-linecap="round" opacity=".9">
           <line x1="12" y1="48" x2="52" y2="48"/><line x1="18" y1="56" x2="46" y2="56"/></g>`,
    "rain": cloud(0, -7) + drops,
    "snow": cloud(0, -9) + flakes,
    "storm": cloud(0, -8, 1, "url(#cloudD)") + `<path d="M34 41l-9 12h7l-4 10 12-14h-7l4-8z" fill="url(#boltG)"/>`,
  }[kind] || cloud();
  return `<svg width="${size}" height="${size}" viewBox="0 0 64 64" overflow="visible">${body}</svg>`;
}

// The glows behind the cards (--g1..3, common.css): for the weather, by day and by night, or for the time of day
const SKY = {
  "sun":       { day: ["#14a39a", "#2f7fe0", "#7a5cf0"], night: ["#3b3fa8", "#5a3fb0", "#1c6f8f"] },
  "sun-cloud": { day: ["#2394a0", "#3a6fcc", "#6f5bd8"], night: ["#35408f", "#4f3d9e", "#24607e"] },
  "cloud":     { day: ["#3f6f8f", "#4a5f8c", "#5d5a8a"], night: ["#2c3c66", "#3a3a6e", "#28465e"] },
  "fog":       { day: ["#5a7488", "#56667f", "#646a86"], night: ["#2f3d55", "#383e5c", "#2c4252"] },
  "rain":      { day: ["#1f6fa8", "#2f56b0", "#3f4fa0"], night: ["#1c3f7a", "#2a3a86", "#1d4d6e"] },
  "snow":      { day: ["#6aa6d8", "#7d8fe0", "#9c8ee0"], night: ["#3f5c99", "#56559c", "#3a6390"] },
  "storm":     { day: ["#5a3fb0", "#3f4fa8", "#b0663a"], night: ["#4a2f96", "#2f3a86", "#8a4a2a"] },
};
const SKY_HOURS = [   // [from hour, colours]: night, dawn, day, dusk
  [0, ["#3b3fa8", "#5a3fb0", "#1c6f8f"]], [5, ["#c0607a", "#5a4fc0", "#e08a4a"]], [8, ["#14a39a", "#2f7fe0", "#7a5cf0"]],
  [17, ["#d0703f", "#8a4fb8", "#c04f6f"]], [20, ["#3b3fa8", "#5a3fb0", "#1c6f8f"]]];
function setSky(colors) {
  colors.forEach((c, i) => document.documentElement.style.setProperty(`--g${i + 1}`, c));
}
function skyForWeather(kind, day) { setSky((SKY[kind] || SKY.cloud)[day ? "day" : "night"]); }
function skyForHour(h = new Date().getHours()) { setSky(SKY_HOURS.filter(([from]) => h >= from).at(-1)[1]); }

const fmtTime = (d, opts = {}) => d.toLocaleTimeString([], { hour: "numeric", minute: "2-digit", ...opts });

// "8:58" and "PM" separately (period is "" where the locale uses 24-hour time), for big clocks.
function timeParts(d, opts = {}) {
  const parts = new Intl.DateTimeFormat([], { hour: "numeric", minute: "2-digit", ...opts }).formatToParts(d);
  const period = parts.find(p => p.type === "dayPeriod")?.value || "";
  const hm = parts.filter(p => p.type !== "dayPeriod").map(p => p.value).join("").trim();
  return { hm, period };
}

// 0 = Sunday ... 6 = Saturday: the first day of the week where the user lives
function firstDayOfWeek() {
  try {
    const loc = new Intl.Locale(navigator.language);
    const info = loc.getWeekInfo ? loc.getWeekInfo() : loc.weekInfo;
    return info.firstDay % 7;
  } catch { return 1; }
}
