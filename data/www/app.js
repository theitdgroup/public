'use strict';
const $ = id => document.getElementById(id);
const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
let cfg = null, status = null, installedFaces = [];

// ---------- API ----------
async function api(path, opts = {}) {
  const o = { method: opts.method || 'GET', headers: { 'X-ClockHub': '1' } };
  if (opts.json !== undefined) { o.method = opts.method || 'POST'; o.body = JSON.stringify(opts.json); o.headers['Content-Type'] = 'text/plain'; }
  const r = await fetch(path, o);
  let data = null;
  try { data = await r.json(); } catch (e) { /* not JSON */ }
  if (!r.ok) throw new Error((data && data.error) || r.statusText);
  return data;
}
const saveCfg = part => api('/api/config', { json: part });
const player = (cmd, arg, extra) => api('/api/player', { json: Object.assign({ cmd, arg }, extra || {}) }).catch(e => toast(e.message));

function upload(path, file, field, onProgress) {
  return new Promise((resolve, reject) => {
    const fd = new FormData();
    fd.append(field || 'file', file, file.name);
    const x = new XMLHttpRequest();
    x.open('POST', path);
    x.setRequestHeader('X-ClockHub', '1');
    x.upload.onprogress = e => onProgress && e.lengthComputable && onProgress(e.loaded / e.total);
    x.onload = () => {
      let d = {};
      try { d = JSON.parse(x.responseText); } catch (e) { /* ignore */ }
      x.status === 200 ? resolve(d) : reject(new Error(d.error || x.statusText));
    };
    x.onerror = () => reject(new Error('Upload failed'));
    x.send(fd);
  });
}

