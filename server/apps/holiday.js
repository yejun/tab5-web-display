// The holiday theme (common.js loads it, see holidayOn there): a winter evening behind the cards - a blue twilight
// sky with stars, a moon and a green aurora, snowy hills and pines, a snow drift along the bottom - snow lying on
// the cards, and snowfall for 20 s when the Tab5 is shaken (firmware 1.6 sends the shake; the server hands it to
// the page as a "tab5shake" event). Everything but the snowfall is drawn once: the Tab5 is sent a still picture.
(() => {
  const W = 1280, H = 720, NS = "http://www.w3.org/2000/svg";

  let seed = 20261224;   // the same scene every time
  const rnd = () => {   // mulberry32
    seed = (seed + 0x6D2B79F5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
  const f = n => +n.toFixed(1);
  const hex = c => [1, 3, 5].map(i => parseInt(c.slice(i, i + 2), 16));
  const mix = (a, b, t) => "#" + hex(a).map((v, i) => Math.round(v + (hex(b)[i] - v) * t).toString(16).padStart(2, "0")).join("");
  const wave = x => {   // smooth bumps, period 4, -1..1 (as the fancy clock's)
    const t = x - 4 * Math.floor(x / 4), u = t < 2 ? t - 1 : t - 3;
    return (t < 2 ? 1 : -1) * (1 - u * u);
  };

  // ---- the scene ----
  const SKY = ["#1b2f58", "#2b4a80", "#4a6fa6", "#7193c4"];   // top .. horizon
  const ridge = x => H - 150 + 18 * wave(x * .0034 + 2.2) + 7 * wave(x * .011 + .7);
  const hill = x => H - 92 + 10 * wave(x * .0047 + .3) + 4 * wave(x * .016 + 1.9);
  const drift = x => H - 44 + 7 * wave(x * .0058 + .4) + 3 * wave(x * .017 + 1.3);
  const contour = (fn, step = 16) => {
    let d = `M0 ${H}`;
    for (let x = 0; x <= W; x += step) d += `L${x} ${f(fn(x))}`;
    return d + `L${W} ${H}Z`;
  };

  // A pine: five stacked tiers, each a cone with up-turned tips and snow along its upper edges, on a short trunk.
  function pine(x, base, h, w, fog) {
    const col = mix("#0f3a3c", SKY[3], fog), snow = mix("#eef4ff", SKY[3], fog * .6), shade = mix("#b9cbe6", SKY[3], fog * .6);
    const k = h / 200;   // snow thickness scales with the tree
    let s = `<rect x="${f(x - w * .07)}" y="${f(base - h * .22)}" width="${f(w * .14)}" height="${f(h * .22)}" fill="${mix("#2b2019", SKY[3], fog)}"/>`;
    const N = 5, top = base - h;
    for (let i = N - 1; i >= 0; i--) {   // the lowest (widest) tier first, the ones above overlap it
      const th = h * .34, yt = top + i * (h * .8 - th) / (N - 1) + (i ? h * .04 : 0), yb = yt + th;
      const hw = w * .5 * (.38 + .62 * i / (N - 1)), tip = yb - th * .2;
      s += `<path d="M${f(x)} ${f(yt)}L${f(x + hw)} ${f(tip)}Q${f(x + hw * .45)} ${f(yb + th * .04)} ${f(x)} ${f(yb)}` +
           `Q${f(x - hw * .45)} ${f(yb + th * .04)} ${f(x - hw)} ${f(tip)}Z" fill="${col}"/>`;
      // snow on both upper edges: thicker towards the tips, in uneven clumps
      for (const side of [1, -1]) {
        const ex = side * hw, ey = tip - yt, len = Math.hypot(ex, ey);
        const nx = -side * ey / len, ny = Math.abs(ex) / len;   // into the tree
        const outer = [], inner = [];
        for (let j = 0; j <= 8; j++) {
          const v = .1 + .9 * j / 8, px = x + ex * v, py = yt + ey * v;
          const t = k * (1.4 + 6.5 * v * v) * (.7 + .5 * rnd());
          outer.push(`${f(px - nx * .5)} ${f(py - ny * .5)}`);
          inner.unshift(`${f(px + nx * t)} ${f(py + ny * t)}`);
        }
        s += `<path d="M${outer.join("L")}L${inner.join("L")}Z" fill="${side > 0 ? snow : shade}"/>`;
      }
    }
    s += `<path d="M${f(x)} ${f(top - 1)}l${f(-w * .05)} ${f(h * .07)}h${f(w * .1)}z" fill="${snow}"/>`;   // the snowy tip
    return s;
  }

  function scene() {
    let s = `<defs>
      <linearGradient id="hSky" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${SKY[0]}"/>
        <stop offset=".42" stop-color="${SKY[1]}"/><stop offset=".72" stop-color="${SKY[2]}"/><stop offset="1" stop-color="${SKY[3]}"/></linearGradient>
      <radialGradient id="hHorizon" cx=".55" cy="1" r=".65"><stop offset="0" stop-color="#b8c8ee" stop-opacity=".55"/>
        <stop offset="1" stop-color="#b8c8ee" stop-opacity="0"/></radialGradient>
      <linearGradient id="hAurora" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#46e0b0" stop-opacity="0"/>
        <stop offset=".3" stop-color="#46e0b0"/><stop offset=".65" stop-color="#4cc8f0"/><stop offset="1" stop-color="#8f8cf0" stop-opacity="0"/></linearGradient>
      <radialGradient id="hMoonGlow"><stop offset="0" stop-color="#fff6dc" stop-opacity=".55"/><stop offset=".35" stop-color="#e8e6ff" stop-opacity=".18"/>
        <stop offset="1" stop-color="#e8e6ff" stop-opacity="0"/></radialGradient>
      <radialGradient id="hMoon" cx=".4" cy=".38" r=".7"><stop offset="0" stop-color="#fffdf2"/><stop offset="1" stop-color="#e3dcc4"/></radialGradient>
      <linearGradient id="hRidge" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#a9bfe2"/><stop offset="1" stop-color="#7f9cc9"/></linearGradient>
      <linearGradient id="hHill" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#d2def2"/><stop offset="1" stop-color="#a7bbdc"/></linearGradient>
      <linearGradient id="hDrift" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f6f9ff"/><stop offset="1" stop-color="#cbd8ef"/></linearGradient>
      <filter id="hBlur" x="-20%" y="-50%" width="140%" height="200%"><feGaussianBlur stdDeviation="26"/></filter>
    </defs>
    <rect width="${W}" height="${H}" fill="url(#hSky)"/><rect width="${W}" height="${H}" fill="url(#hHorizon)"/>`;
    // aurora: soft ribbons across the upper sky
    s += `<g filter="url(#hBlur)" opacity=".5">`;
    for (const [y0, amp, th, ph] of [[150, 40, 46, 0], [215, 30, 30, 1.7], [110, 22, 22, 3.1]]) {
      let top = "", bot = "";
      for (let x = -100; x <= W + 100; x += 40) {
        const y = y0 + amp * wave(x * .004 + ph);
        top += `${top ? "L" : "M"}${x} ${f(y - th * (.6 + .4 * wave(x * .009 + ph)))}`;
        bot = `L${x} ${f(y + th * .4)}` + bot;
      }
      s += `<path d="${top}${bot}Z" fill="url(#hAurora)"/>`;
    }
    s += `</g>`;
    // stars, thinning out towards the horizon, and a few that sparkle
    for (let i = 0; i < 150; i++) {
      const x = rnd() * W, y = Math.pow(rnd(), 1.6) * 440, r = .5 + rnd() * rnd() * 1.4;
      s += `<circle cx="${f(x)}" cy="${f(y)}" r="${f(r)}" fill="#fff" opacity="${f(.35 + .6 * rnd() * (1 - y / 520))}"/>`;
    }
    for (const [x, y, r] of [[612, 150, 7], [520, 40, 5], [770, 118, 6], [968, 36, 5], [1210, 210, 6], [88, 250, 5], [640, 210, 4]])
      s += `<path d="M${x} ${y - r}Q${x} ${y} ${x + r} ${y}Q${x} ${y} ${x} ${y + r}Q${x} ${y} ${x - r} ${y}Q${x} ${y} ${x} ${y - r}Z" fill="#fffbe8" opacity=".95"/>`;
    // the moon, where every app's header leaves the sky free
    s += `<circle cx="620" cy="54" r="150" fill="url(#hMoonGlow)"/><circle cx="620" cy="54" r="25" fill="url(#hMoon)"/>
          <circle cx="612" cy="47" r="4.5" fill="#d8d0b6" opacity=".55"/><circle cx="628" cy="62" r="3" fill="#d8d0b6" opacity=".45"/>`;
    // far ridge with small misty pines, a nearer hill, then the near pines and the drift
    s += `<path d="${contour(ridge)}" fill="url(#hRidge)"/>`;
    for (let x = 14; x < W; x += 26 + rnd() * 46) {
      const h = 34 + rnd() * 34;
      s += pine(x, ridge(x) + 6, h, h * .52, .62);
    }
    s += `<path d="${contour(hill)}" fill="url(#hHill)"/>`;
    for (const [x, h] of [[300, 92], [342, 70], [590, 84], [628, 62], [905, 96], [946, 74], [1150, 80]])
      s += pine(x, hill(x) + 8, h, h * .5, .35);
    for (const [x, h] of [[28, 236], [118, 160], [440, 128], [712, 214], [790, 150], [1062, 176], [1232, 240], [1160, 140]])
      s += pine(x, drift(x) + 10, h, h * .48, 0);
    s += `<path d="${contour(drift)}" fill="url(#hDrift)"/>`;
    return s;
  }

  const bg = document.createElementNS(NS, "svg");
  bg.id = "holiday-scene";
  bg.setAttribute("viewBox", `0 0 ${W} ${H}`);
  bg.setAttribute("aria-hidden", "true");
  bg.innerHTML = scene();
  document.body.prepend(bg);

  // ---- snow lying on the cards ----
  // One layer over the page with a cap of snow along the top edge of each card, tile and note (not on cards inside
  // others). It follows the page as it changes, and grows while it snows.
  const caps = document.createElement("div");
  caps.id = "holiday-caps";
  document.body.append(caps);
  const CAPPED = ".card, a.tile, .note";
  let capKey = "";
  const CAP_TOP = 16;   // the card's top edge, in the cap's own coordinates
  function capPaths(w, r, sx) {
    // a soft pillow of snow along the edge, thickest in the middle, thinning out where the corners round off,
    // resting 3 px over the edge; and a few short icicle drips below it
    const rr = Math.max(4, Math.min(r, 30)), corner = x => {   // the card's edge, following the rounded corners
      const d = x < rr ? rr - x : x > w - rr ? x - (w - rr) : 0;
      return CAP_TOP + rr - Math.sqrt(Math.max(0, rr * rr - d * d));
    };
    const x0 = rr * .3, x1 = w - rr * .3, thick = Math.min(10, 5 + w / 120);
    const top = [], bottom = [];
    for (let x = x0; x <= x1 + .1; x += Math.max(4, (x1 - x0) / 120)) {
      const e = Math.min(1, Math.min(x - x0, x1 - x) / 50), taper = e * e * (3 - 2 * e);
      const lump = .82 + .14 * wave(x * .026 + sx) + .07 * wave(x * .071 + sx * 2.3);
      top.push(`${f(x)} ${f(corner(x) - thick * taper * lump)}`);
      bottom.unshift(`${f(x)} ${f(corner(x) + 2.5)}`);
    }
    let drips = "";
    for (let x = x0 + 40 + (sx * 53) % 50; x < x1 - 40; x += 70 + (x * 7.3 + sx * 31) % 90) {
      const l = 3 + (x * 1.7 + sx) % 5, y = CAP_TOP + 2;
      drips += `M${f(x - 4)} ${y}Q${f(x - 2.5)} ${f(y + l)} ${f(x)} ${f(y + l + 1.5)}Q${f(x + 2.5)} ${f(y + l)} ${f(x + 4)} ${y}Z`;
    }
    return [`M${top.join("L")}L${bottom.join("L")}Z`, drips];
  }
  function drawCaps() {
    const els = [...document.querySelectorAll(CAPPED)].filter(e => !e.parentElement.closest(CAPPED));
    const rects = els.map(e => [e.getBoundingClientRect(), parseFloat(getComputedStyle(e).borderTopLeftRadius) || 0])
                     .filter(([b]) => b.width > 80 && b.height > 40);
    const key = rects.map(([b, r]) => [b.x, b.y, b.width, r].map(Math.round).join(",")).join(";");
    if (key === capKey) return;
    capKey = key;
    caps.innerHTML = rects.map(([b, r], i) => {
      const w = Math.round(b.width), [cap, drips] = capPaths(w, Math.min(r, b.height / 2), 1.3 + i * 2.1);
      return `<svg class="cap" width="${w}" height="30" viewBox="0 0 ${w} 30" style="left:${f(b.x)}px;top:${f(b.y - CAP_TOP)}px">
        <path d="${drips}" fill="#dfe8f7"/><path d="${cap}" fill="url(#hCapG)"/></svg>`;
    }).join("") + `<svg width="0" height="0" style="position:absolute"><defs><linearGradient id="hCapG" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ffffff"/><stop offset=".6" stop-color="#eef3fc"/><stop offset="1" stop-color="#bccde8"/></linearGradient></defs></svg>`;
  }
  let capTimer = 0;
  const capSoon = () => { clearTimeout(capTimer); capTimer = setTimeout(drawCaps, 120); };
  new MutationObserver(capSoon).observe(document.body, { childList: true, subtree: true });
  new ResizeObserver(capSoon).observe(document.body);
  addEventListener("load", drawCaps);
  document.fonts?.ready.then(drawCaps);
  drawCaps();

  // ---- snowfall: shake the Tab5 ----
  // Soft flakes at three depths, of many sizes, and some bigger six-armed crystals turning slowly (as the fancy
  // clock's) drift down in a slow wind for SNOW_S seconds; the snow on the cards grows meanwhile and melts back
  // afterwards. Flakes all over the screen make nearly all of it change, so they move SNOW_FPS times a second, not
  // 60: the Tab5 then gets fewer, sharper full-screen pictures (it falls slowly, 15 is smooth enough).
  const SNOW_S = 20, FLAKES = 64, SNOW_FPS = 15;
  const KINDS = [   // far, middle, near discs and the crystals: radius px, speed px/s, opacity, wind, share
    { r: [.9, 1.7], v: [26, 38], a: .6, wind: .55, n: .4 }, { r: [1.7, 2.7], v: [38, 52], a: .8, wind: .8, n: .3 },
    { r: [2.8, 5.2], v: [55, 76], a: .95, wind: 1.15, n: .18 }, { r: [8, 13], v: [40, 58], a: .95, wind: .95, n: .12 }];
  const SPR = 64;   // the sprites are drawn once at this size and scaled
  const disc = (() => {   // a round flake with a faint halo
    const c = document.createElement("canvas");
    c.width = c.height = SPR;
    const g = c.getContext("2d"), m = SPR / 2, grd = g.createRadialGradient(m, m, 0, m, m, m);
    grd.addColorStop(0, "rgba(255,255,255,1)"); grd.addColorStop(.34, "rgba(255,255,255,.95)");
    grd.addColorStop(.44, "rgba(244,248,255,.3)"); grd.addColorStop(1, "rgba(244,248,255,0)");
    g.fillStyle = grd; g.fillRect(0, 0, SPR, SPR);
    return c;   // the disc's edge is at ~0.4 of the sprite's half-width
  })();
  const crystal = (() => {   // six arms, each with a pair of branches, like the snowflakes on the clock's dial
    const c = document.createElement("canvas");
    c.width = c.height = SPR;
    const g = c.getContext("2d"), m = SPR / 2, rr = SPR * .4;
    const halo = g.createRadialGradient(m, m, 0, m, m, m);
    halo.addColorStop(0, "rgba(244,248,255,.22)"); halo.addColorStop(1, "rgba(244,248,255,0)");
    g.fillStyle = halo; g.fillRect(0, 0, SPR, SPR);
    g.strokeStyle = "#fff"; g.lineCap = "round"; g.lineWidth = SPR / 15;
    g.beginPath();
    for (let j = 0; j < 6; j++) {
      const a = j * Math.PI / 3, ux = Math.sin(a), uy = -Math.cos(a), bx = m + ux * rr * .55, by = m + uy * rr * .55;
      g.moveTo(m, m); g.lineTo(m + ux * rr, m + uy * rr);
      for (const s of [-1, 1]) {
        const b = a + s * Math.PI / 3;
        g.moveTo(bx, by); g.lineTo(bx + Math.sin(b) * rr * .35, by - Math.cos(b) * rr * .35);
      }
    }
    g.stroke();
    return c;   // the arms reach 0.8 of the half-width
  })();
  let canvas = null, flakes = [], until = 0, raf = 0, last = 0;
  function newFlake(top) {
    let r = rnd(), k = 0;
    while (k < KINDS.length - 1 && r >= KINDS[k].n) r -= KINDS[k++].n;
    const K = KINDS[k], u = rnd();
    return { k, x: rnd() * (W + 200) - 100, y: top ? -30 - rnd() * 60 : rnd() * H,
             r: K.r[0] + u * u * (K.r[1] - K.r[0]),   // more small ones than big ones in each kind
             v: K.v[0] + rnd() * (K.v[1] - K.v[0]), ph: rnd() * 6.28, sw: (6 + rnd() * 14) * K.wind,
             rot: rnd() * 6.28, spin: (rnd() - .5) * .8 };
  }
  function frame(now) {
    raf = requestAnimationFrame(frame);
    if (last && now - last < 1000 / SNOW_FPS - 4) return;
    const dt = Math.min(.12, (now - (last || now)) / 1000), t = now / 1000;
    last = now;
    const g = canvas.getContext("2d");
    g.setTransform(1, 0, 0, 1, 0, 0);
    g.clearRect(0, 0, W, H);
    const falling = performance.now() < until;
    const wind = 14 + 18 * Math.sin(t * .23);
    for (let i = flakes.length - 1; i >= 0; i--) {
      const p = flakes[i], K = KINDS[p.k];
      p.y += p.v * dt;
      p.x += (wind * K.wind + p.sw * Math.cos(t * 1.3 + p.ph)) * dt;
      p.rot += p.spin * dt;
      if (p.y > H + 20) {
        if (falling) flakes[i] = newFlake(true); else flakes.splice(i, 1);
      }
    }
    flakes.sort((a, b) => a.k - b.k || a.r - b.r);   // far ones first
    for (const p of flakes) {
      g.globalAlpha = KINDS[p.k].a;
      if (p.k < 3) {
        const sz = p.r / .4 * 2;
        g.setTransform(1, 0, 0, 1, 0, 0);
        g.drawImage(disc, p.x - sz / 2, p.y - sz / 2, sz, sz);
      } else {
        const sz = p.r / .8 * 2, c = Math.cos(p.rot), s = Math.sin(p.rot);
        g.setTransform(c, s, -s, c, p.x, p.y);
        g.drawImage(crystal, -sz / 2, -sz / 2, sz, sz);
      }
    }
    if (falling && flakes.length < FLAKES) flakes.push(newFlake(true), newFlake(true));   // it starts gently
    if (!flakes.length) {
      cancelAnimationFrame(raf);
      canvas.remove(); canvas = null; raf = 0;
    }
  }
  function snow() {
    until = performance.now() + SNOW_S * 1000;
    document.documentElement.classList.add("snowing");
    clearTimeout(snow.melt);
    snow.melt = setTimeout(() => document.documentElement.classList.remove("snowing"), SNOW_S * 1000);
    if (raf) return;   // already snowing: it just goes on longer
    canvas = document.createElement("canvas");
    canvas.id = "holiday-snow";
    canvas.width = W; canvas.height = H;
    document.body.append(canvas);
    flakes = Array.from({ length: 12 }, () => newFlake(true));
    last = 0;
    raf = requestAnimationFrame(frame);
  }
  addEventListener("tab5shake", snow);
  window.holidaySnow = snow;   // for trying it in a desktop browser
})();
