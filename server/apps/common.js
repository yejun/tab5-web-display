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

// Simple line-art weather icon as an SVG string.
function weatherIcon(kind, day = true, size = 64) {
  const sun = day
    ? `<circle cx="24" cy="24" r="9" fill="var(--sun)"/><g stroke="var(--sun)" stroke-width="3" stroke-linecap="round">
       ${[0, 45, 90, 135, 180, 225, 270, 315].map(a => `<line x1="24" y1="8" x2="24" y2="4" transform="rotate(${a} 24 24)"/>`).join("")}</g>`
    : `<path d="M30 10a13 13 0 1 0 10 20a11 11 0 0 1-10-20z" fill="var(--violet)"/>`;
  const cloud = (dx = 0, dy = 0, c = "var(--ink)") =>
    `<path transform="translate(${dx} ${dy})" d="M18 46h26a10 10 0 0 0 0-20a14 14 0 0 0-27-2a9 9 0 0 0 1 22z" fill="${c}"/>`;
  const drops = `<g stroke="var(--sky)" stroke-width="3" stroke-linecap="round"><line x1="22" y1="52" x2="19" y2="60"/>
                 <line x1="32" y1="52" x2="29" y2="60"/><line x1="42" y1="52" x2="39" y2="60"/></g>`;
  const flakes = `<g fill="var(--ink)"><circle cx="21" cy="56" r="2.5"/><circle cx="31" cy="59" r="2.5"/><circle cx="41" cy="56" r="2.5"/></g>`;
  const bolt = `<path d="M33 44l-7 10h6l-3 9 9-12h-6l3-7z" fill="var(--sun)"/>`;
  const body = {
    "sun": sun,
    "sun-cloud": `<g transform="translate(-4 -4) scale(.85)">${sun}</g>${cloud(4, 4, "var(--ink)")}`,
    "cloud": cloud(0, 0, "var(--dim)") + cloud(6, 4),
    "fog": `<g stroke="var(--dim)" stroke-width="4" stroke-linecap="round"><line x1="10" y1="24" x2="54" y2="24"/>
            <line x1="14" y1="34" x2="50" y2="34"/><line x1="10" y1="44" x2="54" y2="44"/></g>`,
    "rain": cloud(0, -6) + drops, "snow": cloud(0, -6) + flakes, "storm": cloud(0, -8, "var(--dim)") + bolt,
  }[kind];
  return `<svg width="${size}" height="${size}" viewBox="0 0 64 64">${body}</svg>`;
}

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