let toastTimer;
function toast(msg) {
  const t = $('toast');
  t.textContent = msg;
  t.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove('show'), 2400);
}
function flash(id, msg) { const e = $(id); e.textContent = msg; setTimeout(() => (e.textContent = ''), 2500); }
function download(name, text) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([text], { type: 'application/json' }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

// ---------- tabs ----------
function showTab(name) {
  document.querySelectorAll('nav button').forEach(b => b.classList.toggle('on', b.dataset.tab === name));
  document.querySelectorAll('section').forEach(s => s.classList.toggle('on', s.id === name));
  location.hash = name;
  if (name === 'faces') loadFaces();
  if (name === 'gallery') loadGallery();
  if (name === 'designer') initDesigner();
  if (name === 'news') loadNews();
  if (name === 'photos') loadPhotos();
  if (name === 'alarms') loadAlarmsTab();
  if (name === 'music') { renderStations(); loadTracks(); }
}
$('nav').addEventListener('click', e => e.target.dataset.tab && showTab(e.target.dataset.tab));

// ---------- status ----------
function fmtTime(ms) { const s = Math.floor(ms / 1000); return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`; }

async function pollStatus() {
  try {
    status = await api('/api/status');
    const n = status.net, p = status.player;
    $('pill').textContent = n.connected ? `${n.ssid} · ${n.ip}` : n.ap ? 'Setup hotspot' : 'Offline';
    document.querySelectorAll('#srcSeg button').forEach(b => b.classList.toggle('on', b.dataset.src === p.source));
    if (document.activeElement !== $('vol')) $('vol').value = p.volume;
    $('np').classList.toggle('playing', p.playing);
    $('playBtn').innerHTML = p.playing ? '&#10074;&#10074;' : '&#9654;';
    let title = p.title, artist = p.artist, info = '';
    if (p.source === 'none') { title = 'Choose a source'; artist = 'Radio or SD card'; }
    else if (p.busy) { title = 'Connecting...'; artist = p.station; }
    else if (p.source === 'radio') { if (!title) title = p.station || 'Radio'; else info = p.station; }
    else if (p.source === 'sd' && p.trackIdx >= 0) info = `Track ${p.trackIdx + 1} of ${p.trackCount}` + (p.durMs ? ` · ${fmtTime(p.posMs)} / ${fmtTime(p.durMs)}` : '');
    if (p.error) info = '⚠ ' + p.error;
    $('npTitle').textContent = title || 'Not playing';
    $('npArtist').textContent = artist || '';
    $('npInfo').textContent = info;
    $('radioCard').style.display = p.source === 'radio' || p.source === 'none' ? '' : 'none';
    $('sdCard').style.display = p.source === 'sd' ? '' : 'none';
    $('sdInfo').textContent = p.usbDrive ? 'The card is open on your computer as a USB drive.' : p.sd ? `${p.trackCount} tracks found` : 'No SD card detected.';
    $('usbOn').textContent = p.usbDrive ? 'Give card back to watch' : 'Open as USB drive';
    $('davUrl').textContent = `\\\\${n.ip}@80\\dav`;
    document.querySelectorAll('#stations .item').forEach((el, i) => el.classList.toggle('cur', p.source === 'radio' && i === p.stationIdx));
    if (document.querySelector('#system.on')) renderSystem();
  } catch (e) {
    $('pill').textContent = 'Watch not reachable';
  }
}

// ---------- music ----------
$('srcSeg').addEventListener('click', e => e.target.dataset.src && player('source', e.target.dataset.src).then(() => setTimeout(() => { pollStatus(); loadTracks(); }, 300)));
document.querySelectorAll('.ctl button').forEach(b => b.addEventListener('click', () => player(b.dataset.cmd).then(() => setTimeout(pollStatus, 400))));
$('vol').addEventListener('change', e => player('volume', +e.target.value));

function renderStations() {
  if (!cfg) return;
  $('stations').innerHTML = cfg.stations.map((s, i) => `
    <div class="item"><button class="b sm" data-play="${i}">&#9654;</button>
      <div class="grow"><div>${esc(s.name)}</div><div class="muted">${esc(s.url)}</div></div>
      <button class="b sm" data-up="${i}" title="Move up">&#8593;</button>
      <button class="b sm d" data-del="${i}" title="Remove">&#10005;</button></div>`).join('') || '<div class="muted item">No stations yet.</div>';
}
$('stations').addEventListener('click', async e => {
  const t = e.target.closest('button');
  if (!t) return;
  if (t.dataset.play) return player('station', +t.dataset.play);
  if (t.dataset.del) cfg.stations.splice(+t.dataset.del, 1);
  if (t.dataset.up && +t.dataset.up > 0) { const i = +t.dataset.up; [cfg.stations[i - 1], cfg.stations[i]] = [cfg.stations[i], cfg.stations[i - 1]]; }
  await saveCfg({ stations: cfg.stations });
  renderStations();
});
async function addStation(name, url) {
  if (!url) return toast('Enter a stream URL');
  cfg.stations.push({ name: name || url, url });
  await saveCfg({ stations: cfg.stations });
  renderStations();
  toast('Station added');
}
$('stAdd').onclick = () => addStation($('stName').value.trim(), $('stUrl').value.trim()).then(() => { $('stName').value = $('stUrl').value = ''; });

let rbResults = [];
$('rbGo').onclick = async () => {
  const q = $('rbQ').value.trim();
  if (!q) return;
  $('rbResults').innerHTML = '<div class="muted item">Searching...</div>';
  const hosts = ['de1.api.radio-browser.info', 'nl1.api.radio-browser.info', 'at1.api.radio-browser.info'];
  for (const h of hosts) {
    try {
      const params = new URLSearchParams({ name: q, limit: 30, hidebroken: 'true', order: 'clickcount', reverse: 'true' });
      let r = await fetch(`https://${h}/json/stations/search?${params}`);
      rbResults = await r.json();
      if (!rbResults.length) {
        r = await fetch(`https://${h}/json/stations/bytag/${encodeURIComponent(q)}?limit=30&hidebroken=true&order=clickcount&reverse=true`);
        rbResults = await r.json();
      }
      rbResults = rbResults.filter(s => /mp3|aac|ogg|flac/i.test(s.codec || 'mp3'));
      $('rbResults').innerHTML = rbResults.map((s, i) => `
        <div class="item"><div class="grow"><div>${esc(s.name.trim())}</div><div class="muted">${esc([s.country, s.codec, s.bitrate ? s.bitrate + ' kbps' : '', s.tags].filter(Boolean).join(' · '))}</div></div>
        <button class="b sm" data-try="${i}">Try</button><button class="b sm p" data-add="${i}">Add</button></div>`).join('') || '<div class="muted item">No stations found.</div>';
      return;
    } catch (e) { /* try next mirror */ }
  }
  $('rbResults').innerHTML = '<div class="muted item">Could not reach radio-browser.info.</div>';
};
$('rbQ').addEventListener('keydown', e => e.key === 'Enter' && $('rbGo').click());
$('rbResults').addEventListener('click', e => {
  const t = e.target.closest('button');
  if (!t) return;
  const s = rbResults[+(t.dataset.try ?? t.dataset.add)];
  const url = s.url_resolved || s.url;
  if (t.dataset.try !== undefined) player('url', 0, { url });
  else addStation(s.name.trim(), url);
});

async function loadTracks() {
  if (!status || status.player.source !== 'sd') return;
  const tracks = await api('/api/tracks').catch(() => []);
  $('tracks').innerHTML = tracks.slice(0, 500).map((t, i) => `
    <div class="item ${i === status.player.trackIdx ? 'cur' : ''}"><button class="b sm" data-track="${i}">&#9654;</button>
      <div class="grow"><div>${esc(t.split('/').pop())}</div><div class="muted">${esc(t)}</div></div></div>`).join('') || '<div class="muted item">No music files. Upload some below or copy them to a /music folder on the card.</div>';
}
$('tracks').addEventListener('click', e => { const t = e.target.closest('button'); t && player('track', +t.dataset.track); });
$('usbOn').onclick = async () => {
  try {
    await api('/api/usbdrive', { json: { on: !(status && status.player.usbDrive) } });
    setTimeout(pollStatus, 500);
  } catch (e) { toast(e.message); }
};
$('rescan').onclick = () => player('rescan').then(() => setTimeout(() => { pollStatus(); loadTracks(); }, 1500));
$('musicUp').onclick = async () => {
  const files = [...$('musicFiles').files];
  if (!files.length) return toast('Choose files first');
  for (let i = 0; i < files.length; i++) {
    try {
      await upload('/api/music', files[i], 'file', p => ($('musicProg').style.width = ((i + p) / files.length) * 100 + '%'));
    } catch (e) { toast(`${files[i].name}: ${e.message}`); }
  }
  $('musicProg').style.width = '0';
  toast('Upload finished');
  setTimeout(() => { pollStatus(); loadTracks(); }, 1500);
};

// ---------- faces ----------
const faceCache = {};
async function getFace(url) {
  if (!faceCache[url]) faceCache[url] = fetch(url, { cache: 'no-cache' }).then(r => { if (!r.ok) throw new Error(r.statusText); return r.json(); });
  return faceCache[url];
}
const previews = new Map();  // canvas -> {face, assetBase}
setInterval(() => {
  const now = new Date();
  previews.forEach((v, c) => {
    if (!c.isConnected) return previews.delete(c);
    if (c.offsetParent) ClockFace.render(c, v.face, { date: now, assetBase: v.assetBase, data: { h24: cfg ? cfg.h24 : true } });
  });
}, 1000);
function previewCanvas(face, assetBase) {
  const c = document.createElement('canvas');
  c.width = c.height = 400;
  previews.set(c, { face, assetBase });
  ClockFace.render(c, face, { assetBase, data: { h24: cfg ? cfg.h24 : true }, onAssetLoad: () => ClockFace.render(c, face, { assetBase }) });
  return c;
}

async function loadFaces() {
  installedFaces = await api('/api/faces').catch(() => []);
  const grid = $('faceGrid');
  grid.innerHTML = installedFaces.length ? '' : '<p class="muted">No faces installed yet. Get some from the gallery.</p>';
  for (const f of installedFaces) {
    const card = document.createElement('div');
    card.className = 'card face';
    card.innerHTML = `<div class="n">${esc(f.name)} ${f.active ? '<span class="badge">On watch</span>' : ''}</div>
      <div class="muted">${esc(f.author || '')}</div>
      <div class="row">${f.active ? '' : `<button class="b sm p" data-use>Use</button>`}
      <button class="b sm" data-edit>Edit</button><button class="b sm" data-dl>Download</button>
      <button class="b sm d" data-rm>Delete</button></div>`;
    grid.appendChild(card);
    const url = `/faces/${encodeURIComponent(f.id)}.json`;
    delete faceCache[url];
    getFace(url).then(face => card.prepend(previewCanvas(face, '/faces/assets/'))).catch(() => {});
    card.addEventListener('click', async e => {
      const b = e.target.closest('button');
      if (!b) return;
      if ('use' in b.dataset) { await api('/api/faces/activate?id=' + encodeURIComponent(f.id), { method: 'POST' }); toast(`${f.name} is on the watch`); loadFaces(); }
      if ('dl' in b.dataset) download(f.id + '.json', JSON.stringify(await getFace(url), null, 1));
      if ('edit' in b.dataset) { showTab('designer'); setDesigner(await getFace(url)); }
      if ('rm' in b.dataset && confirm(`Delete "${f.name}" from the watch?`)) { await api('/api/faces?id=' + encodeURIComponent(f.id), { method: 'DELETE' }); loadFaces(); }
    });
  }
}
$('faceFile').onchange = async e => {
  const file = e.target.files[0];
  if (!file) return;
  if (/\.orb$/i.test(file.name)) {
    try {
      await installOrb(file);
    } catch (err) { toast('Could not import: ' + err.message); }
    e.target.value = '';
    return;
  }
  try {
    const face = JSON.parse(await file.text());
    await installFace(face, null, false);
    toast(`Installed ${face.name}`);
    loadFaces();
  } catch (err) { toast('Could not install: ' + err.message); }
  e.target.value = '';
};

// .orb theme -> converted in the browser, images uploaded, face saved and shown
async function installOrb(file) {
  toast(`Reading ${file.name}...`);
  const { face, files } = await OrbImport.convert(await file.arrayBuffer(), msg => toast(msg));
  const total = files.reduce((n, f) => n + f.bytes.length, 0);
  let done = 0;
  for (const f of files) {
    toast(`Uploading to the watch... ${Math.round((done / total) * 100)}%`);
    await upload('/api/faces/asset', new File([f.bytes], f.name), 'file');
    done += f.bytes.length;
  }
  await api(`/api/faces?id=${encodeURIComponent(face.id)}&activate=1`, { json: face });
  toast(`${face.name} is on the watch`);
  loadFaces();
}

async function installFace(face, baseUrl, activate) {
  // Gallery faces keep their images in an assets/ folder next to the face file;
  // copy them first so the face renders completely. Uploaded files bring no images.
  const assets = baseUrl ? face.assets || [] : [];
  for (let i = 0; i < assets.length; i++) {
    const name = assets[i];
    if (assets.length > 2) toast(`Copying images ${i + 1} of ${assets.length}...`);
    const r = await fetch(new URL('assets/' + name, baseUrl));
    if (!r.ok) throw new Error('Missing asset ' + name);
    const blob = await r.blob();
    await upload('/api/faces/asset', new File([blob], name.split('/').pop()), 'file');
  }
  const id = face.id || face.name;
  return api(`/api/faces?id=${encodeURIComponent(id)}${activate ? '&activate=1' : ''}`, { json: face });
}

// ---------- gallery ----------
function renderGallerySelect() {
  const sel = $('galSel'), cur = sel.value;
  sel.innerHTML = cfg.galleries.map(g => `<option value="${esc(g)}">${g.startsWith('/') ? 'Built-in gallery (on the watch)' : esc(g)}</option>`).join('');
  if (cur && cfg.galleries.includes(cur)) sel.value = cur;
}
async function loadGallery() {
  if (!cfg) return;
  renderGallerySelect();
  const src = $('galSel').value || cfg.galleries[0];
  $('galErr').textContent = '';
  const grid = $('galGrid');
  grid.innerHTML = '<p class="muted">Loading...</p>';
  if (!installedFaces.length) installedFaces = await api('/api/faces').catch(() => []);
  let index;
  const indexUrl = new URL(src, location.href);
  try {
    const r = await fetch(indexUrl, { cache: 'no-cache' });
    index = await r.json();
  } catch (e) {
    grid.innerHTML = '';
    $('galErr').textContent = 'Could not load this gallery. Online galleries need internet on this computer.';
    return;
  }
  grid.innerHTML = '';
  for (const item of index.faces || []) {
    const faceUrl = new URL(item.file, indexUrl).href;
    const assetBase = new URL('assets/', faceUrl).href;
    const installed = installedFaces.some(f => f.id === item.id);
    const card = document.createElement('div');
    card.className = 'card face';
    card.innerHTML = `<div class="n">${esc(item.name)} ${installed ? '<span class="badge">Installed</span>' : ''}</div>
      <div class="muted">${esc(item.description || '')}</div><div class="muted">by ${esc(item.author || 'unknown')}</div>
      <div class="row"><button class="b sm p" data-use>Install &amp; use</button><button class="b sm" data-inst>${installed ? 'Update' : 'Install'}</button><button class="b sm" data-dl>Download</button></div>`;
    grid.appendChild(card);
    getFace(faceUrl).then(face => {
      face.assets = face.assets || item.assets || [];
      card.prepend(previewCanvas(face, assetBase));
    }).catch(() => card.prepend(Object.assign(document.createElement('div'), { className: 'err', textContent: 'Preview failed' })));
    card.addEventListener('click', async e => {
      const b = e.target.closest('button');
      if (!b) return;
      const face = await getFace(faceUrl);
      if ('dl' in b.dataset) return download(item.id + '.json', JSON.stringify(face, null, 1));
      b.disabled = true;
      try {
        await installFace(Object.assign({ id: item.id }, face), faceUrl, 'use' in b.dataset);
        toast('use' in b.dataset ? `${item.name} is on the watch` : `${item.name} installed`);
        installedFaces = await api('/api/faces');
        loadGallery();
      } catch (err) { toast('Install failed: ' + err.message); b.disabled = false; }
    });
  }
}
$('galSel').onchange = loadGallery;
$('galReload').onclick = () => { Object.keys(faceCache).forEach(k => delete faceCache[k]); loadGallery(); };
$('galAdd').onclick = async () => {
  const u = $('galUrl').value.trim();
  if (!u) return;
  if (!cfg.galleries.includes(u)) cfg.galleries.push(u);
  await saveCfg({ galleries: cfg.galleries });
  $('galUrl').value = '';
  renderGallerySelect();
  $('galSel').value = u;
  loadGallery();
};

// ---------- designer ----------
const SNIPPETS = {
  ticks: { type: 'ticks', count: 60, radius: 226, length: 10, width: 2, color: '#555555', majorEvery: 5, majorLength: 24, majorWidth: 6, majorColor: '#FFFFFF' },
  numerals: { type: 'numerals', style: 'arabic', radius: 175, size: 32, color: '#FFFFFF' },
  hand: { type: 'hand', hand: 'minute', length: 170, tail: 18, width: 8, color: '#FFFFFF' },
  text: { type: 'text', text: '{time}', x: 0, y: 0, size: 48, color: '#FFFFFF', align: 'center' },
  arc: { type: 'arc', value: 'seconds', radius: 228, width: 8, color: '#0A84FF', bgColor: '#1C1C1E' },
  circle: { type: 'circle', x: 0, y: 0, radius: 10, color: '#FF3B30' },
  rect: { type: 'rect', x: 0, y: 80, w: 120, h: 40, radius: 12, color: '#222222' },
  image: { type: 'image', src: 'my-background.png', x: 0, y: 0 },
};
const BLANK = { name: 'My Face', author: '', description: '', background: { color: '#000000' }, elements: [SNIPPETS.ticks, SNIPPETS.hand, { type: 'hand', hand: 'hour', length: 110, width: 12, color: '#FFFFFF' }, { type: 'hand', hand: 'second', length: 190, tail: 30, width: 3, color: '#FF3B30', smooth: true }, { type: 'circle', radius: 9, color: '#FF3B30' }] };
let designerFace = null, designerReady = false;

function setDesigner(face) {
  designerFace = face;
  $('dJson').value = JSON.stringify(face, null, 2);
  $('dErr').textContent = '';
}
async function initDesigner() {
  if (!designerReady) {
    designerReady = true;
    if (!designerFace) setDesigner(BLANK);
    const now = new Date();
    $('dTime').value = now.getHours() * 60 + now.getMinutes();
    const draw = () => {
      if (!designerFace) return;
      let d = new Date();
      if (!$('dLive').checked) { const m = +$('dTime').value; d.setHours(Math.floor(m / 60), m % 60, 30, 0); }
      ClockFace.render($('dCanvas'), designerFace, { date: d, data: { h24: cfg ? cfg.h24 : true }, onAssetLoad: draw });
    };
    setInterval(() => { if (document.querySelector('#designer.on')) draw(); }, 100);
    $('dTime').oninput = () => { $('dLive').checked = false; draw(); };
  }
  const faces = await api('/api/faces').catch(() => []);
  $('dLoad').innerHTML = '<option value="">Start from...</option><option value="__blank">Blank analog</option>' +
    faces.map(f => `<option value="${esc(f.id)}">${esc(f.name)}</option>`).join('');
}
$('dJson').addEventListener('input', () => {
  try {
    const f = JSON.parse($('dJson').value);
    if (!Array.isArray(f.elements)) throw new Error('"elements" must be an array');
    designerFace = f;
    $('dErr').textContent = '';
  } catch (e) { $('dErr').textContent = e.message; }
});
$('dJson').addEventListener('keydown', e => {
  if (e.key === 'Tab') { e.preventDefault(); document.execCommand('insertText', false, '  '); }
});
$('dFormat').onclick = () => designerFace && setDesigner(designerFace);
$('dLoad').onchange = async e => {
  const v = e.target.value;
  if (!v) return;
  setDesigner(v === '__blank' ? JSON.parse(JSON.stringify(BLANK)) : await getFace(`/faces/${encodeURIComponent(v)}.json`));
  e.target.value = '';
};
$('dAdd').onchange = e => {
  const t = e.target.value;
  if (!t || !designerFace) return;
  designerFace.elements.push(JSON.parse(JSON.stringify(SNIPPETS[t])));
  setDesigner(designerFace);
  e.target.value = '';
  const ta = $('dJson');
  ta.scrollTop = ta.scrollHeight;
};
$('dSave').onclick = async () => {
  if (!designerFace) return;
  if (!designerFace.name) return toast('Give the face a "name"');
  try {
    const r = await installFace(Object.assign({}, designerFace, { assets: [] }), null, true);
    designerFace.id = r.id;
    toast('Saved and shown on the watch');
  } catch (e) { $('dErr').textContent = e.message; }
};
$('dDownload').onclick = () => designerFace && download((designerFace.id || designerFace.name || 'face').toLowerCase().replace(/[^a-z0-9-_]+/g, '-') + '.json', JSON.stringify(designerFace, null, 2));
$('dAssetUp').onclick = async () => {
  const f = $('dAsset').files[0];
  if (!f) return toast('Choose a PNG or JPG');
  if (f.size > 600000) return toast('Keep images under 600 KB');
  try {
    await upload('/api/faces/asset', f, 'file');
    toast(`Uploaded. Use "src": "${f.name}" in an image element.`);
  } catch (e) { toast(e.message); }
};

// ---------- news ----------
const POPULAR_FEEDS = [
  ['BBC News', 'https://feeds.bbci.co.uk/news/rss.xml'], ['BBC Technology', 'https://feeds.bbci.co.uk/news/technology/rss.xml'],
  ['NPR', 'https://feeds.npr.org/1001/rss.xml'], ['The Guardian', 'https://www.theguardian.com/world/rss'],
  ['Al Jazeera', 'https://www.aljazeera.com/xml/rss/all.xml'], ['CNBC', 'https://www.cnbc.com/id/100003114/device/rss/rss.html'],
  ['Ars Technica', 'https://feeds.arstechnica.com/arstechnica/index'], ['The Verge', 'https://www.theverge.com/rss/index.xml'],
  ['TechCrunch', 'https://techcrunch.com/feed/'], ['Hacker News', 'https://hnrss.org/frontpage'],
  ['ESPN', 'https://www.espn.com/espn/rss/news'], ['NASA', 'https://www.nasa.gov/feed/'],
];
function renderNewsCfg() {
  $('feeds').innerHTML = cfg.feeds.map((f, i) => `
    <div class="item"><input type="checkbox" data-en="${i}" ${f.enabled ? 'checked' : ''}>
      <div class="grow"><div>${esc(f.name)}</div><div class="muted">${esc(f.url)}</div></div>
      <button class="b sm d" data-del="${i}">&#10005;</button></div>`).join('') || '<div class="muted item">No feeds.</div>';
  $('topics').innerHTML = cfg.topics.map((t, i) => `<span class="chip">${esc(t)}<button data-tdel="${i}">&times;</button></span>`).join('') || '<span class="muted">No topics.</span>';
  $('newsMin').value = cfg.newsRefreshMin;
  $('feedSuggest').innerHTML = POPULAR_FEEDS.filter(([, u]) => !cfg.feeds.some(f => f.url === u)).map(([n, u]) => `
    <div class="item"><div class="grow"><div>${esc(n)}</div><div class="muted">${esc(u)}</div></div><button class="b sm" data-sug="${esc(u)}" data-name="${esc(n)}">Add</button></div>`).join('');
}
$('feeds').addEventListener('click', e => {
  if (e.target.dataset.en !== undefined) cfg.feeds[+e.target.dataset.en].enabled = e.target.checked;
  const b = e.target.closest('button');
  if (b && b.dataset.del !== undefined) { cfg.feeds.splice(+b.dataset.del, 1); renderNewsCfg(); }
});
$('feedSuggest').addEventListener('click', e => {
  const b = e.target.closest('button');
  if (b) { cfg.feeds.push({ name: b.dataset.name, url: b.dataset.sug, enabled: true }); renderNewsCfg(); }
});
$('topics').addEventListener('click', e => { const b = e.target.closest('button'); if (b) { cfg.topics.splice(+b.dataset.tdel, 1); renderNewsCfg(); } });
$('fdAdd').onclick = () => {
  const url = $('fdUrl').value.trim();
  if (!url) return;
  cfg.feeds.push({ name: $('fdName').value.trim() || new URL(url).hostname, url, enabled: true });
  $('fdName').value = $('fdUrl').value = '';
  renderNewsCfg();
};
$('tpAdd').onclick = () => { const t = $('tpNew').value.trim(); if (t) { cfg.topics.push(t); $('tpNew').value = ''; renderNewsCfg(); } };
$('tpNew').addEventListener('keydown', e => e.key === 'Enter' && $('tpAdd').click());
$('newsSave').onclick = async () => {
  await saveCfg({ feeds: cfg.feeds, topics: cfg.topics, newsRefreshMin: +$('newsMin').value || 20 });
  flash('newsMsg', 'Saved. Fetching headlines...');
  setTimeout(loadNews, 8000);
};
$('newsNow').onclick = async () => { await api('/api/news', { method: 'POST' }); flash('newsMsg', 'Refreshing...'); setTimeout(loadNews, 8000); };
async function loadNews() {
  if (cfg) renderNewsCfg();
  const n = await api('/api/news').catch(() => ({ items: [], status: 'Watch not reachable' }));
  $('newsStatus').textContent = n.status || `${n.items.length} headlines`;
  const ago = ts => { if (!ts) return ''; const m = Math.round((Date.now() / 1000 - ts) / 60); return m < 60 ? m + 'm' : m < 1440 ? Math.round(m / 60) + 'h' : Math.round(m / 1440) + 'd'; };
  $('headlines').innerHTML = n.items.map(i => `<div class="item"><div class="grow"><div class="muted">${esc(i.source)} ${i.ts ? '· ' + ago(i.ts) : ''}</div>
    <div style="white-space:normal;font-weight:600">${esc(i.title)}</div>${i.summary ? `<div class="muted" style="white-space:normal">${esc(i.summary)}</div>` : ''}</div></div>`).join('');
}

// ---------- photos ----------
let phFit = 'fill', phTo = 'flash';
$('phFit').addEventListener('click', e => { const b = e.target.closest('button'); if (!b) return; phFit = b.dataset.fit; [...$('phFit').children].forEach(x => x.classList.toggle('on', x === b)); });
$('phDest').addEventListener('click', e => { const b = e.target.closest('button'); if (!b || b.disabled) return; phTo = b.dataset.to; [...$('phDest').children].forEach(x => x.classList.toggle('on', x === b)); });
const mb = n => (n / 1048576).toFixed(1) + ' MB';

async function loadPhotos() {
  const p = cfg.photos || {};
  $('phSecs').value = p.seconds ?? 8; $('phShuffle').checked = !!p.shuffle; $('phFrame').checked = !!p.frame;
  let r;
  try { r = await api('/api/photos'); } catch (e) { $('phGrid').innerHTML = '<p class="muted">Watch not reachable.</p>'; return; }
  const sdBtn = $('phDest').querySelector('[data-to=sd]');
  sdBtn.disabled = !r.sd;
  if (!r.sd && phTo === 'sd') $('phDest').querySelector('[data-to=flash]').click();
  $('phSpace').textContent = `Watch memory: ${mb(r.flash.total - r.flash.used)} free` + (r.sd ? ` \u00b7 SD card: ${mb(r.sd.total - r.sd.used)} free` : ' \u00b7 No SD card');
  $('phGrid').innerHTML = r.photos.map(ph => {
    const url = ph.sd ? `/api/photo?sd=1&name=${encodeURIComponent(ph.name)}` : `/photos/${encodeURIComponent(ph.name)}`;
    return `<div style="text-align:center"><img src="${url}" loading="lazy" style="width:100%;aspect-ratio:1;border-radius:50%;object-fit:cover;background:#000">
      <div class="muted" style="font-size:12px">${ph.sd ? 'SD' : 'Watch'} \u00b7 ${Math.round(ph.size / 1024)} KB</div>
      <button class="b sm d" data-del="${esc(ph.name)}" data-sd="${ph.sd ? 1 : 0}">Delete</button></div>`;
  }).join('') || '<p class="muted">No photos yet. Add some on the left.</p>';
}
$('phGrid').addEventListener('click', async e => {
  const b = e.target.closest('button[data-del]');
  if (!b) return;
  await api(`/api/photos?name=${encodeURIComponent(b.dataset.del)}&sd=${b.dataset.sd}`, { method: 'DELETE' });
  loadPhotos();
});

// Crop/fit a picture to the 466 x 466 round screen and encode it as JPEG
async function preparePhoto(file) {
  const bmp = await createImageBitmap(file, { imageOrientation: 'from-image' });
  const S = 466, c = document.createElement('canvas');
  c.width = c.height = S;
  const ctx = c.getContext('2d');
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, S, S);
  const scale = phFit === 'fill' ? Math.max(S / bmp.width, S / bmp.height) : Math.min(S / bmp.width, S / bmp.height);
  const w = bmp.width * scale, h = bmp.height * scale;
  ctx.imageSmoothingQuality = 'high';
  ctx.drawImage(bmp, (S - w) / 2, (S - h) / 2, w, h);
  return new Promise(res => c.toBlob(res, 'image/jpeg', 0.82));
}
$('phUp').onclick = async () => {
  const files = [...$('phFiles').files];
  if (!files.length) return toast('Choose some pictures first');
  let ok = 0;
  for (let i = 0; i < files.length; i++) {
    try {
      const blob = await preparePhoto(files[i]);
      const base = files[i].name.replace(/\.[^.]+$/, '').replace(/[^A-Za-z0-9 _-]+/g, '').trim().slice(0, 40) || 'photo';
      const name = `${base}-${Date.now().toString(36)}.jpg`;
      await upload(`/api/photos?to=${phTo}`, new File([blob], name), 'file', p => ($('phProg').style.width = ((i + p) / files.length) * 100 + '%'));
      ok++;
    } catch (e) { toast(`${files[i].name}: ${e.message}`); if (/full/i.test(e.message)) break; }
  }
  $('phProg').style.width = '0';
  $('phFiles').value = '';
  toast(`${ok} photo${ok === 1 ? '' : 's'} added`);
  loadPhotos();
};
$('bibSave').onclick = async () => {
  const bible = { mode: $('bibMode').value, minutes: +$('bibMins').value || 30, version: $('bibVer').value };
  await saveCfg({ bible });
  cfg.bible = bible;
  flash('bibMsg', 'Saved');
};
$('bibMode').onchange = () => { const r = $('bibMode').value === 'random'; $('bibMinsRow').style.display = r ? '' : 'none'; $('bibVerRow').style.display = r ? 'none' : ''; };
function loadBible() {
  const b = cfg.bible || {};
  $('bibMode').value = b.mode || 'daily'; $('bibMins').value = b.minutes ?? 30; $('bibVer').value = b.version || 'KJV';
  $('bibMode').onchange();
}
$('phSave').onclick = async () => {
  const photos = { seconds: +$('phSecs').value || 8, shuffle: $('phShuffle').checked, frame: $('phFrame').checked };
  await saveCfg({ photos });
  cfg.photos = photos;
  flash('phMsg', 'Saved');
};

