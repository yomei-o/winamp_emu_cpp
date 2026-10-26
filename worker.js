// The emulator's thread.  It never returns from waemu_main while Winamp runs:
// waiting for input is an Atomics.wait on the event ring the page fills.
// File operations from the page's file panel arrive on the same ring and are
// done by jwFileOp below, inside that wait.
importScripts('waemu.js');

const ROOT = '/C:/Music';
const PATH_BYTES = 1024;

self.onmessage = async (msg) => {
  const m = msg.data;
  if (m.type !== 'start') return;
  const Module = await createWaemu({
    print: (s) => postMessage({type: 'log', text: s}),
    printErr: (s) => postMessage({type: 'log', text: s}),
    locateFile: (p) => p,
    setStatus: (s) => { if (s) postMessage({type: 'status', text: s}); },
  });
  const FS = Module.FS;
  const events = new Int32Array(m.events);
  const box = new Uint8Array(m.mailbox);
  Module.jwEvents = events;
  // The sound ring the page's AudioWorklet drains (see audio-worklet.js).
  if (m.audioHeader) {
    Module.waAudio = new Int32Array(m.audioHeader);
    Module.waPcm = new Int16Array(m.audioPcm);
  }

  const mkdirs = (dir) => {
    let cur = '';
    for (const part of dir.split('/').filter(Boolean)) {
      cur += '/' + part;
      try { FS.mkdir(cur); } catch (e) {}
    }
  };
  const parent = (p) => p.slice(0, p.lastIndexOf('/')) || '/';
  const isDir = (p) => { try { return FS.isDir(FS.stat(p).mode); } catch (e) { return false; } };
  const removeTree = (p) => {
    if (!isDir(p)) { try { FS.unlink(p); } catch (e) {} return; }
    for (const n of FS.readdir(p)) if (n !== '.' && n !== '..') removeTree(p + '/' + n);
    try { FS.rmdir(p); } catch (e) {}
  };
  const list = (dir, out) => {
    for (const n of FS.readdir(dir)) {
      if (n === '.' || n === '..') continue;
      const p = dir + '/' + n;
      const st = FS.stat(p);
      const d = FS.isDir(st.mode);
      out.push({p, size: d ? 0 : st.size, mtime: +st.mtime, dir: d});
      if (d) list(p, out);
    }
    return out;
  };
  const boxPath = () => {
    let end = box.indexOf(0);
    if (end < 0 || end > PATH_BYTES) end = PATH_BYTES;
    return new TextDecoder().decode(box.slice(0, end));
  };
  const ack = (id) => { Atomics.store(events, 2, id); Atomics.notify(events, 2); };
  const inside = (p) => p === ROOT || p.startsWith(ROOT + '/');

  // slot: [type, id, offset, length, total, ...]
  Module.jwFileOp = (slot) => {
    const [type, id, offset, length] = slot;
    const p = boxPath();
    switch (type) {
      case 100:  // list everything under C:\Music
        postMessage({type: 'list', id, entries: list(ROOT, [])});
        break;
      case 101: {  // read one file
        let data = null;
        try { data = FS.readFile(p); } catch (e) {}
        postMessage({type: 'file', id, path: p, data}, data ? [data.buffer] : []);
        break;
      }
      case 102: {  // write a chunk (offset 0 starts the file)
        if (!inside(p)) break;
        mkdirs(parent(p));
        const chunk = box.slice(PATH_BYTES, PATH_BYTES + length);
        if (offset === 0) {
          FS.writeFile(p, chunk);
        } else {
          const s = FS.open(p, 'a');
          FS.write(s, chunk, 0, chunk.length);
          FS.close(s);
        }
        break;
      }
      case 103:  // delete a file or a folder
        if (inside(p) && p !== ROOT) removeTree(p);
        break;
      case 104:  // new folder
        if (inside(p)) mkdirs(p);
        break;
      case 105:  // play this file now
      case 106:  // add it to the playlist
        Module.waOpenPath = p;
        ack(id);
        return true;  // the emulator carries it out (main_wasm.cpp winamp_open)
    }
    ack(id);
    return false;
  };

  try { FS.mkdir('/persist'); } catch (e) {}
  // Settings (the registry file) come from the page's localStorage.
  if (m.registry) FS.writeFile('/persist/registry.txt', m.registry);
  // Files the user added, kept by the page in IndexedDB.
  for (const f of m.files || []) {
    if (!inside(f.path)) continue;
    if (f.deleted) removeTree(f.path);
    else if (f.dir) mkdirs(f.path);
    else { mkdirs(parent(f.path)); FS.writeFile(f.path, new Uint8Array(f.data)); }
  }
  FS.chdir(ROOT);
  postMessage({type: 'status', text: 'running'});
  postMessage({type: 'ready', entries: list(ROOT, [])});
  Module._waemu_main(m.width, m.height);
};
