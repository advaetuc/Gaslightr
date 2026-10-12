"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const addon = require("../build/Release/gaslightr.node");
const options = { width: 1, height: 1, preset: 0, detailOnly: false };
test("compiled Node binding reports its actual capabilities", () => {
  assert.equal(addon.capabilities().protocol, 1);
  assert.equal(addon.capabilities().detailOnly, false);
  assert.equal(addon.capabilities().sdkMessaging, false);
});
test("compiled boundary rejects malformed types, lengths, dimensions, and HDR overflow before GPU work", () => {
  assert.throws(() => addon.processTile(), /requires/);
  assert.throws(() => addon.processTile(new Uint8Array(4), options), /Float32Array/);
  assert.throws(() => addon.processTile(new Float32Array(3), options), /components/);
  const valid = new Float32Array([0.1, 0.2, 0.3, 1]);
  for (const overrides of [{ width: 0 }, { width: 2049 }, { width: 1.5 }, { preset: -1 }, { detailOnly: true }])
    assert.throws(() => addon.processTile(valid, { ...options, ...overrides }));
  assert.throws(() => addon.processTile(new Float32Array([Infinity, 0, 0, 1]), options), /non-finite/);
  assert.throws(() => addon.processTile(new Float32Array([65505, 0, 0, 1]), options), /float16/);
  assert.throws(() => addon.processTile(new Float32Array(new SharedArrayBuffer(16)), options), /SharedArrayBuffer/);
});
test("subarray offset is honored and missing runtime fails before device initialization", {
  skip: fs.existsSync(path.resolve(__dirname, "../build/Release/nvngx_dlssnr.dll"))
}, () => {
  const pixels = new Float32Array([NaN, NaN, NaN, NaN, 0, 0.2, 0.3, 1, NaN]);
  assert.throws(() => addon.processTile(pixels.subarray(4, 8), options), /dll is missing/);
});