// ---------- alarms, night mode, countdowns ----------
const DAYS = ['Su', 'Mo', 'Tu', 'We', 'Th', 'Fr', 'Sa'];
function renderAlarms() {
  const st = cfg.stations || [];
  $('alList').innerHTML = (cfg.alarms || []).map((a, i) => `
    <div class="item" style="flex-wrap:wrap" data-i="${i}">
      <input type="checkbox" data-k="on" ${a.on ? 'checked' : ''} title="On/off">
      <input type="time" data-k="time" value="${esc(a.time)}" style="width:auto">
      <span class="row" style="gap:2px">${DAYS.map((d, n) => `<label class="chip" style="padding:3px 7px"><input type="checkbox" data-day="${n}" ${a.days.includes(n) ? 'checked' : ''}> ${d}</label>`).join('')}</span>
      <input type="text" data-k="label" value="${esc(a.label)}" placeholder="Label" style="flex:1 1 120px;width:auto">
      <select data-k="sound" style="width:auto"><option value="reveille" ${a.sound !== 'radio' && a.sound !== 'chime' ? 'selected' : ''}>Reveille</option><option value="chime" ${a.sound === 'chime' ? 'selected' : ''}>Chime</option>${st.length ? '<option value="radio" ' + (a.sound === 'radio' ? 'selected' : '') + '>Radio</option>' : ''}</select>
      <select data-k="station" style="width:auto;${a.sound === 'radio' ? '' : 'display:none'}">${st.map((s, n) => `<option value="${n}" ${n === a.station ? 'selected' : ''}>${esc(s.name)}</option>`).join('')}</select>
      <button class="b sm d" data-del="${i}">&#10005;</button>
    </div>`).join('') || '<div class="muted item">No alarms yet.</div>';
}
function readAlarms() {
  document.querySelectorAll('#alList .item[data-i]').forEach(row => {
    const a = cfg.alarms[+row.dataset.i];
    a.on = row.querySelector('[data-k=on]').checked;
    a.time = row.querySelector('[data-k=time]').value || '07:00';
    a.label = row.querySelector('[data-k=label]').value.trim();
    a.sound = row.querySelector('[data-k=sound]').value;
    a.station = +(row.querySelector('[data-k=station]').value || 0);
    a.days = [...row.querySelectorAll('[data-day]')].filter(c => c.checked).map(c => +c.dataset.day);
  });
}
$('alList').addEventListener('change', e => { if (e.target.dataset.k === 'sound') { readAlarms(); renderAlarms(); } });
$('alList').addEventListener('click', e => { const b = e.target.closest('button[data-del]'); if (b) { readAlarms(); cfg.alarms.splice(+b.dataset.del, 1); renderAlarms(); } });
$('alAdd').onclick = () => {
  readAlarms();
  if (cfg.alarms.length >= 10) return toast('Up to 10 alarms');
  cfg.alarms.push({ time: '07:00', days: [1, 2, 3, 4, 5], on: true, sound: 'reveille', station: 0, label: '' });
  renderAlarms();
};
$('alSave').onclick = async () => {
  readAlarms();
  for (const a of cfg.alarms) if (a.on && !a.days.length) return toast('Pick at least one day for each alarm');
  await saveCfg({ alarms: cfg.alarms, chimeVolume: +$('chimeVol').value, snoozeMin: +$('snoozeMin').value || 9 });
  cfg.chimeVolume = +$('chimeVol').value;
  flash('alMsg', 'Saved');
};
$('chimeTest').onclick = async () => { await player('alert', +$('chimeVol').value); setTimeout(() => player('alertstop'), 3000); };
$('reveilleTest').onclick = async () => { await player('alert', +$('chimeVol').value, { sound: 'reveille' }); setTimeout(() => player('alertstop'), 14000); };
$('nightBri').oninput = () => ($('nightBriV').textContent = $('nightBri').value);
$('nightSave').onclick = async () => {
  const night = { on: $('nightOn').checked, start: $('nightStart').value || '22:00', end: $('nightEnd').value || '07:00', brightness: +$('nightBri').value };
  await saveCfg({ night });
  cfg.night = night;
  flash('nightMsg', 'Saved');
};
function renderCountdowns() {
  $('cdList').innerHTML = (cfg.countdowns || []).map((c, i) => `<div class="item"><div class="grow"><div>${esc(c.name || 'Countdown')}</div><div class="muted">${esc(c.date)}</div></div><button class="b sm d" data-del="${i}">&#10005;</button></div>`).join('') || '<div class="muted item">No countdowns.</div>';
}
$('cdList').addEventListener('click', e => { const b = e.target.closest('button[data-del]'); if (b) { cfg.countdowns.splice(+b.dataset.del, 1); renderCountdowns(); } });
$('cdAdd').onclick = () => {
  if (!$('cdDate').value) return toast('Pick a date');
  cfg.countdowns.push({ name: $('cdName').value.trim(), date: $('cdDate').value });
  $('cdName').value = ''; $('cdDate').value = '';
  renderCountdowns();
};
$('cdSave').onclick = async () => { await saveCfg({ countdowns: cfg.countdowns }); flash('cdMsg', 'Saved'); };
function loadAlarmsTab() {
  cfg.alarms = cfg.alarms || []; cfg.countdowns = cfg.countdowns || [];
  renderAlarms(); renderCountdowns();
  $('chimeVol').value = cfg.chimeVolume ?? 15; $('snoozeMin').value = cfg.snoozeMin ?? 9;
  const n = cfg.night || {};
  $('nightOn').checked = !!n.on; $('nightStart').value = n.start || '22:00'; $('nightEnd').value = n.end || '07:00';
  $('nightBri').value = n.brightness ?? 12; $('nightBriV').textContent = $('nightBri').value;
}

