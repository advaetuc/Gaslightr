"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const p = require("../uxp-frontend/pixels.js");
const { createBridge, COMPONENT_ID } = require("../uxp-frontend/bridge.js");

test("bounded tiles cover odd/negative layer bounds exactly once", () => {
  const b = { left: -3, top: -2, right: 1941, bottom: 970 };
  const coverage = new Uint8Array((b.right - b.left) * (b.bottom - b.top));
  for (const { core, read } of p.tiles(b)) {
    assert.ok(read.right - read.left <= 1024 && read.bottom - read.top <= 1024);
    for (let y = core.top; y < core.bottom; ++y) for (let x = core.left; x < core.right; ++x)
      ++coverage[(y - b.top) * (b.right - b.left) + x - b.left];
  }
  assert.ok(coverage.every(n => n === 1));
});
test("RGBA handoff preserves the view; RGB expansion supplies alpha", () => {
  const backing = new Float32Array([9, 9, 1, 2, 3, 0.5, 9]);
  const view = backing.subarray(2, 6);
  assert.equal(p.rgba(view, 1, 1, 4), view);
  assert.deepEqual([...p.rgba(new Float32Array([1, 2, 3]), 1, 1, 3)], [1, 2, 3, 1]);
  assert.throws(() => p.rgba(new Uint8Array(4), 1, 1, 4));
});
test("zero intensity preserves HDR, negative channels and alpha exactly", () => {
  const original = new Float32Array([-0.25, 4, 0.3, 0.25]);
  const result = p.blend(original, new Float32Array([1, 2, 3, 1]), 1, 1,
    { left: 0, top: 0 }, { left: 0, top: 0, right: 1, bottom: 1 }, p.settings({ intensity: 0, structure: 1 }));
  assert.deepEqual(result, original);
});
test("structure and feather agree across overlapping tiles", () => {
  const width = 1951, height = 5, bounds = { left: -13, top: 3, right: 1938, bottom: 8 };
  const src = new Float32Array(width * height * 4);
  for (let i = 0; i < src.length; i += 4) { src[i] = (i % 37) / 11; src[i + 1] = -0.1; src[i + 2] = 0.8; src[i + 3] = 0.3; }
  const config = p.settings({ intensity: 0.8, structure: 0.7, feather: 3, style: "cinematic" });
  const full = p.blend(src, src.slice(), width, height, bounds, bounds, config);
  for (const region of p.tiles(bounds)) {
    const input = p.crop(src, width, bounds, region.read);
    const output = p.blend(input, input.slice(), region.read.right - region.read.left, region.read.bottom - region.read.top,
      region.read, bounds, config);
    const actual = p.crop(output, region.read.right - region.read.left, region.read, region.core);
    const expected = p.crop(full, width, bounds, region.core);
    for (let i = 0; i < actual.length; ++i) assert.ok(Math.abs(actual[i] - expected[i]) < 1e-6);
  }
});
test("invalid settings and non-finite GPU data are rejected", () => {
  for (const settings of [{ intensity: NaN }, { feather: 33 }, { preset: 0.5 }, { style: "imaginary" }]) assert.throws(() => p.settings(settings));
  assert.throws(() => p.blend(new Float32Array(4), new Float32Array([NaN, 1, 1, 1]), 1, 1,
    { left: 0, top: 0 }, { left: 0, top: 0, right: 1, bottom: 1 }, p.settings()));
  assert.deepEqual([...p.displayBytes(new Float32Array([-1, 1, 4, 0.5]))], [0, 255, 255, 128]);
});

