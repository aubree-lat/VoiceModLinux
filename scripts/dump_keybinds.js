// dump_keybinds.js <brigitte-port>
// Read-only: asks the running Voicemod engine for the user's keybinds and the resource
// names behind them, and prints a summary. Uses the same "busMessage" protocol as the UI.
const port = process.argv[2];
const ws = new WebSocket(`ws://127.0.0.1:${port}`);
const waiters = [];
const t = setTimeout(() => { console.error('timeout'); process.exit(1); }, 15000);
ws.onmessage = (m) => {
  let d; try { d = JSON.parse(m.data)?.bridgeData?.data; } catch { return; }
  if (!d) return;
  for (const w of waiters.filter((w) => w.type === d.type)) { waiters.splice(waiters.indexOf(w), 1); w.resolve(d.object ?? {}); }
};
const ask = (type, object = null) => new Promise((resolve) => {
  waiters.push({ type, resolve });
  ws.send(JSON.stringify({ bridgeData: { type: 'busMessage', data: { id: crypto.randomUUID(), type, object } } }));
});
ws.onopen = async () => {
  const { keyBinds = [] } = await ask('GetUserKeyBinds');
  const { collections = [] } = await ask('GetAllResources', { sender: 'wheel' });
  const names = new Map();
  for (const c of collections) for (const r of c.resources ?? []) names.set(r.id, `${r.type}:${r.name}`);
  const rows = keyBinds.map((b) => ({
    keys: b.keys.map((k) => '0x' + k.toString(16)).join('+'),
    keyDecimal: b.keys,
    command: b.action.command,
    interaction: b.action.interaction,
    target: names.get(b.action.data?.id) || JSON.stringify(b.action.data ?? {}),
  }));
  console.log(`total binds: ${rows.length}`);
  const byCmd = {};
  for (const r of rows) byCmd[r.command] = (byCmd[r.command] || 0) + 1;
  console.log('by command:', JSON.stringify(byCmd));
  const sizes = {};
  for (const r of rows) sizes[r.keyDecimal.length] = (sizes[r.keyDecimal.length] || 0) + 1;
  console.log('binds by number of keys in the combo:', JSON.stringify(sizes));
  const distinct = new Set(rows.flatMap((r) => r.keyDecimal));
  console.log('distinct key codes used:', [...distinct].sort((a, b) => a - b).map((k) => '0x' + k.toString(16)).join(' '));
  console.log('--- all binds (keys -> command target)');
  for (const r of rows) console.log(`${r.keys.padEnd(34)} ${r.interaction.padEnd(10)} ${r.command}  ${r.target}`);
  clearTimeout(t);
  process.exit(0);
};
