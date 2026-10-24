// Converts an .orb theme (container "ORBTHM01") into a ClockHub face.
// Only the clock part is used: dial plate (+ animation frames), image hands with
// shadows, glass overlay and the two optional date/time texts. Images are turned
// into LVGL binary images (.bin) here in the browser so the watch never decodes PNGs.
(function (global) {
  'use strict';
  const CF_RGB565 = 0x12, CF_ARGB8888 = 0x10, MAGIC = 0x19;

  function parseOrb(buf) {
    const dv = new DataView(buf);
    const magic = new TextDecoder().decode(new Uint8Array(buf, 0, 8));
    if (magic !== 'ORBTHM01') throw new Error('Not an .orb theme file');
    let p = 8;
    const str = n => { const s = new TextDecoder().decode(new Uint8Array(buf, p, n)); p += n; return s; };
    const idLen = dv.getUint16(p, true); p += 2;
    const id = str(idLen);
    const count = dv.getUint16(p, true); p += 2;
    const files = {};
    for (let i = 0; i < count; i++) {
      const nameLen = dv.getUint16(p, true); p += 2;
      const name = str(nameLen);
      const size = dv.getUint32(p, true); p += 4;
      if (p + size > buf.byteLength) throw new Error('The .orb file is truncated');
      files[name] = new Uint8Array(buf, p, size);
      p += size;
    }
    return { id, files };
  }

  const json = bytes => JSON.parse(new TextDecoder().decode(bytes));
  const hex = n => '#' + (n >>> 0 & 0xffffff).toString(16).padStart(6, '0').toUpperCase();

  async function pixels(bytes) {
    const bmp = await createImageBitmap(new Blob([bytes], { type: 'image/png' }));
    const c = document.createElement('canvas');
    c.width = bmp.width; c.height = bmp.height;
    const ctx = c.getContext('2d', { willReadFrequently: true });
    ctx.drawImage(bmp, 0, 0);
    return ctx.getImageData(0, 0, c.width, c.height);
  }

  function alphaBox(img) {
    const { width: w, height: h, data } = img;
    let x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      if (data[(y * w + x) * 4 + 3] > 2) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    }
    return x1 < 0 ? null : { x: x0, y: y0, w: x1 - x0 + 1, h: y1 - y0 + 1 };
  }

  function header(cf, w, h, stride) {
    const b = new Uint8Array(12);
    const dv = new DataView(b.buffer);
    b[0] = MAGIC; b[1] = cf;
    dv.setUint16(4, w, true); dv.setUint16(6, h, true); dv.setUint16(8, stride, true);
    return b;
  }

  // Opaque picture -> RGB565, composited over the background colour
  function toRGB565(img, bg) {
    const { width: w, height: h, data } = img;
    const out = new Uint8Array(12 + w * h * 2);
    out.set(header(CF_RGB565, w, h, w * 2));
    const br = bg >> 16 & 255, bgc = bg >> 8 & 255, bb = bg & 255;
    for (let i = 0, o = 12; i < w * h; i++, o += 2) {
      const a = data[i * 4 + 3] / 255;
      const r = data[i * 4] * a + br * (1 - a), g = data[i * 4 + 1] * a + bgc * (1 - a), b = data[i * 4 + 2] * a + bb * (1 - a);
      const v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
      out[o] = v & 255; out[o + 1] = v >> 8;
    }
    return out;
  }

  // Transparent picture -> ARGB8888 (stored B,G,R,A), cropped to `box`
  function toARGB(img, box) {
    const { width: w, data } = img;
    const out = new Uint8Array(12 + box.w * box.h * 4);
    out.set(header(CF_ARGB8888, box.w, box.h, box.w * 4));
    let o = 12;
    for (let y = box.y; y < box.y + box.h; y++) for (let x = box.x; x < box.x + box.w; x++) {
      const i = (y * w + x) * 4;
      out[o++] = data[i + 2]; out[o++] = data[i + 1]; out[o++] = data[i]; out[o++] = data[i + 3];
    }
    return out;
  }

  const STRFTIME = { H: '{HH}', M: '{mm}', S: '{ss}', I: '{hh}', l: '{h}', k: '{H}', p: '{AMPM}', P: '{ampm}', a: '{ddd}', A: '{dddd}',
    b: '{MMM}', h: '{MMM}', B: '{MMMM}', d: '{dd}', e: '{d}', m: '{MM}', Y: '{yyyy}', y: '{yy}', R: '{HH}:{mm}', T: '{HH}:{mm}:{ss}', '%': '%' };
  const fmtTokens = f => String(f || '').replace(/%([a-zA-Z%])/g, (m, k) => STRFTIME[k] ?? '');

  // Returns {face, files: [{name, bytes}]} ready to upload
  async function convert(arrayBuffer, onProgress) {
    const { id: rawId, files } = parseOrb(arrayBuffer);
    if (!files['theme.json'] || !files['clock_style.json']) throw new Error('This .orb has no clock face');
    const theme = json(files['theme.json']);
    const st = json(files['clock_style.json']);
    const id = (rawId || theme.name || 'orb').toLowerCase().replace(/[^a-z0-9_-]+/g, '-').slice(0, 30);
    const bg = st.bg ?? 0;
    const out = [];
    const asset = (name, bytes) => { const n = `${id}_${name}.bin`; out.push({ name: n, bytes }); return n; };
    const progress = msg => onProgress && onProgress(msg);

    // Dial plate and its animation frames
    const frameNames = ['clock_plate.png'];
    const nFrames = st.bgAnim && st.bgAnim.frames || 0;
    for (let i = 1; i <= nFrames; i++) { const n = `clock_plate_${String(i).padStart(2, '0')}.png`; if (files[n]) frameNames.push(n); }
    const frames = [];
    for (let i = 0; i < frameNames.length; i++) {
      if (!files[frameNames[i]]) continue;
      progress(`Converting dial ${i + 1} of ${frameNames.length}...`);
      frames.push(asset(`plate${i}`, toRGB565(await pixels(files[frameNames[i]]), bg)));
    }
    const elements = [];
    if (frames.length) elements.push({ type: 'plate', frames, fps: st.bgAnim?.fps || 8, everySec: st.bgAnim?.everySec || 60 });

    const texts = [];
    for (const key of ['text1', 'text2']) {
      const t = st[key];
      if (!t || !t.show) continue;
      texts.push({ type: 'text', text: fmtTokens(t.fmt), x: (t.x ?? 233) - 233, y: (t.y ?? 233) - 233, size: t.size || 24,
        color: hex(t.color ?? 0xffffff), align: ['left', 'center', 'right'][t.align ?? 1] || 'center', upper: !!t.upper });
    }
    if (!st.textOverHands) elements.push(...texts);

    // Hands: index 0 hour, 1 minute, 2 second, 3/4 static decorations; order lists bottom -> top
    const H = st.hands || {};
    const kinds = ['hour', 'minute', 'second'];
    const order = (H.order || [3, 4, 2, 1, 0]).filter(i => i < 3);
    const handEls = [], shadowEls = [];
    for (const i of order) {
      const kind = kinds[i], cfg = H[kind];
      if (!cfg || cfg.show === false || !files[`clock_hand_${kind}.png`]) continue;
      progress(`Converting ${kind} hand...`);
      const cx = (cfg.centerX ?? 233) - 233, cy = (cfg.centerY ?? 233) - 233;
      const make = async (file, dx, dy) => {
        const img = await pixels(files[file]);
        const box = alphaBox(img);
        if (!box) return null;
        return { type: 'imagehand', hand: kind, src: asset(file.replace('.png', '').replace('clock_', ''), toARGB(img, box)),
          pivotX: (cfg.pivotX ?? img.width / 2) - box.x, pivotY: (cfg.pivotY ?? img.height) - box.y, x: cx + dx, y: cy + dy,
          smooth: kind === 'second' && !!st.secondSweep };
      };
      const hand = await make(`clock_hand_${kind}.png`, 0, 0);
      if (hand) handEls.push(hand);
      if (H.shadow?.on && files[`clock_shadow_${kind}.png`]) {
        const sh = await make(`clock_shadow_${kind}.png`, H.shadow.dx || 0, H.shadow.dy || 0);
        if (sh) shadowEls.push(sh);
      }
    }
    elements.push(...shadowEls, ...handEls);
    if (st.textOverHands) elements.push(...texts);

    if (files['clock_overlay.png']) {
      progress('Converting glass overlay...');
      const img = await pixels(files['clock_overlay.png']);
      const box = alphaBox(img);
      if (box) elements.push({ type: 'image', src: asset('overlay', toARGB(img, box)),
        x: Math.round(box.x + box.w / 2 - 233), y: Math.round(box.y + box.h / 2 - 233) });
    }

    const face = {
      id, version: 1, name: theme.name || id, author: theme.author || '',
      description: `Imported from .orb${theme.license ? ' (' + theme.license + ')' : ''}`,
      source: 'orb', background: { color: hex(bg) }, elements,
    };
    return { face, files: out };
  }

  global.OrbImport = { convert, parseOrb };
})(window);