function host({ width = 1921, failTile = -1, trim = false } = {}) {
  const calls = [], listeners = new Set();
  let modal = false, count = 0, bridge;
  const check = () => assert.equal(modal, true, "Photoshop operation outside modal scope");
  const layer = { id: 7, name: "Source", kind: "normal", boundsNoEffects: { left: -3, top: 2, right: width - 3, bottom: 5 } };
  const doc = { id: 2, activeLayers: [layer], createLayer: async () => { check(); calls.push("create"); return { id: 8 }; } };
  const ctx = { isCancelled: false, reportProgress: value => { check(); calls.push(["progress", value.value]); }, hostControl: {
    suspendHistory: async () => { check(); calls.push("suspend"); return 91; },
    resumeHistory: async (id, commit) => { check(); assert.equal(id, 91); calls.push(["resume", commit]); }
  } };
  const imageData = (bounds, components = 4) => {
    const w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
    return { width: w, height: h, components,
      getData: async () => { check(); const d = new Float32Array(w * h * components); d.fill(0.5); return d; },
      dispose: () => { check(); calls.push("dispose"); } };
  };
  const ps = {
    core: { executeAsModal: async fn => { assert.equal(modal, false); modal = true; try { return await fn(ctx); } finally { modal = false; } } },
    app: { get documents() { check(); return [doc]; }, get activeDocument() { check(); return doc; } },
    constants: { LayerKind: { NORMAL: "normal" } },
    imaging: {
      getPixels: async options => {
        check(); calls.push(["get", options]);
        let b = { ...options.sourceBounds };
        if (trim) b.left += Math.min(1, b.right - b.left - 1);
        return { imageData: imageData(b), sourceBounds: b, level: 0 };
      },
      createImageDataFromBuffer: async (data, options) => {
        check(); assert.equal(data.length, options.width * options.height * 4);
        return { dispose: () => { check(); calls.push("writeDispose"); } };
      },
      putPixels: async options => { check(); calls.push(["put", options]); }
    },
    messaging: {
      addSDKMessagingListener: fn => { check(); listeners.add(fn); },
      removeSDKMessagingListener: fn => { check(); listeners.delete(fn); },
      sendSDKPluginMessage: async (id, message) => {
        check(); assert.equal(id, COMPONENT_ID); JSON.parse(message.gcfg);
        for (const fn of listeners) { fn({ pluginId: "unrelated", message }); fn({ pluginId: id, message: { greq: "stale" } }); fn({ pluginId: id, message }); }
      }
    }
  };
  const api = {
    capabilities: () => ({ protocol: 1, detailOnly: false, sdkMessaging: true }),
    processTile: async input => { check(); if (++count === failTile) throw new Error("GPU test failure"); if (api.cancel) bridge.cancel(); return input.slice(); }
  };
  bridge = createBridge(ps, () => api);
  return { bridge, api, ps, calls, ctx, listeners };
}
test("apply writes non-overlapping chunks and commits one history transaction", async () => {
  const h = host();
  assert.equal(await h.bridge.apply({}), 8);
  const writes = h.calls.filter(x => x[0] === "put");
  assert.equal(writes.length, 3);
  assert.deepEqual(writes.map(x => x[1].targetBounds.left), [-3, 957, 1917]);
  assert.ok(writes.every(x => x[1].replace === false && x[1].layerID === 8));
  assert.deepEqual(h.calls.at(-1), ["resume", true]);
  assert.equal(h.listeners.size, 0);
});
test("failed later tile disposes image data and rolls back the entire output", async () => {
  const h = host({ failTile: 2 });
  await assert.rejects(h.bridge.apply({}), /GPU test failure/);
  assert.deepEqual(h.calls.at(-1), ["resume", false]);
  assert.equal(h.calls.filter(x => x === "dispose").length, 2);
  assert.equal(h.bridge.isBusy(), false);
});
test("cancellation between GPU completion and writeback rolls back", async () => {
  const h = host(); h.api.cancel = true;
  await assert.rejects(h.bridge.apply({}), /cancelled/);
  assert.equal(h.calls.filter(x => x[0] === "put").length, 0);
  assert.deepEqual(h.calls.at(-1), ["resume", false]);
});
test("preview uses full-resolution bounds and never creates a layer", async () => {
  const h = host({ trim: true });
  const result = await h.bridge.preview({});
  assert.ok(result.width <= 512 && result.height <= 512);
  assert.ok(!h.calls.includes("create") && !h.calls.includes("suspend"));
});
test("unavailable detail mode fails before modifying Photoshop", async () => {
  const h = host();
  await assert.rejects(h.bridge.apply({ detailOnly: true }), /unavailable/);
  assert.equal(h.calls.length, 0);
});
test("SDK send failures remove listeners without dangling timeout rejections", async () => {
  const h = host();
  h.ps.messaging.sendSDKPluginMessage = async () => { throw new Error("companion missing"); };
  await assert.rejects(h.bridge.sendSDKMessage("configure", {}), /companion missing/);
  assert.equal(h.listeners.size, 0);
  assert.equal(h.bridge.isBusy(), false);
});
test("concurrent operations cannot enter a second modal scope", async () => {
  const h = host();
  const running = h.bridge.apply({});
  await assert.rejects(h.bridge.preview({}), /already processing/);
  await running;
});
