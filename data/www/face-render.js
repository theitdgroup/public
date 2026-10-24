// ClockHub face renderer for the browser. Mirrors src/faces.cpp so previews
// match the watch. Usage: ClockFace.render(canvas, faceJson, {date, assetBase, data})
(function (global) {
  'use strict';
  const R = 233;
  const FONT_SIZES = [12, 14, 16, 20, 24, 28, 32, 40, 48];
  const DAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
  const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
  const ROMAN = ['XII', 'I', 'II', 'III', 'IV', 'V', 'VI', 'VII', 'VIII', 'IX', 'X', 'XI'];
  const SAMPLE = {
    battery: 76, charging: false, wifi: true, h24: true, volume: 12,
    title: 'Midnight City', artist: 'M83', station: 'SomaFM Groove Salad',
    headline: 'Scientists unveil a tiny round display that runs for weeks', headlineSource: 'BBC News',
    temp: '72°', tempHi: '78°', tempLo: '58°', weather: 'Partly cloudy', city: 'Austin',
    nextAlarm: 'Tomorrow 7:00 AM', countdown: '12 days to Vacation',
  };
  const imageCache = {};

  function color(v, fallback) {
    if (v === undefined || v === null || v === '') return fallback;
    if (v === 'none' || v === 'transparent') return 'rgba(0,0,0,0)';
    let s = String(v).replace('#', '');
    if (s.length === 3) s = s.split('').map(c => c + c).join('');
    if (s.length === 8) {
      const a = parseInt(s.slice(6), 16) / 255;
      return `rgba(${parseInt(s.slice(0, 2), 16)},${parseInt(s.slice(2, 4), 16)},${parseInt(s.slice(4, 6), 16)},${a.toFixed(3)})`;
    }
    return '#' + s;
  }

  // Same font choice as the firmware: largest built-in size <= requested, scaled above 48
  function fontPx(size) {
    size = size || 32;
    if (size > 48) return size;
    let best = FONT_SIZES[0];
    for (const f of FONT_SIZES) if (f <= size) best = f;
    return best;
  }

  function font(size) {
    return `500 ${fontPx(size)}px Montserrat, "Segoe UI", Helvetica, Arial, sans-serif`;
  }

  const two = n => (n < 10 ? '0' : '') + n;

  function expand(tmpl, d, data) {
    const h = d.getHours(), h12 = h % 12 === 0 ? 12 : h % 12;
    const ampm = h < 12 ? 'AM' : 'PM';
    const map = {
      HH: two(h), H: String(h), hh: two(h12), h: String(h12), mm: two(d.getMinutes()), ss: two(d.getSeconds()),
      AMPM: ampm, ampm: ampm.toLowerCase(),
      time: data.h24 ? `${two(h)}:${two(d.getMinutes())}` : `${h12}:${two(d.getMinutes())}`,
      hour: data.h24 ? two(h) : String(h12), 'ampm?': data.h24 ? '' : ampm,
      dddd: DAYS[d.getDay()], ddd: DAYS[d.getDay()].slice(0, 3),
      dd: two(d.getDate()), d: String(d.getDate()),
      MMMM: MONTHS[d.getMonth()], MMM: MONTHS[d.getMonth()].slice(0, 3), MM: two(d.getMonth() + 1), M: String(d.getMonth() + 1),
      yyyy: String(d.getFullYear()), yy: two(d.getFullYear() % 100),
      battery: data.battery >= 0 ? data.battery + '%' : '', charging: data.charging ? '⚡' : '', wifi: data.wifi ? '◉' : '',
      title: data.title, artist: data.artist, station: data.station,
      np: data.artist ? `${data.artist} - ${data.title}` : data.title,
      headline: data.headline, headlineSource: data.headlineSource,
      temp: data.temp, tempHi: data.tempHi, tempLo: data.tempLo, weather: data.weather, city: data.city,
      nextAlarm: data.nextAlarm, countdown: data.countdown,
      'symbol:bell': '🔔', 'symbol:audio': '♫',
    };
    return String(tmpl).replace(/\{([^}]+)\}/g, (m, k) => (k in map ? map[k] : m));
  }

  function arcFraction(value, d, data, secF) {
    switch (value) {
      case 'seconds': return secF / 60;
      case 'minutes': return (d.getMinutes() * 60 + d.getSeconds()) / 3600;
      case 'hours': return ((d.getHours() % 12) * 60 + d.getMinutes()) / 720;
      case 'battery': return Math.max(0, data.battery) / 100;
      case 'dayOfWeek': return (((d.getDay() + 6) % 7) + 1) / 7;
      case 'dayOfMonth': return d.getDate() / 31;
      case 'month': return (d.getMonth() + 1) / 12;
      case 'dayOfYear': {
        const start = new Date(d.getFullYear(), 0, 0);
        return Math.floor((d - start) / 86400000) / 366;
      }
      case 'volume': return data.volume / 21;
      default: return 1;
    }
  }

  function line(ctx, x1, y1, x2, y2, w, col, round) {
    ctx.beginPath();
    ctx.lineWidth = w;
    ctx.strokeStyle = col;
    ctx.lineCap = round ? 'round' : 'butt';
    ctx.moveTo(x1, y1);
    ctx.lineTo(x2, y2);
    ctx.stroke();
  }

  // LVGL binary images (.bin: 12-byte header + RGB565 or ARGB8888 pixels) -> canvas
  const binCache = {};
  function loadBin(src, onload) {
    const hit = binCache[src];
    if (hit) return hit.canvas || null;
    const entry = binCache[src] = { canvas: null };
    fetch(src).then(r => r.arrayBuffer()).then(buf => {
      const dv = new DataView(buf);
      const cf = dv.getUint8(1), w = dv.getUint16(4, true), h = dv.getUint16(6, true), stride = dv.getUint16(8, true);
      const px = new Uint8Array(buf, 12);
      const c = document.createElement('canvas');
      c.width = w; c.height = h;
      const ctx = c.getContext('2d');
      const img = ctx.createImageData(w, h);
      for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
        const o = (y * w + x) * 4;
        if (cf === 0x0E) {  // A8: alpha only, tinted when drawn
          img.data[o] = img.data[o + 1] = img.data[o + 2] = 255; img.data[o + 3] = px[y * stride + x];
        } else if (cf === 0x12 || cf === 0x14) {
          // RGB565, or RGB565A8: the colour plane followed by an alpha plane
          const v = px[y * stride + x * 2] | (px[y * stride + x * 2 + 1] << 8);
          img.data[o] = (v >> 11) << 3; img.data[o + 1] = ((v >> 5) & 63) << 2; img.data[o + 2] = (v & 31) << 3;
          img.data[o + 3] = cf === 0x14 ? px[stride * h + y * (stride >> 1) + x] : 255;
        } else {
          const i = y * stride + x * 4;
          img.data[o] = px[i + 2]; img.data[o + 1] = px[i + 1]; img.data[o + 2] = px[i]; img.data[o + 3] = px[i + 3];
        }
      }
      ctx.putImageData(img, 0, 0);
      entry.canvas = c;
      if (onload) onload();
    }).catch(() => {});
    return null;
  }

  function loadImage(src, onload) {
    if (/\.bin$/i.test(src)) return loadBin(src, onload);
    if (imageCache[src]) return imageCache[src].complete ? imageCache[src] : null;
    const img = new Image();
    img.crossOrigin = 'anonymous';
    img.onload = onload;
    img.src = src;
    imageCache[src] = img;
    return null;
  }

  function render(canvas, face, opts) {
    opts = opts || {};
    const data = Object.assign({}, SAMPLE, opts.data || {});
    const d = opts.date || new Date();
    const secF = d.getSeconds() + d.getMilliseconds() / 1000;
    const ctx = canvas.getContext('2d');
    const scale = canvas.width / (R * 2);
    ctx.save();
    ctx.setTransform(scale, 0, 0, scale, 0, 0);
    ctx.clearRect(0, 0, R * 2, R * 2);

    // Round screen
    ctx.beginPath();
    ctx.arc(R, R, R, 0, Math.PI * 2);
    ctx.clip();

    const bg = (face && face.background) || {};
    if (bg.gradient && bg.gradient !== 'none' && bg.color2) {
      const g = bg.gradient === 'horizontal' ? ctx.createLinearGradient(0, 0, R * 2, 0) : ctx.createLinearGradient(0, 0, 0, R * 2);
      g.addColorStop(0, color(bg.color, '#000'));
      g.addColorStop(1, color(bg.color2, '#000'));
      ctx.fillStyle = g;
    } else {
      ctx.fillStyle = color(bg.color, '#000');
    }
    ctx.fillRect(0, 0, R * 2, R * 2);
    ctx.translate(R, R);

    const elements = (face && face.elements) || [];
    for (const e of elements) {
      ctx.save();
      try { drawElement(ctx, e, d, data, secF, opts); } catch (err) { /* skip bad element */ }
      ctx.restore();
    }
    ctx.restore();
  }

  function drawElement(ctx, e, d, data, secF, opts) {
    switch (e.type) {
      case 'ticks': {
        const count = e.count ?? 60, every = e.majorEvery ?? 5, r = e.radius ?? 225;
        const len = e.length ?? 10, mlen = e.majorLength ?? len * 2;
        const w = e.width ?? 2, mw = e.majorWidth ?? w * 2;
        const c = color(e.color, '#666'), mc = color(e.majorColor ?? e.color, '#fff');
        for (let i = 0; i < count && count <= 360; i++) {
          const major = every > 0 && i % every === 0;
          if (major && e.skipMajor) continue;
          const l = major ? mlen : len;
          if (l <= 0) continue;
          const a = (i * 2 * Math.PI) / count, s = Math.sin(a), co = -Math.cos(a);
          line(ctx, r * s, r * co, (r - l) * s, (r - l) * co, major ? mw : w, major ? mc : c, !!e.round);
        }
        break;
      }
      case 'numerals': {
        const style = e.style || 'arabic', r = e.radius ?? 185;
        ctx.font = font(e.size ?? 32);
        ctx.fillStyle = color(e.color, '#fff');
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        for (let i = 0; i < 12; i++) {
          if (style.startsWith('quarters') && i % 3) continue;
          const a = (i * Math.PI) / 6;
          ctx.fillText(style.endsWith('roman') ? ROMAN[i] : String(i === 0 ? 12 : i), r * Math.sin(a), -r * Math.cos(a));
        }
        break;
      }
      case 'hand': {
        const kind = e.hand || 'minute';
        let frac;
        if (kind === 'hour') frac = ((d.getHours() % 12) + d.getMinutes() / 60 + d.getSeconds() / 3600) / 12;
        else if (kind === 'second') frac = (e.smooth ? secF : d.getSeconds()) / 60;
        else frac = (d.getMinutes() + d.getSeconds() / 60) / 60;
        const a = frac * 2 * Math.PI, s = Math.sin(a), c = -Math.cos(a);
        const len = e.length ?? (kind === 'hour' ? 120 : 180), tail = e.tail ?? 0;
        const w = e.width ?? (kind === 'second' ? 3 : 8);
        line(ctx, -tail * s, -tail * c, len * s, len * c, w, color(e.color, kind === 'second' ? '#ff3b30' : '#fff'), e.round !== false);
        break;
      }
      case 'circle': {
        const r = e.radius ?? 8, bw = e.borderWidth ?? 0;
        ctx.beginPath();
        ctx.arc(e.x ?? 0, e.y ?? 0, r, 0, Math.PI * 2);
        ctx.fillStyle = color(e.color, '#fff');
        ctx.fill();
        if (bw > 0) {
          ctx.beginPath();
          ctx.arc(e.x ?? 0, e.y ?? 0, Math.max(0, r - bw / 2), 0, Math.PI * 2);
          ctx.lineWidth = bw;
          ctx.strokeStyle = color(e.borderColor, '#fff');
          ctx.stroke();
        }
        break;
      }
      case 'rect': {
        const w = e.w ?? 100, h = e.h ?? 40, x = (e.x ?? 0) - w / 2, y = (e.y ?? 0) - h / 2;
        const rad = Math.min(e.radius ?? 0, w / 2, h / 2);
        ctx.beginPath();
        if (ctx.roundRect) ctx.roundRect(x, y, w, h, rad); else ctx.rect(x, y, w, h);
        ctx.fillStyle = color(e.color, '#333');
        ctx.fill();
        if ((e.borderWidth ?? 0) > 0) {
          ctx.lineWidth = e.borderWidth;
          ctx.strokeStyle = color(e.borderColor, '#fff');
          ctx.stroke();
        }
        break;
      }
      case 'text': {
        let txt = expand(e.text ?? '{time}', d, data);
        if (e.upper) txt = txt.toUpperCase();
        const align = e.align || 'center';
        const size = e.size ?? 32;
        ctx.font = font(size);
        ctx.fillStyle = color(e.color, '#fff');
        ctx.textBaseline = 'middle';
        if (e.letterSpacing && 'letterSpacing' in ctx) ctx.letterSpacing = e.letterSpacing + 'px';
        let x = e.x ?? 0;
        const y = e.y ?? 0;
        ctx.textAlign = align;
        // Device scales the 48 px font for bigger sizes; canvas can render natively
        const width = e.width ?? 0;
        if (width > 0 && e.wrap) {
          const lines = [];
          let cur = '';
          for (const word of txt.split(' ')) {
            const t = cur ? cur + ' ' + word : word;
            if (ctx.measureText(t).width > width && cur) { lines.push(cur); cur = word; } else cur = t;
          }
          if (cur) lines.push(cur);
          const lh = fontPx(size) * 1.2;
          const top = y - ((lines.length - 1) * lh) / 2;
          lines.forEach((l, i) => ctx.fillText(l, x, top + i * lh));
          break;
        }
        if (width > 0) {
          let bx = align === 'left' ? x : align === 'right' ? x - width : x - width / 2;
          ctx.beginPath();
          ctx.rect(bx, y - size, width, size * 2);
          ctx.clip();
          if (!e.marquee && !e.wrap && ctx.measureText(txt).width > width) {
            while (txt.length > 1 && ctx.measureText(txt + '...').width > width) txt = txt.slice(0, -1);
            txt += '...';
          }
          if (e.marquee && ctx.measureText(txt).width > width) { ctx.textAlign = 'left'; x = bx; }
        }
        ctx.fillText(txt, x, y);
        break;
      }
      case 'arc': {
        const r = e.radius ?? 225, w = e.width ?? 8;
        const start = e.startAngle ?? 0, sweep = e.sweep ?? 360;
        const cr = r - w / 2;
        const a0 = ((start - 90) * Math.PI) / 180;
        const x = e.x ?? 0, y = e.y ?? 0;
        ctx.lineWidth = w;
        ctx.lineCap = e.round === false ? 'butt' : 'round';
        if (e.bgColor !== undefined) {
          ctx.beginPath();
          ctx.strokeStyle = color(e.bgColor, '#222');
          ctx.arc(x, y, cr, a0, a0 + (sweep * Math.PI) / 180);
          ctx.stroke();
        }
        const frac = arcFraction(e.value || 'none', d, data, secF);
        if (frac > 0.001) {
          ctx.beginPath();
          ctx.strokeStyle = color(e.color, '#ff3b30');
          ctx.arc(x, y, cr, a0, a0 + (sweep * frac * Math.PI) / 180);
          ctx.stroke();
        }
        break;
      }
      case 'plate': {
        const base = opts.assetBase || '/faces/assets/';
        let img = e.frames && e.frames.length ? loadImage(base + e.frames[0], opts.onAssetLoad) : null;
        if (img && e.color) {  // tint alpha-only frames, as the watch does
          const t = document.createElement('canvas'); t.width = img.width; t.height = img.height;
          const tc = t.getContext('2d'); tc.fillStyle = color(e.color, '#ffffff'); tc.fillRect(0, 0, t.width, t.height);
          tc.globalCompositeOperation = 'destination-in'; tc.drawImage(img, 0, 0); img = t;
        }
        if (img) ctx.drawImage(img, (e.x ?? 0) - img.width / 2, (e.y ?? 0) - img.height / 2);
        break;
      }
      case 'imagehand': {
        const base = opts.assetBase || '/faces/assets/';
        const img = e.src ? loadBin(base + e.src, opts.onAssetLoad) : null;
        if (!img) break;
        const kind = e.hand || 'minute';
        let frac;
        if (kind === 'hour') frac = ((d.getHours() % 12) + d.getMinutes() / 60) / 12;
        else if (kind === 'second') frac = (e.smooth ? secF : d.getSeconds()) / 60;
        else frac = (d.getMinutes() + d.getSeconds() / 60) / 60;
        ctx.translate(e.x ?? 0, e.y ?? 0);
        ctx.rotate(frac * 2 * Math.PI);
        ctx.globalAlpha = (e.opacity ?? 255) / 255;
        ctx.drawImage(img, -(e.pivotX ?? img.width / 2), -(e.pivotY ?? img.height));
        break;
      }
      case 'weathericon': {
        // Preview only: the watch draws its own weather icon here
        const px = e.size === 'small' ? 36 : 80;
        ctx.font = `${px * 0.8}px "Segoe UI Emoji", "Apple Color Emoji", sans-serif`;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText('⛅', e.x ?? 0, e.y ?? 0);
        break;
      }
      case 'image': {
        if (!e.src) break;
        const base = opts.assetBase || '/faces/assets/';
        const img = loadImage(base + e.src, opts.onAssetLoad);
        if (!img) break;
        const s = (e.scale ?? 256) / 256;
        ctx.globalAlpha = (e.opacity ?? 255) / 255;
        ctx.drawImage(img, (e.x ?? 0) - (img.width * s) / 2, (e.y ?? 0) - (img.height * s) / 2, img.width * s, img.height * s);
        break;
      }
    }
  }

  // Animate a canvas; returns a stop() function
  function animate(canvas, getFace, opts) {
    let stopped = false;
    function frame() {
      if (stopped) return;
      const face = getFace();
      if (face) render(canvas, face, Object.assign({}, opts, { date: new Date() }));
      setTimeout(() => requestAnimationFrame(frame), 100);
    }
    frame();
    return () => { stopped = true; };
  }

  global.ClockFace = { render, animate, expand };
})(window);
