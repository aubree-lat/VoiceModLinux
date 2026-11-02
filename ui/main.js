// Linux launcher and window for Voicemod: runs first-time setup, starts the Windows engine
// under Wine, serves the frontend extracted from the user's Voicemod.exe and points it at the
// engine's Brigitte websocket.
const { app, BrowserWindow, protocol, net, ipcMain, shell, Tray, Menu, nativeImage } = require('electron');
const { spawn, spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const sockets = require('net');
const { EngineClient, runEngineCommand, ENGINE_HELP } = require('./engine');
const { pathToFileURL } = require('url');

// Adapted from AriCuteGirl/voicemod-linux (MIT). The `vmwine` script (next to this ui/ folder)
// does the Wine work; it exports WINEPREFIX and VMWINE_DATA before starting Electron.
const RES = path.resolve(__dirname, '..');
const SCRIPT = path.join(RES, 'vmwine');
const PREFIX = process.env.WINEPREFIX;
const DATA = process.env.VMWINE_DATA;
if (!PREFIX || !DATA) {
  console.error('Start this through `vmwine ui` (it sets WINEPREFIX and VMWINE_DATA).');
  process.exit(2);
}
const FRONTEND = path.join(DATA, 'frontend');
const VM_DATA = path.join(PREFIX, 'drive_c/users', process.env.USER, 'AppData/Local/VoicemodV3');
const ENGINE_LOG = path.join(VM_DATA, 'logs/voicemod-desktop.log');
const DEBUG_LOG = path.join(VM_DATA, 'logs/linux-ui.log');
const CONTROL_API_PORT = 59129;
// voicemod-ctl (installed into ~/.local/bin) talks to this socket; see handleControl().
const CONTROL_SOCKET = path.join(process.env.XDG_RUNTIME_DIR || os.tmpdir(), 'voicemod-linux.sock');
// Apps whose sound is also sent through the virtual mic; tools/vad-watch.py does the linking.
const MUSIC_FILE = path.join(process.env.XDG_RUNTIME_DIR || os.tmpdir(), 'voicemod-linux-music');

protocol.registerSchemesAsPrivileged([
  { scheme: 'vm', privileges: { standard: true, secure: true, supportFetchAPI: true, corsEnabled: true, stream: true } },
]);

// Environment for the shell script and Wine: drop what the AppImage runtime adds for Electron.
function scriptEnv() {
  const env = { ...process.env, VOICEMOD_HOME: DATA };
  const appdir = process.env.APPDIR;
  for (const key of ['LD_LIBRARY_PATH', 'LD_PRELOAD']) {
    if (!env[key]) continue;
    const kept = env[key].split(':').filter((p) => p && !(appdir && p.startsWith(appdir)));
    if (kept.length) env[key] = kept.join(':');
    else delete env[key];
  }
  return env;
}

function runScript(args, onLine) {
  return new Promise((resolve) => {
    const child = spawn('bash', [SCRIPT, ...args], { env: scriptEnv() });
    let pending = '';
    const feed = (chunk) => {
      pending += chunk.toString().replace(/\r/g, '\n');
      const lines = pending.split('\n');
      pending = lines.pop();
      for (const line of lines) if (line.trim()) onLine?.(line);
    };
    child.stdout.on('data', feed);
    child.stderr.on('data', feed);
    child.on('close', (code) => {
      if (pending.trim()) onLine?.(pending);
      resolve(code);
    });
  });
}

// Wine sockets belong to wineserver, so the engine's ports show up as its listening sockets.
function wineserverPids() {
  const pids = [];
  for (const pid of fs.readdirSync('/proc').filter((p) => /^\d+$/.test(p))) {
    try {
      if (fs.readFileSync(`/proc/${pid}/comm`, 'utf8').trim() !== 'wineserver') continue;
      const env = fs.readFileSync(`/proc/${pid}/environ`, 'utf8').split('\0');
      const prefix = env.find((e) => e.startsWith('WINEPREFIX='));
      if (!prefix || path.resolve(prefix.slice(11)) === path.resolve(PREFIX)) pids.push(pid);
    } catch { /* process vanished or not ours */ }
  }
  return pids;
}

function socketInodes(pid) {
  const inodes = new Set();
  for (const fd of fs.readdirSync(`/proc/${pid}/fd`)) {
    try {
      const m = /^socket:\[(\d+)\]$/.exec(fs.readlinkSync(`/proc/${pid}/fd/${fd}`));
      if (m) inodes.add(m[1]);
    } catch { /* fd closed meanwhile */ }
  }
  return inodes;
}

// Brigitte listens on a random 127.0.0.1 port; the Control API is the fixed 59129.
function findBrigittePorts() {
  const inodes = new Set();
  for (const pid of wineserverPids()) for (const i of socketInodes(pid)) inodes.add(i);
  const ports = [];
  for (const line of fs.readFileSync('/proc/net/tcp', 'utf8').split('\n').slice(1)) {
    const f = line.trim().split(/\s+/);
    if (f.length < 10 || f[3] !== '0A' || !inodes.has(f[9])) continue; // 0A = LISTEN
    const [addr, port] = f[1].split(':');
    const n = parseInt(port, 16);
    if (addr === '0100007F' && n !== CONTROL_API_PORT) ports.push(n);
  }
  return ports;
}

function readDeviceId() {
  try {
    const m = /DeviceId ([0-9a-f-]{36})/.exec(fs.readFileSync(ENGINE_LOG, 'utf8'));
    return m ? m[1] : 'unknown';
  } catch {
    return 'unknown';
  }
}

function readVersion() {
  try {
    return fs.readFileSync(path.join(DATA, '.voicemod-version'), 'utf8').trim();
  } catch {
    return 'unknown';
  }
}

async function waitForBrigitte(timeoutMs) {
  const end = Date.now() + timeoutMs;
  while (Date.now() < end) {
    const ports = findBrigittePorts();
    if (ports.length) return ports[0];
    await new Promise((r) => setTimeout(r, 500));
  }
  return null;
}

function pageUrl(port) {
  const query = new URLSearchParams({
    t: String(Date.now()),
    boot: 'true',
    os: 'Windows',
    deviceId: readDeviceId(),
    version: readVersion(),
    lang: process.env.VOICEMOD_LANG || 'en',
  });
  return `vm://app/index.html?${query}#/h/${port}/voices`;
}

const isSetUp = () => fs.existsSync(path.join(FRONTEND, 'index.html')) &&
  fs.existsSync(path.join(VM_DATA, 'app/last/Voicemod.exe'));

const linkFromArgs = (argv) => argv.find((a) => a.startsWith('voicemod:'));

let win = null;
let wheel = null;
let wheelOpen = false;
let wheelHideTimer = null;
let engineStarted = false;
let brigittePort = null;

function status(text) {
  win?.webContents.send('launcher-line', text);
}

async function forwardLink(url) {
  if (!engineStarted) return;
  await runScript(['url', url]);
  win?.show();
  win?.focus();
}

// The Action Wheel lives in its own transparent window that stays loaded and connected, so
// opening it is instant. Hyprland rules (title "Voicemod Wheel") make it a pinned overlay.
function createWheel(port) {
  wheel = new BrowserWindow({
    show: false,
    frame: false,
    transparent: true,
    backgroundColor: '#00000000',
    alwaysOnTop: true,
    skipTaskbar: true,
    resizable: false,
    hasShadow: false,
    fullscreenable: false,
    width: 1920,
    height: 1080,
    title: 'Voicemod Wheel',
    webPreferences: {
      preload: path.join(__dirname, 'wheel-preload.js'),
      contextIsolation: false,
      sandbox: false,
    },
  });
  wheel.setAlwaysOnTop(true, 'screen-saver');
  wheel.on('page-title-updated', (e) => e.preventDefault());
  const query = new URLSearchParams({
    port: String(port),
    t: String(Date.now()),
    deviceId: readDeviceId(),
    appVersion: readVersion(),
    env: 'production',
  });
  wheel.loadURL(`vm://app/wheel/index.html?${query}`);
  wheel.on('closed', () => { wheel = null; wheelOpen = false; });
}

function hideWheel() {
  clearTimeout(wheelHideTimer);
  wheelOpen = false;
  wheel?.hide();
}

function showWheel() {
  if (!wheel || wheelOpen) return;
  wheelOpen = true;
  wheel.show();
  wheel.focus();
  wheel.webContents.send('wheel-show');
}

function releaseWheel() {
  if (!wheel || !wheelOpen) return;
  wheel.webContents.send('wheel-release');
  // The page reports WheelHidden after its closing animation; don't stay up if it doesn't.
  clearTimeout(wheelHideTimer);
  wheelHideTimer = setTimeout(hideWheel, 1500);
}

const CONTROL_HELP = `commands:\nwheel show|release|toggle\nshow\n${ENGINE_HELP}`;
let engineClient = null;

async function handleControl(line) {
  const [cmd, ...rest] = line.trim().split(/\s+/);
  const arg = rest[0];
  if (cmd === 'wheel') {
    if (!wheel) return 'error: engine not ready';
    if (arg === 'show') showWheel();
    else if (arg === 'release' || arg === 'hide') releaseWheel();
    else if (arg === 'toggle' || !arg) (wheelOpen ? releaseWheel : showWheel)();
    else return `error: ${CONTROL_HELP}`;
    return 'ok';
  }
  if (cmd === 'show') {
    win?.show();
    win?.focus();
    return 'ok';
  }
  if (!brigittePort) return 'error: engine not ready';
  engineClient ??= new EngineClient(brigittePort);
  if (cmd === 'sound-id') {   // used by the mirrored hotkeys: play one sound by its resource id
    if (!/^[0-9a-f-]{36}$/i.test(arg || '')) return 'error: sound-id needs a sound id';
    await engineClient.send('playSound', { id: arg, sendToVAD: true });
    return 'ok';
  }
  try {
    const reply = await runEngineCommand(engineClient, cmd, rest);
    if (reply !== null) return reply;
  } catch (err) {
    return `error: ${err.message}`;
  }
  return `error: ${CONTROL_HELP}`;
}

// --- Mirror the keybinds made inside Voicemod for the Linux hotkey listener (tools/vmkeys.py).
// Voicemod's own hotkeys can't see the keyboard under Wine on Wayland, so we read the binds out
// of the engine every few seconds and write them where the listener picks them up.
const BINDS_FILE = path.join(process.env.XDG_RUNTIME_DIR || os.tmpdir(), 'vmwine-voicemod-binds.json');
let lastBindsJson = '';

function bindCommand(a) {
  switch (a.command) {
    case 'playSound': return a.data?.id ? `sound-id ${a.data.id}` : null;
    case 'stopAllSounds': return 'sound stop';
    case 'showWheel': return 'wheel toggle';
    case 'MuteMic': return 'mute toggle';
    case 'toggleVoiceChanger': return 'voice-changer toggle';
    case 'toggleHearMyself': return 'hear-myself toggle';
    case 'setHearMyself': return `hear-myself ${a.data?.enabled ? 'on' : 'off'}`;
    case 'enableVoiceChanger': return `voice-changer ${a.data?.enabled ? 'on' : 'off'}`;
    case 'bringToFront': return 'show';
    default: return null;
  }
}

async function syncKeybinds() {
  try {
    engineClient ??= new EngineClient(brigittePort);
    const { keyBinds = [] } = await engineClient.request('GetUserKeyBinds');
    const names = new Map();
    try { for (const r of await engineClient.allResources()) names.set(r.id, r.name); } catch { /* names are cosmetic */ }
    const binds = [];
    const skipped = [];
    for (const b of keyBinds) {
      if (!b.keys?.length) continue;
      const label = b.action.command + (b.action.data?.id ? `: ${names.get(b.action.data.id) || b.action.data.id}` : '');
      const cmd = bindCommand(b.action);
      if (!cmd) skipped.push({ label, action: b.action.command, reason: 'action not supported yet' });
      else if (b.action.interaction !== 'OnPress') skipped.push({ label, action: b.action.command, reason: `${b.action.interaction} not supported yet` });
      else binds.push({ keys: b.keys, cmd, label });
    }
    const json = JSON.stringify({ binds, skipped });
    if (json === lastBindsJson) return;
    lastBindsJson = json;
    const tmp = `${BINDS_FILE}.tmp`;
    fs.writeFileSync(tmp, json, { mode: 0o600 });
    fs.renameSync(tmp, BINDS_FILE);
  } catch { /* engine busy or not ready: try again next round */ }
}

function startKeybindSync() {
  syncKeybinds();
  setInterval(syncKeybinds, 8000);
}

// --- System tray (StatusNotifierItem), replacing Wine's own tray window ---------------------
let tray = null;
let quitting = false;
app.on('before-quit', () => { quitting = true; });

// Is something listening for tray icons (Quickshell, waybar, KDE, GNOME's extension...)?
let trayHostCache = null;
function trayHost() {
  if (trayHostCache === null) {
    const r = spawnSync('busctl', ['--user', 'status', 'org.kde.StatusNotifierWatcher'], { timeout: 3000 });
    trayHostCache = r.status === 0;
  }
  return trayHostCache;
}

const trayState = { 'voice-changer': false, 'hear-myself': false, mute: false };
const TRAY_QUERIES = { 'voice-changer': 'isVoiceChangerEnabled', 'hear-myself': 'getHearMyself', mute: 'IsMuteMicEnabled' };

function toggleWindow() {
  if (!win) return;
  if (win.isVisible() && win.isFocused()) win.hide();
  else { win.show(); win.focus(); }
}

function buildTrayMenu() {
  const toggle = (label, key) => ({
    label, type: 'checkbox', checked: trayState[key], enabled: !!brigittePort,
    click: async () => { await handleControl(`${key} toggle`); refreshTrayState(); },
  });
  return Menu.buildFromTemplate([
    { label: win && win.isVisible() ? 'Hide Window' : 'Show Window', click: toggleWindow },
    { type: 'separator' },
    toggle('Voice Changer', 'voice-changer'),
    toggle('Hear Myself', 'hear-myself'),
    toggle('Mute Microphone', 'mute'),
    { label: 'Stop All Sounds', enabled: !!brigittePort, click: () => handleControl('sound stop') },
    { type: 'separator' },
    { label: 'Exit', click: () => app.quit() },
  ]);
}

async function refreshTrayState() {
  if (!tray) return;
  if (brigittePort) {
    engineClient ??= new EngineClient(brigittePort);
    for (const [key, query] of Object.entries(TRAY_QUERIES)) {
      try { trayState[key] = !!(await engineClient.request(query)).enabled; } catch { /* keep last value */ }
    }
  }
  tray.setContextMenu(buildTrayMenu());
}

function createTray() {
  if (tray || !trayHost()) return;
  // Prefer Voicemod's own icon (converted at install time), else the bundled one.
  const own = nativeImage.createFromPath(path.join(PREFIX, 'vmwine', 'icon.png'));
  const bundled = nativeImage.createFromPath(path.join(__dirname, 'icon.png'));
  const icon = !own.isEmpty() && own.getSize().width >= 32 ? own : bundled;   // a 16px icon looks blurry
  tray = new Tray(icon.resize({ width: 32, height: 32 }));
  tray.setToolTip('Voicemod');
  tray.on('click', toggleWindow);
  tray.setContextMenu(buildTrayMenu());
  setInterval(refreshTrayState, 5000);
}

// --- Keep the audio stream alive ---------------------------------------------------------------
// Right after the interface loads, the engine often stops its audio stream and doesn't restart
// it (log: "Audio streams were finished, reason=kByUser"), so the interface shows "Audio Down"
// until you pick the microphone again in Settings. Picking it sends SetSelectedAudioDevices; this
// watchdog sends the same message with the devices already saved in settings.ini when the
// stream has been inactive for a few seconds.
const WATCHDOG_LOG = path.join(PREFIX, 'vmwine', 'watchdog.log');
let streamDownSince = 0;
let lastKick = 0;

function savedDevices() {
  const ini = fs.readFileSync(path.join(VM_DATA, 'settings.ini'), 'utf8');
  const m = /^selectedAudioDevices="(.*)"\s*$/m.exec(ini);
  if (!m) return null;
  const j = JSON.parse(m[1].replace(/\\"/g, '"'));
  const d = {
    inputDeviceUID: j.inputDevice?.deviceId,
    monitorDeviceUID: j.outputDevice?.deviceId,
    virtualDeviceUID: j.virtualDevice?.deviceId,
  };
  return d.inputDeviceUID ? d : null;
}

function watchdogLog(text) {
  try {
    fs.mkdirSync(path.dirname(WATCHDOG_LOG), { recursive: true });
    fs.appendFileSync(WATCHDOG_LOG, `${new Date().toISOString()} ${text}\n`);
  } catch { /* logging is best effort */ }
}

async function audioWatchdog() {
  if (!brigittePort) return;
  try {
    engineClient ??= new EngineClient(brigittePort);
    const st = await engineClient.request('GetAudioStreamStatus');
    if (st.isActive !== false) { streamDownSince = 0; return; }
    const now = Date.now();
    if (!streamDownSince) { streamDownSince = now; watchdogLog('stream inactive'); }
    if (now - streamDownSince > 6000 && now - lastKick > 20000) {
      const devices = savedDevices();
      if (!devices) return;
      lastKick = now;
      await engineClient.send('SetSelectedAudioDevices', devices);
      watchdogLog('stream still inactive: re-sent the saved audio devices');
    }
  } catch { /* engine busy: try again next round */ }
}

function startControlServer() {
  try { fs.unlinkSync(CONTROL_SOCKET); } catch { /* not there */ }
  const server = sockets.createServer((conn) => {
    let buffer = '';
    conn.on('data', async (chunk) => {
      buffer += chunk;
      if (!buffer.includes('\n')) return;
      const reply = await handleControl(buffer.split('\n')[0]);
      conn.end(String(reply) + '\n');
    });
  });
  server.listen(CONTROL_SOCKET, () => fs.chmodSync(CONTROL_SOCKET, 0o600));
  app.on('will-quit', () => { try { fs.unlinkSync(CONTROL_SOCKET); } catch { /* gone */ } });
}

function routedMusicApps() {
  try {
    return fs.readFileSync(MUSIC_FILE, 'utf8').split('\n').map((l) => l.trim()).filter(Boolean);
  } catch {
    return [];
  }
}

// Apps currently playing sound, for the "music through mic" panel.
function playingApps() {
  const out = spawnSync('pactl', ['-f', 'json', 'list', 'sink-inputs'], { env: { ...scriptEnv(), LC_ALL: 'C' } });
  let inputs = [];
  try { inputs = JSON.parse(out.stdout.toString() || '[]'); } catch { /* pactl missing or odd output */ }
  const names = new Set();
  for (const input of inputs) {
    const p = input.properties || {};
    // Display name; the watcher matches it case-insensitively against PipeWire node names.
    const name = p['application.name'] || p['node.name'];
    if (!name || /voicemod/i.test(name) || /^wine/.test(p['application.process.binary'] || '')) continue;
    if (Number(p['application.process.id']) === process.pid) continue;
    names.add(name);
  }
  return [...names].sort((a, b) => a.localeCompare(b));
}

function setMusicApp(app, on) {
  const key = String(app).trim().toLowerCase();
  const apps = routedMusicApps().filter((a) => a !== key);
  if (on) apps.push(key);
  fs.writeFileSync(MUSIC_FILE, apps.map((a) => a + '\n').join(''));
}

// Put voicemod-ctl on PATH so compositor binds can reach the running app.
function installCtl() {
  const src = path.join(RES, 'tools', 'voicemod-ctl');
  const dest = path.join(os.homedir(), '.local/bin/voicemod-ctl');
  try {
    const content = fs.readFileSync(src);
    if (fs.existsSync(dest) && fs.readFileSync(dest).equals(content)) return;
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    fs.writeFileSync(dest, content, { mode: 0o755 });
  } catch (err) {
    console.error(`could not install voicemod-ctl: ${err.message}`);
  }
}

async function launch() {
  win = new BrowserWindow({
    width: 1280,
    height: 800,
    minWidth: 960,
    minHeight: 600,
    title: 'Voicemod',
    backgroundColor: '#0d0d12',
    autoHideMenuBar: true,
    webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true },
  });
  // With a tray, closing the window hides it (like Voicemod on Windows); Quit is in the tray menu.
  // Without a tray host the window would vanish with no way back, so closing quits instead.
  createTray();
  win.on('close', (e) => {
    if (!quitting && tray && trayHost()) { e.preventDefault(); win.hide(); }
  });
  // The hidden wheel window would otherwise keep the app alive after the main window closes.
  win.on('closed', () => app.quit());
  win.webContents.setWindowOpenHandler(({ url }) => {
    if (/^https?:\/\//.test(url)) shell.openExternal(url);
    return { action: 'deny' };
  });
  if (process.env.VOICEMOD_UI_LOG) {
    win.webContents.on('console-message', (e) => console.log(`[page] ${e.message}`));
  }
  await win.loadFile(path.join(__dirname, 'launcher.html'));

  if (!isSetUp()) {
    win.webContents.send('launcher-failed',
      'Voicemod is not installed yet. In a terminal run:  vmwine install /path/to/VoicemodInstaller.exe');
    return;
  }

  status('Starting the Voicemod engine…');
  if (await runScript(['start-engine'], status) !== 0) {
    win.webContents.send('launcher-failed', 'Could not start the engine. See the log above.');
    return;
  }
  engineStarted = true;

  const port = await waitForBrigitte(90000);
  if (!port) {
    win.webContents.send('launcher-failed', 'The engine did not come up within 90 seconds.');
    return;
  }
  const pending = linkFromArgs(process.argv);
  if (pending) forwardLink(pending);
  brigittePort = port;
  startKeybindSync();
  setInterval(audioWatchdog, 3000);
  win.loadURL(pageUrl(port));
  createWheel(port);
  // Test aid: VMWINE_SHOT=/path/file.png saves a screenshot of the window after a while.
  if (process.env.VMWINE_SHOT) {
    setTimeout(async () => {
      try { fs.writeFileSync(process.env.VMWINE_SHOT, (await win.webContents.capturePage()).toPNG()); } catch { /* window gone */ }
    }, Number(process.env.VMWINE_SHOT_DELAY || 15000));
  }
}

if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', (_e, argv) => {
    const link = linkFromArgs(argv);
    if (link) forwardLink(link);
    if (win) {
      if (win.isMinimized()) win.restore();
      win.show();
      win.focus();
    }
  });

  app.whenReady().then(() => {
    protocol.handle('vm', (request) => {
      const rel = decodeURIComponent(new URL(request.url).pathname);
      const file = path.resolve(FRONTEND, '.' + rel);
      if (!file.startsWith(FRONTEND + path.sep)) return new Response('forbidden', { status: 403 });
      return net.fetch(pathToFileURL(file).toString());
    });

    ipcMain.handle('debug-log', (_e, text) => fs.promises.appendFile(DEBUG_LOG, text));
    ipcMain.handle('open-url', (_e, url) => {
      if (/^https?:\/\//.test(url)) shell.openExternal(url);
    });
    ipcMain.on('wheel-closed', () => setTimeout(hideWheel, 200));
    ipcMain.handle('music-state', () => ({ playing: playingApps(), routed: routedMusicApps() }));
    ipcMain.handle('music-set', (_e, app, on) => setMusicApp(app, on));

    // (voicemod-ctl is installed by the package / `vmwine install`, not written to $HOME from here)
    startControlServer();
    launch();
  });

  app.on('window-all-closed', () => app.quit());

  // Closing the window ends the session: stop Wine, the watcher and the virtual devices.
  app.on('will-quit', () => {
    if (engineStarted && !process.env.VOICEMOD_KEEP_ENGINE) {
      spawnSync('bash', [SCRIPT, 'stop'], { env: scriptEnv(), timeout: 15000 });
    }
  });
}
