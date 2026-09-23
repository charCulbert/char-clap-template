// JS side of the message codec, and a JS -> C++ -> JS round trip.
// Usage: node tests/messages.test.mjs <path to messages_test>
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {encode, decode} from '../webUI/lib/messages.js';

const roundTrip = v => decode(encode(v));

// The two cbor-walker encoder bugs: exact sizes and maps of 24+ keys.
assert.equal(encode('x'.repeat(64)).byteLength, 66);
for (const n of [23, 24, 31, 32, 300, 70000]) {
  const object = Object.fromEntries(Array.from({length: n}, (_, i) => ['k' + i, i]));
  assert.deepEqual(roundTrip(object), object);
}
const values = {
  type: 'state', ints: [0, 23, 24, 255, 256, 65535, 65536, 2 ** 32, 2 ** 53 - 1, -1, -25, -(2 ** 40)],
  floats: [0.5, -2.75, 1e300, Infinity], text: 'héllo ✓ \u0000', empty: {}, list: [null, true, false, []],
  long: 'y'.repeat(100000),
};
assert.deepEqual(roundTrip(values), values);
assert.ok(Number.isNaN(roundTrip(NaN)));
assert.equal(roundTrip(undefined), null);
assert.equal(Object.getPrototypeOf(decode(encode(JSON.parse('{"__proto__": 1}')))), Object.prototype);
assert.throws(() => encode(() => {}));
assert.throws(() => encode(1n));

const bytes = new Uint8Array(encode(values));
for (let i = 0; i < bytes.length; i += 997) assert.throws(() => decode(bytes.slice(0, i).buffer));
assert.throws(() => decode(new Uint8Array([...bytes, 0xf6]).buffer));
assert.throws(() => decode(new Uint8Array([0x78, 100, 97]).buffer));
assert.throws(() => decode(new Uint8Array([...Array(40).fill(0x81), 0xf6]).buffer));

// C++ must decode what JS encodes, and JS must decode what C++ encodes.
const tool = process.argv[2];
assert.ok(tool, 'pass the messages_test binary');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'messages-'));
try {
  fs.writeFileSync(path.join(dir, 'in'), bytes);
  execFileSync(tool, [path.join(dir, 'in'), path.join(dir, 'out')]);
  const back = fs.readFileSync(path.join(dir, 'out'));
  const fromCpp = decode(back.buffer.slice(back.byteOffset, back.byteOffset + back.byteLength));
  assert.deepEqual(fromCpp, values);
} finally {
  fs.rmSync(dir, {recursive: true});
}
console.log('messages.test.mjs: pass');