// ---------- weather location ----------
let wxPick = null, wxUnits = 'F';
function renderWeatherCfg() {
  const w = cfg.weather || {};
  wxUnits = w.units || 'F';
  [...$('wxUnits').children].forEach(b => b.classList.toggle('on', b.dataset.u === wxUnits));
  $('wxNow').textContent = w.place ? `Current location: ${w.place}` : 'No location set yet.';
}
$('wxUnits').addEventListener('click', e => { const b = e.target.closest('button'); if (b) { wxUnits = b.dataset.u; [...$('wxUnits').children].forEach(x => x.classList.toggle('on', x === b)); } });
$('wxFind').onclick = async () => {
  const q = $('wxSearch').value.trim();
  if (!q) return;
  try {
    const r = await (await fetch('https://geocoding-api.open-meteo.com/v1/search?count=6&language=en&name=' + encodeURIComponent(q))).json();
    const res = r.results || [];
    $('wxResults').innerHTML = res.map((p, i) => `<div class="item"><div class="grow">${esc(p.name)}<span class="muted"> ${esc([p.admin1, p.country].filter(Boolean).join(', '))}</span></div><button class="b sm" data-i="${i}">Use</button></div>`).join('') || '<div class="muted item">No places found.</div>';
    $('wxResults').onclick = e => {
      const b = e.target.closest('button[data-i]');
      if (!b) return;
      const p = res[+b.dataset.i];
      wxPick = { lat: p.latitude, lon: p.longitude, place: p.name };
      $('wxNow').textContent = `Selected: ${p.name}${p.admin1 ? ', ' + p.admin1 : ''} - press Save`;
      $('wxResults').innerHTML = '';
    };
  } catch (e) { toast('Place search needs internet on this device'); }
};
$('wxSearch').addEventListener('keydown', e => e.key === 'Enter' && $('wxFind').click());
$('wxSave').onclick = async () => {
  const weather = Object.assign({}, cfg.weather || {}, wxPick || {}, { units: wxUnits });
  await saveCfg({ weather });
  cfg.weather = weather;
  wxPick = null;
  renderWeatherCfg();
  flash('wxMsg', 'Saved - the watch is updating its weather');
};

