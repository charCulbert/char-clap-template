// Plugin <-> page messages, as clap.webview/3 defines them: ArrayBuffers sent
// with window.parent.postMessage() and received as 'message' events.
// Values are JSON-like and travel as CBOR, matching libs/core/messages.cpp.
// Nothing in here is specific to one plugin.

export function sendMessage(value) {
  window.parent.postMessage(encode(value), '*');
}

export function onMessage(handler) {
  window.addEventListener('message', event => {
    if (!(event.data instanceof ArrayBuffer)) return;
    let value;
    try { value = decode(event.data); } catch { return; }
    handler(value);
  });
}

// ---- CBOR subset: null, booleans, numbers, strings, arrays, plain objects.

export function encode(value) {
  const bytes = [];
  const head = (major, n) => {
    const type = major << 5;
    if (n < 24) bytes.push(type | n);
    else if (n < 0x100) bytes.push(type | 24, n);
    else if (n < 0x10000) bytes.push(type | 25, n >> 8, n & 0xff);
    else if (n < 0x100000000) bytes.push(type | 26, n >>> 24, (n >> 16) & 0xff, (n >> 8) & 0xff, n & 0xff);
    else {
      bytes.push(type | 27);
      for (let shift = 56; shift >= 0; shift -= 8) bytes.push(Number((BigInt(n) >> BigInt(shift)) & 0xffn));
    }
  };
  const write = v => {
    if (v === null || v === undefined) bytes.push(0xf6);
    else if (v === false) bytes.push(0xf4);
    else if (v === true) bytes.push(0xf5);
    else if (typeof v === 'number') {
      if (Number.isSafeInteger(v)) head(v < 0 ? 1 : 0, v < 0 ? -1 - v : v);
      else {
        const view = new DataView(new ArrayBuffer(8));
        view.setFloat64(0, v);
        bytes.push(0xfb, ...new Uint8Array(view.buffer));
      }
    } else if (typeof v === 'string') {
      const utf8 = new TextEncoder().encode(v);
      head(3, utf8.length);
      for (const b of utf8) bytes.push(b);
    } else if (Array.isArray(v)) {
      head(4, v.length);
      v.forEach(write);
    } else if (typeof v === 'object') {
      const keys = Object.keys(v);
      head(5, keys.length);
      for (const key of keys) { write(key); write(v[key]); }
    } else {
      throw new TypeError(`cannot send a ${typeof v}`);
    }
  };
  write(value);
  return new Uint8Array(bytes).buffer;
}

export function decode(buffer) {
  const view = new DataView(buffer);
  let pos = 0;
  const need = n => { if (pos + n > view.byteLength) throw new RangeError('truncated CBOR'); };
  const read = depth => {
    if (depth > 32) throw new RangeError('CBOR nested too deeply');
    need(1);
    const initial = view.getUint8(pos++), major = initial >> 5, info = initial & 31;
    if (major === 7) {
      if (info === 20) return false;
      if (info === 21) return true;
      if (info === 22) return null;
      if (info === 26) { need(4); pos += 4; return view.getFloat32(pos - 4); }
      if (info === 27) { need(8); pos += 8; return view.getFloat64(pos - 8); }
      throw new TypeError('unsupported CBOR simple value');
    }
    let n = info;
    if (info === 24) { need(1); n = view.getUint8(pos); pos += 1; }
    else if (info === 25) { need(2); n = view.getUint16(pos); pos += 2; }
    else if (info === 26) { need(4); n = view.getUint32(pos); pos += 4; }
    else if (info === 27) { need(8); n = Number(view.getBigUint64(pos)); pos += 8; }
    else if (info > 27) throw new TypeError('unsupported CBOR length');
    if (major === 0) return n;
    if (major === 1) return -1 - n;
    if (major === 3) {
      need(n);
      pos += n;
      return new TextDecoder().decode(buffer.slice(pos - n, pos));
    }
    if (major === 4) {
      need(n);
      const array = [];
      for (let i = 0; i < n; i++) array.push(read(depth + 1));
      return array;
    }
    if (major === 5) {
      need(n * 2);
      const entries = new Map();
      for (let i = 0; i < n; i++) {
        const key = read(depth + 1);
        if (typeof key !== 'string' || entries.has(key)) throw new TypeError('bad CBOR map key');
        entries.set(key, read(depth + 1));
      }
      return Object.fromEntries(entries); // own properties, even for "__proto__"
    }
    throw new TypeError('unsupported CBOR type');
  };
  const value = read(0);
  if (pos !== view.byteLength) throw new RangeError('trailing bytes after CBOR value');
  return value;
}
