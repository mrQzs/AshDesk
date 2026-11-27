/* 液态玻璃：按 docs/UI设计.md §8 的 SDF + 斜面法线 + Snell 折射生成位移图，
 * 交给 SVG feDisplacementMap 作为 backdrop-filter。仅用于 Chrome 原型预览，
 * 不代表 Qt 实现的性能。 */
window.Glass = (() => {
  'use strict';

  const ROLE = {
    nav:    { bevel: 14, h0: 8, blur: 9,  sat: 1.7 },
    action: { bevel: 12, h0: 7, blur: 7,  sat: 1.7 },
    pop:    { bevel: 10, h0: 5, blur: 14, sat: 1.35 },
  };
  const ETA = 1 / 1.5, EPS = 0.08, MAX_OFF = 16, DISPERSION = 0.01, FROST_END = 0.85;
  const SCALE = 2 * MAX_OFF;

  // backdrop-filter:url() 目前只有 Chromium 渲染；其他浏览器停在 Q1。
  const supported = !!navigator.userAgentData?.brands?.some(b => /Chromium/.test(b.brand));

  const mapCache = new Map();
  const items = new Map();
  const pending = new Set();
  let defs, on = false, q = 2, seq = 0, raf = 0, genMs = 0;

  const ro = new ResizeObserver(entries => {
    for (const e of entries) pending.add(e.target);
    schedule();
  });

  function init(els) {
    defs = document.getElementById('lg-defs');
    for (const el of els) {
      items.set(el, { id: `lg-${++seq}`, key: '' });
      ro.observe(el);
    }
  }

  function configure(opts) {
    const changed = opts.on !== on || opts.q !== q;
    on = opts.on && supported;
    q = opts.q;
    document.documentElement.classList.toggle('lg-svg', on);
    if (changed) {
      for (const el of items.keys()) pending.add(el);
      schedule();
    }
  }

  function schedule() {
    if (!raf) raf = requestAnimationFrame(flush);
  }

  function flush() {
    raf = 0;
    for (const el of pending) update(el);
    pending.clear();
  }

  function update(el) {
    if (!on) return;
    const w = el.offsetWidth, h = el.offsetHeight;
    if (!w || !h) return;
    const it = items.get(el);
    const roleName = el.dataset.role in ROLE ? el.dataset.role : 'action';
    const r = Math.min(parseFloat(getComputedStyle(el).borderTopLeftRadius) || 0, w / 2, h / 2);
    const dispersion = q >= 3;
    const key = `${w}x${h}|${r}|${roleName}|${dispersion}`;
    if (key === it.key) return;
    it.key = key;
    const role = ROLE[roleName];
    const m = maps(w, h, r, role, dispersion);
    defs.querySelector(`#${it.id}`)?.remove();
    defs.appendChild(filter(it.id, w, h, m, role, dispersion));
    el.style.setProperty('--lg', `url(#${it.id})`);
  }

  function maps(w, h, r, role, dispersion) {
    const key = `${w}x${h}|${r}|${role.bevel}|${role.h0}|${dispersion}`;
    const hit = mapCache.get(key);
    if (hit) return hit;

    const t0 = performance.now();
    // 高折射率的蓝光偏折更大：ηB = η0(1−δ)，ηR = η0(1+δ)。
    const etas = dispersion ? [ETA * (1 + DISPERSION), ETA, ETA * (1 - DISPERSION)] : [ETA];
    const disp = etas.map(() => new ImageData(w, h));
    const frost = new ImageData(w, h);
    const bx = w / 2, by = h / 2;
    const bevel = Math.min(role.bevel, bx, by);

    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const i = (y * w + x) * 4;
        const px = x + 0.5 - bx, py = y + 0.5 - by;
        const g = sdf(px, py, bx, by, r);
        const inside = g.phi <= 0;
        const t = clamp(-g.phi / bevel, 0, 1), s = 1 - t;
        const v = 2 * t - t * t + EPS * EPS, sv = Math.sqrt(v);
        const z = bevel * (sv - EPS);
        let nx = s * g.gx, ny = s * g.gy, nz = sv;
        const nl = Math.hypot(nx, ny, nz);
        nx /= nl; ny /= nl; nz /= nl;

        for (let c = 0; c < etas.length; c++) {
          const d = disp[c].data;
          const [dx, dy] = inside ? refract(nx, ny, nz, etas[c], role.h0 + z) : [0, 0];
          d[i] = Math.round((0.5 + dx / SCALE) * 255);
          d[i + 1] = Math.round((0.5 + dy / SCALE) * 255);
          d[i + 2] = 128;
          d[i + 3] = 255;
        }
        const f = frost.data;
        f[i] = f[i + 1] = f[i + 2] = 255;
        f[i + 3] = inside ? Math.round(smoothstep(0, FROST_END, t) * 255) : 0;
      }
    }

    const m = { disp: disp.map(png), frost: png(frost) };
    genMs += performance.now() - t0;
    mapCache.set(key, m);
    return m;
  }

  // 圆角矩形 SDF 与外法向梯度；len=0 时取固定分支，避免 0/0。
  function sdf(px, py, bx, by, r) {
    const qx = Math.abs(px) - bx + r, qy = Math.abs(py) - by + r;
    const ox = Math.max(qx, 0), oy = Math.max(qy, 0);
    const len = Math.hypot(ox, oy);
    const phi = len + Math.min(Math.max(qx, qy), 0) - r;
    const sx = px < 0 ? -1 : 1, sy = py < 0 ? -1 : 1;
    if (len > 0) return { phi, gx: sx * ox / len, gy: sy * oy / len };
    return qx > qy ? { phi, gx: sx, gy: 0 } : { phi, gx: 0, gy: sy };
  }

  function refract(nx, ny, nz, eta, hgt) {
    const kr = Math.max(0, 1 - eta * eta * (1 - nz * nz));
    const a = eta * nz - Math.sqrt(kr);
    const tz = -eta + a * nz;
    const k = hgt / Math.max(-tz, 1e-6);
    let dx = k * a * nx, dy = k * a * ny;
    const len = Math.hypot(dx, dy);
    if (len > MAX_OFF) { dx *= MAX_OFF / len; dy *= MAX_OFF / len; }
    return [dx, dy];
  }

  function filter(id, w, h, m, role, dispersion) {
    const img = (href, res) =>
      `<feImage href="${href}" x="0" y="0" width="${w}" height="${h}" preserveAspectRatio="none" result="${res}"/>`;
    const shift = (input, map, res) =>
      `<feDisplacementMap in="${input}" in2="${map}" scale="${SCALE}" xChannelSelector="R" yChannelSelector="G" result="${res}"/>`;
    const keep = (input, row, res) => {
      const mtx = ['0 0 0 0 0', '0 0 0 0 0', '0 0 0 0 0'];
      mtx[row] = ['1 0 0 0 0', '0 1 0 0 0', '0 0 1 0 0'][row];
      return `<feColorMatrix in="${input}" type="matrix" values="${mtx.join(' ')} 0 0 0 1 0" result="${res}"/>`;
    };
    const sharp = dispersion
      ? img(m.disp[0], 'mR') + img(m.disp[2], 'mB')
        + shift('SourceGraphic', 'mR', 'sR') + shift('SourceGraphic', 'm', 'sG') + shift('SourceGraphic', 'mB', 'sB')
        + keep('sR', 0, 'cR') + keep('sG', 1, 'cG') + keep('sB', 2, 'cB')
        + '<feComposite in="cR" in2="cG" operator="arithmetic" k2="1" k3="1" result="cRG"/>'
        + '<feComposite in="cRG" in2="cB" operator="arithmetic" k2="1" k3="1" result="sharp"/>'
      : shift('SourceGraphic', 'm', 'sharp');

    // body = mix(sharp, blurred, frostWeight)；模糊先于位移，与 §8 的金字塔取样顺序一致。
    return parse(`<filter id="${id}" x="0" y="0" width="100%" height="100%" color-interpolation-filters="sRGB">
      ${img(m.disp[dispersion ? 1 : 0], 'm')}${img(m.frost, 'f')}
      <feGaussianBlur in="SourceGraphic" stdDeviation="${role.blur}" edgeMode="duplicate" result="b"/>
      ${shift('b', 'm', 'db')}
      ${sharp}
      <feComposite in="db" in2="f" operator="in" result="fb"/>
      <feComposite in="fb" in2="sharp" operator="over" result="mix"/>
      <feColorMatrix in="mix" type="saturate" values="${role.sat}"/>
    </filter>`);
  }

  function parse(markup) {
    const doc = new DOMParser().parseFromString(
      `<svg xmlns="http://www.w3.org/2000/svg">${markup}</svg>`, 'image/svg+xml');
    return document.importNode(doc.documentElement.firstElementChild, true);
  }

  function png(img) {
    const c = document.createElement('canvas');
    c.width = img.width;
    c.height = img.height;
    c.getContext('2d').putImageData(img, 0, 0);
    return c.toDataURL('image/png');
  }

  function clamp(x, a, b) { return Math.min(b, Math.max(a, x)); }
  function smoothstep(a, b, x) { const t = clamp((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }

  return {
    init,
    configure,
    supported,
    stats: () => ({ maps: mapCache.size, ms: genMs }),
  };
})();