// ---------- screens ----------
const SCREENS = { weather: 'Weather', today: 'Today (calendar, alarms, countdowns)', week: 'Week (calendar for the next 7 days)', sports: 'Sports scores', music: 'Music', news: 'News', photos: 'Photos', timer: 'Timer', bible: 'Bible verse', settings: 'Settings' };
let scrOrder = [];
function renderScreens() {
  if (!scrOrder.length) {
    const on = (cfg.screens && cfg.screens.length) ? cfg.screens.filter(s => SCREENS[s]) : Object.keys(SCREENS);
    scrOrder = [...on.map(id => ({ id, on: true })), ...Object.keys(SCREENS).filter(id => !on.includes(id)).map(id => ({ id, on: false }))];
  }
  $('scrList').innerHTML = '<div class="item"><input type="checkbox" checked disabled><div class="grow">Clock</div></div>' + scrOrder.map((s, i) => `
    <div class="item"><input type="checkbox" data-on="${i}" ${s.on ? 'checked' : ''} ${s.id === 'settings' ? 'disabled' : ''}><div class="grow">${SCREENS[s.id]}</div>
    ${i > 0 ? `<button class="b sm" data-up="${i}">&#8593;</button>` : ''}</div>`).join('');
}
$('scrList').addEventListener('change', e => { if (e.target.dataset.on !== undefined) scrOrder[+e.target.dataset.on].on = e.target.checked; });
$('scrList').addEventListener('click', e => {
  const b = e.target.closest('button[data-up]');
  if (!b) return;
  const i = +b.dataset.up;
  [scrOrder[i - 1], scrOrder[i]] = [scrOrder[i], scrOrder[i - 1]];
  renderScreens();
  renderCalendars();
  renderTeams();
});
$('scrSave').onclick = async () => {
  const screens = ['clock', ...scrOrder.filter(s => s.on || s.id === 'settings').map(s => s.id)];
  await saveCfg({ screens });
  cfg.screens = screens;
  await api('/api/reboot', { method: 'POST' }).catch(() => {});
  flash('scrMsg', 'Saved - the watch is restarting');
};

// ---------- calendar ----------
function renderCalendars() {
  cfg.calendars = cfg.calendars || [];
  $('calList').innerHTML = cfg.calendars.map((c, i) => `<div class="item"><div class="grow"><div>${esc(c.name || 'Calendar')}</div><div class="muted">${c.url === '********' ? 'Link saved' : esc(c.url)}</div></div><button class="b sm d" data-del="${i}">&#10005;</button></div>`).join('') || '<div class="muted item">No calendars yet.</div>';
  $('calRemind').value = cfg.reminderMin ?? 10;
}
$('calList').addEventListener('click', e => { const b = e.target.closest('button[data-del]'); if (b) { cfg.calendars.splice(+b.dataset.del, 1); renderCalendars(); } });
$('calAdd').onclick = () => {
  let url = $('calUrl').value.trim();
  if (!/^(https?|webcal):\/\//i.test(url)) return toast('Paste the calendar link (starting with https:// or webcal://)');
  const name = $('calName').value.trim() || 'Calendar ' + (cfg.calendars.length + 1);
  if (cfg.calendars.some(c => c.name === name)) return toast('Use a different name');
  cfg.calendars.push({ name, url });
  $('calName').value = ''; $('calUrl').value = '';
  renderCalendars();
};
$('calSave').onclick = async () => {
  await saveCfg({ calendars: cfg.calendars, reminderMin: +$('calRemind').value || 0 });
  cfg = await api('/api/config');
  renderCalendars();
  flash('calMsg', 'Saved - the watch is reading your calendar');
};

// ---------- sports ----------
let spTeams = [];
function renderTeams() {
  cfg.teams = cfg.teams || [];
  $('sportList').innerHTML = cfg.teams.map((t, i) => `<div class="item"><div class="grow"><div>${esc(t.name)}</div><div class="muted">${esc(t.league)}</div></div><button class="b sm d" data-del="${i}">&#10005;</button></div>`).join('') || '<div class="muted item">Not following any teams yet.</div>';
  $('sportAlerts').checked = cfg.sportsAlerts !== false;
}
$('sportList').addEventListener('click', e => { const b = e.target.closest('button[data-del]'); if (b) { cfg.teams.splice(+b.dataset.del, 1); renderTeams(); } });
$('sportLoad').onclick = async () => {
  const [sport, league] = $('sportLeague').value.split('/');
  $('sportTeam').innerHTML = '<option>Loading...</option>';
  try {
    spTeams = (await api(`/api/espn/teams?sport=${sport}&league=${league}`)).sort((a, b) => a.name.localeCompare(b.name));
    $('sportTeam').innerHTML = '<option value="">Choose a team...</option>' + spTeams.map((t, i) => `<option value="${i}">${esc(t.name)}</option>`).join('');
    $('sportTeam').disabled = $('sportAdd').disabled = false;
  } catch (err) { $('sportTeam').innerHTML = '<option>Couldn’t load teams</option>'; toast(err.message); }
};
// Adds the team selected in the drop-down; returns false if there's nothing new to add
function followSelected(quiet) {
  if ($('sportTeam').value === '' || $('sportTeam').disabled) return false;
  const t = spTeams[+$('sportTeam').value];
  if (!t) return false;
  const [sport, league] = $('sportLeague').value.split('/');
  if (cfg.teams.some(x => String(x.id) === String(t.id) && x.league === league)) { if (!quiet) toast('Already following ' + t.name); return false; }
  if (cfg.teams.length >= 8) { toast('Up to 8 teams'); return false; }
  cfg.teams.push({ sport, league, id: String(t.id), abbr: t.abbr, name: t.name });
  renderTeams();
  return true;
}
$('sportAdd').onclick = () => followSelected(false);
$('sportTeam').onchange = () => { if (followSelected(true)) toast('Added - press Save to keep it'); };
$('sportSave').onclick = async () => {
  followSelected(true);  // include the team showing in the drop-down
  await saveCfg({ teams: cfg.teams, sportsAlerts: $('sportAlerts').checked });
  cfg.sportsAlerts = $('sportAlerts').checked;
  flash('sportMsg', 'Saved - scores appear on the Sports screen shortly');
};

// ---------- messages ----------
$('msgFrom').value = (() => { try { return localStorage.getItem('clockhubFrom') || ''; } catch (e) { return ''; } })();
$('msgSend').onclick = async () => {
  const text = $('msgText').value.trim(), from = $('msgFrom').value.trim(), file = $('msgPhoto').files[0];
  if (!text && !file) return toast('Type a message or pick a photo');
  try { localStorage.setItem('clockhubFrom', from); } catch (e) { /* private mode */ }
  $('msgSend').disabled = true;
  try {
    if (file) {
      const saved = phFit;
      phFit = 'fill';
      const blob = await preparePhoto(file);
      phFit = saved;
      await upload('/api/message/photo', new File([blob], 'msg.jpg'), 'file');
    }
    await api('/api/message', { json: { text, from, photo: !!file } });
    $('msgText').value = ''; $('msgPhoto').value = '';
    flash('msgMsg', 'Sent');
  } catch (err) { toast(err.message); }
  $('msgSend').disabled = false;
};

// ---------- settings ----------
const TZ = {
  'UTC': 'UTC0', 'Europe/London': 'GMT0BST,M3.5.0/1,M10.5.0', 'Europe/Dublin': 'IST-1GMT0,M10.5.0,M3.5.0/1', 'Europe/Lisbon': 'WET0WEST,M3.5.0/1,M10.5.0',
  'Europe/Paris': 'CET-1CEST,M3.5.0,M10.5.0/3', 'Europe/Berlin': 'CET-1CEST,M3.5.0,M10.5.0/3', 'Europe/Madrid': 'CET-1CEST,M3.5.0,M10.5.0/3',
  'Europe/Rome': 'CET-1CEST,M3.5.0,M10.5.0/3', 'Europe/Amsterdam': 'CET-1CEST,M3.5.0,M10.5.0/3', 'Europe/Brussels': 'CET-1CEST,M3.5.0,M10.5.0/3',
  'Europe/Zurich': 'CET-1CEST,M3.5.0,M10.5.0/3', 'Europe/Vienna': 'CET-1CEST,M3.5.0,M10.5.0/3', 'Europe/Stockholm': 'CET-1CEST,M3.5.0,M10.5.0/3',
  'Europe/Oslo': 'CET-1CEST,M3.5.0,M10.5.0/3', 'Europe/Copenhagen': 'CET-1CEST,M3.5.0,M10.5.0/3', 'Europe/Warsaw': 'CET-1CEST,M3.5.0,M10.5.0/3',
  'Europe/Prague': 'CET-1CEST,M3.5.0,M10.5.0/3', 'Europe/Budapest': 'CET-1CEST,M3.5.0,M10.5.0/3', 'Europe/Athens': 'EET-2EEST,M3.5.0/3,M10.5.0/4',
  'Europe/Helsinki': 'EET-2EEST,M3.5.0/3,M10.5.0/4', 'Europe/Kyiv': 'EET-2EEST,M3.5.0/3,M10.5.0/4', 'Europe/Bucharest': 'EET-2EEST,M3.5.0/3,M10.5.0/4',
  'Europe/Istanbul': '<+03>-3', 'Europe/Moscow': 'MSK-3', 'Africa/Lagos': 'WAT-1', 'Africa/Cairo': 'EET-2EEST,M4.5.5/0,M10.5.4/24',
  'Africa/Johannesburg': 'SAST-2', 'Africa/Nairobi': 'EAT-3', 'Asia/Jerusalem': 'IST-2IDT,M3.4.4/26,M10.5.0', 'Asia/Riyadh': '<+03>-3',
  'Asia/Tehran': '<+0330>-3:30', 'Asia/Dubai': '<+04>-4', 'Asia/Karachi': 'PKT-5', 'Asia/Kolkata': 'IST-5:30', 'Asia/Kathmandu': '<+0545>-5:45',
  'Asia/Dhaka': '<+06>-6', 'Asia/Bangkok': '<+07>-7', 'Asia/Ho_Chi_Minh': '<+07>-7', 'Asia/Jakarta': 'WIB-7', 'Asia/Singapore': '<+08>-8',
  'Asia/Kuala_Lumpur': '<+08>-8', 'Asia/Manila': 'PST-8', 'Asia/Shanghai': 'CST-8', 'Asia/Hong_Kong': 'HKT-8', 'Asia/Taipei': 'CST-8',
  'Australia/Perth': 'AWST-8', 'Asia/Tokyo': 'JST-9', 'Asia/Seoul': 'KST-9', 'Australia/Darwin': 'ACST-9:30', 'Australia/Adelaide': 'ACST-9:30ACDT,M10.1.0,M4.1.0/3',
  'Australia/Brisbane': 'AEST-10', 'Australia/Sydney': 'AEST-10AEDT,M10.1.0,M4.1.0/3', 'Australia/Melbourne': 'AEST-10AEDT,M10.1.0,M4.1.0/3',
  'Pacific/Auckland': 'NZST-12NZDT,M9.5.0,M4.1.0/3', 'Pacific/Honolulu': 'HST10', 'America/Anchorage': 'AKST9AKDT,M3.2.0,M11.1.0',
  'America/Los_Angeles': 'PST8PDT,M3.2.0,M11.1.0', 'America/Vancouver': 'PST8PDT,M3.2.0,M11.1.0', 'America/Phoenix': 'MST7',
  'America/Denver': 'MST7MDT,M3.2.0,M11.1.0', 'America/Edmonton': 'MST7MDT,M3.2.0,M11.1.0', 'America/Chicago': 'CST6CDT,M3.2.0,M11.1.0',
  'America/Winnipeg': 'CST6CDT,M3.2.0,M11.1.0', 'America/Mexico_City': 'CST6', 'America/New_York': 'EST5EDT,M3.2.0,M11.1.0',
  'America/Toronto': 'EST5EDT,M3.2.0,M11.1.0', 'America/Detroit': 'EST5EDT,M3.2.0,M11.1.0', 'America/Bogota': '<-05>5', 'America/Lima': '<-05>5',
  'America/Halifax': 'AST4ADT,M3.2.0,M11.1.0', 'America/Santiago': '<-04>4<-03>,M9.1.6/24,M4.1.6/24', 'America/St_Johns': 'NST3:30NDT,M3.2.0,M11.1.0',
  'America/Sao_Paulo': '<-03>3', 'America/Argentina/Buenos_Aires': '<-03>3',
};
$('tzSel').innerHTML = Object.keys(TZ).map(z => `<option>${z}</option>`).join('') + '<option value="">Custom...</option>';
$('tzSel').onchange = () => { if ($('tzSel').value) $('tz').value = TZ[$('tzSel').value]; };
$('tzDetect').onclick = () => {
  const z = Intl.DateTimeFormat().resolvedOptions().timeZone;
  if (TZ[z]) { $('tzSel').value = z; $('tz').value = TZ[z]; toast('Set to ' + z); }
  else toast(`${z} isn't in the list. Pick the nearest city or enter a POSIX string.`);
};

function renderWifi() {
  $('wifiList').innerHTML = cfg.wifi.map((w, i) => `<div class="item"><div class="grow"><div>${i === 0 ? '<b>' + esc(w.ssid) + '</b> <span class="muted">(tried first)</span>' : esc(w.ssid)}</div><div class="muted">${w.pass ? 'Password saved' : 'Open network'}</div></div>
    ${i > 0 ? `<button class="b sm" data-up="${i}" title="Try this one first">&#8593;</button>` : ''}<button class="b sm d" data-del="${i}">&#10005;</button></div>`).join('') || '<div class="muted item">No networks saved. The watch opens a setup hotspot.</div>';
}
$('wifiList').addEventListener('click', e => {
  const b = e.target.closest('button');
  if (!b) return;
  if (b.dataset.del !== undefined) cfg.wifi.splice(+b.dataset.del, 1);
  if (b.dataset.up !== undefined) { const i = +b.dataset.up; cfg.wifi.unshift(cfg.wifi.splice(i, 1)[0]); }
  renderWifi();
});
$('wfScan').onclick = async () => {
  $('wfScan').disabled = true;
  try {
    const nets = await api('/api/wifi/scan');
    $('wfSsidSel').innerHTML = '<option value="">Choose network...</option>' + nets.map(n => `<option value="${esc(n.ssid)}">${esc(n.ssid)} (${n.rssi} dBm${n.secure ? ', secured' : ''})</option>`).join('');
  } catch (e) { toast(e.message); }
  $('wfScan').disabled = false;
};
$('wfSsidSel').onchange = () => { $('wfSsid').value = $('wfSsidSel').value; $('wfPass').focus(); };
$('wfAdd').onclick = () => {
  const ssid = $('wfSsid').value.trim();
  if (!ssid) return;
  cfg.wifi = cfg.wifi.filter(w => w.ssid !== ssid);
  cfg.wifi.unshift({ ssid, pass: $('wfPass').value });  // newest first = tried first
  $('wfSsid').value = $('wfPass').value = '';
  renderWifi();
};
$('wifiSave').onclick = async () => {
  // Include a network typed into the boxes even if "Add" wasn't clicked
  if ($('wfSsid').value.trim()) $('wfAdd').click();
  if (!cfg.wifi.length) return toast('Add a network first');
  const target = cfg.wifi[0].ssid;
  await saveCfg({ wifi: cfg.wifi, hostname: $('hostname').value.trim() || 'clockhub' });
  cfg = await api('/api/config');
  renderWifi();
  $('wifiMsg').textContent = `Saved. Joining "${target}"...`;
  // The page may lose the watch while it switches networks; report what we can see
  const t0 = Date.now();
  while (Date.now() - t0 < 45000) {
    await new Promise(r => setTimeout(r, 2500));
    try {
      const n = (await api('/api/status')).net;
      if (n.connected && n.ssid === target) { $('wifiMsg').textContent = `Connected to "${target}" at ${n.ip}.`; return; }
      if (n.wifiError) { $('wifiMsg').textContent = n.wifiError; return; }
    } catch (e) {
      $('wifiMsg').textContent = `Switching to "${target}"... If this page stops responding, the watch has moved to that network; its new address is on the watch's Settings screen.`;
    }
  }
};
$('timeSave').onclick = async () => {
  await saveCfg({ tz: $('tz').value.trim() || 'UTC0', tzName: $('tzSel').value || 'Custom', ntp: $('ntp').value.trim() || 'pool.ntp.org', h24: $('h24').checked });
  cfg.h24 = $('h24').checked;
  flash('timeMsg', 'Saved');
};
['bri', 'dimBri'].forEach(id => $(id).addEventListener('input', () => { $('briV').textContent = $('bri').value; $('dimV').textContent = $('dimBri').value; }));
$('bri').addEventListener('change', () => saveCfg({ brightness: +$('bri').value }));
$('dispSave').onclick = async () => {
  await saveCfg({ brightness: +$('bri').value, dimBrightness: +$('dimBri').value, dimAfter: +$('dimAfter').value, offAfter: +$('offAfter').value, pixelShift: $('pixelShift').checked });
  flash('dispMsg', 'Saved');
};
['encA', 'encB', 'encBtn'].forEach(id => ($(id).innerHTML = [16, 17, 18, -1].map(p => `<option value="${p}">${p < 0 ? 'Not used' : 'GPIO' + p}</option>`).join('')));
$('encSave').onclick = async () => {
  const enc = { enabled: $('encOn').checked, a: +$('encA').value, b: +$('encB').value, button: +$('encBtn').value, reverse: $('encRev').checked, stepsPerDetent: +$('encSteps').value };
  if (enc.enabled && new Set([enc.a, enc.b, enc.button].filter(p => p >= 0)).size < [enc.a, enc.b, enc.button].filter(p => p >= 0).length) return toast('Each pin can only be used once');
  await saveCfg({ encoder: enc });
  cfg.encoder = enc;
  flash('encMsg', 'Saved');
};
// Live knob test: polls the watch quickly while the panel is open
let knobBase = null;
setInterval(async () => {
  if (!$('knobTest').open || !document.querySelector('#settings.on')) { knobBase = null; return; }
  try {
    const s = await api('/api/status');
    const k = s.knob || {};
    if (!knobBase) knobBase = { t: k.transitions, p: k.presses };
    const bit = b => (k.pins >> b) & 1 ? 'high' : '<b style="color:var(--err)">low</b>';
    const e = cfg.encoder || {};
    $('knobLive').innerHTML = k.pins < 0 ? '<tr><td>Knob is turned off in the settings above.</td></tr>' :
      `<tr><td class="muted">CLK (GPIO${e.a})</td><td>${bit(2)}</td></tr><tr><td class="muted">DT (GPIO${e.b})</td><td>${bit(1)}</td></tr>` +
      `<tr><td class="muted">SW (GPIO${e.button})</td><td>${bit(0)}</td></tr>` +
      `<tr><td class="muted">Turn steps seen</td><td>${Math.trunc((k.transitions - knobBase.t) / (e.stepsPerDetent || 4))}</td></tr>` +
      `<tr><td class="muted">Presses seen</td><td>${k.presses - knobBase.p}</td></tr>`;
  } catch (err) { /* watch busy */ }
}, 250);
$('knobWire').onclick = async () => {
  try {
    const r = await api('/api/knobtest');
    const names = ['GPIO18', 'GPIO17', 'GPIO16'];
    const held = [0, 1, 2].filter(b => (r.pulldown >> b) & 1).map(b => names[b]);
    $('knobWireMsg').textContent = held.length === 3 ? "All three pins see the knob's power. If turning shows nothing, check the GND wire."
      : held.length ? `Connected: ${held.join(', ')}. Not connected: ${names.filter(n => !held.includes(n)).join(', ')}.`
      : 'None of the knob pins are connected (or the knob has no power).';
  } catch (err) { $('knobWireMsg').textContent = err.message; }
};
$('pwSave').onclick = async () => { await saveCfg({ adminPass: $('adminPass').value }); flash('pwMsg', 'Saved'); };

function renderSettings() {
  renderWifi();
  $('hostname').value = cfg.hostname;
  $('tz').value = cfg.tz;
  $('tzSel').value = TZ[cfg.tzName] === cfg.tz ? cfg.tzName : (Object.keys(TZ).find(z => TZ[z] === cfg.tz) || '');
  $('ntp').value = cfg.ntp;
  $('h24').checked = cfg.h24;
  $('bri').value = cfg.brightness; $('dimBri').value = cfg.dimBrightness;
  $('briV').textContent = cfg.brightness; $('dimV').textContent = cfg.dimBrightness;
  $('dimAfter').value = cfg.dimAfter; $('offAfter').value = cfg.offAfter;
  $('pixelShift').checked = cfg.pixelShift;
  $('adminPass').value = cfg.adminPass;
  const enc = cfg.encoder || {};
  $('encOn').checked = !!enc.enabled;
  $('encA').value = enc.a ?? 18; $('encB').value = enc.b ?? 17; $('encBtn').value = enc.button ?? 16;
  $('encRev').checked = !!enc.reverse;
  $('encSteps').value = enc.stepsPerDetent ?? 4;
}

// ---------- system ----------
function renderSystem() {
  if (!status) return;
  const up = status.uptime, n = status.net;
  const rows = [
    ['Firmware', status.version], ['Uptime', `${Math.floor(up / 86400)}d ${Math.floor(up / 3600) % 24}h ${Math.floor(up / 60) % 60}m`],
    ['Wi-Fi', n.connected ? `${n.ssid} (${n.rssi} dBm)` : n.ap ? 'Setup hotspot' : 'Disconnected'], ['IP address', n.ip],
    ['Time synced', n.timeSynced ? 'Yes' : 'Not yet'], ['Watch time', new Date(status.time * 1000).toLocaleString()],
    ['Battery', status.power.percent >= 0 ? `${status.power.percent}%${status.power.charging ? ' (charging)' : ''}` : 'No battery'],
    ['Free memory', `${Math.round(status.heap / 1024)} KB internal, ${Math.round(status.psram / 1024)} KB PSRAM`],
    ['Active face', status.face], ['Headlines', status.newsCount],
  ];
  $('sysTable').innerHTML = rows.map(([k, v]) => `<tr><td class="muted">${k}</td><td>${esc(v)}</td></tr>`).join('');
}
async function ota(file, field) {
  if (!file) return toast('Choose a .bin file');
  if (!confirm('Update the watch now? It restarts when done.')) return;
  try {
    await upload('/api/ota', file, field, p => ($('otaProg').style.width = p * 100 + '%'));
    toast('Updated. Restarting...');
    setTimeout(() => location.reload(), 9000);
  } catch (e) { toast('Update failed: ' + e.message); $('otaProg').style.width = '0'; }
}
$('fwUp').onclick = () => ota($('fwFile').files[0], 'firmware');
$('fsUp').onclick = () => ota($('fsFile').files[0], 'fs');
$('reboot').onclick = () => confirm('Restart the watch?') && api('/api/reboot', { method: 'POST' }).then(() => toast('Restarting...'));
$('cfgExport').onclick = async () => download('clockhub-settings.json', JSON.stringify(await api('/api/config'), null, 2));

// ---------- start ----------
(async function start() {
  try { cfg = await api('/api/config'); } catch (e) { $('pill').textContent = 'Watch not reachable'; return; }
  renderSettings();
  renderWeatherCfg();
  renderScreens();
  loadBible();
  await pollStatus();
  setInterval(pollStatus, 2000);
  const tab = location.hash.slice(1);
  showTab(document.getElementById(tab) && document.querySelector(`nav [data-tab="${tab}"]`) ? tab : 'music');
})();
