"use strict";
const pixels = require("./pixels.js");
const COMPONENT_ID = "com.advaet.gaslightr.native";

function createBridge(ps, loadNative) {
  let busy = false, cancelled = false, request = 0;
  const checkCancel = context => { if (cancelled || context.isCancelled) throw new Error("Enhancement cancelled."); };
  const modal = async (name, fn) => {
    if (busy) throw new Error("Gaslightr is already processing an image.");
    busy = true; cancelled = false;
    try { return await ps.core.executeAsModal(fn, { commandName: name }); }
    finally { busy = false; }
  };
  function native() {
    let api;
    try { api = loadNative(); } catch (error) {
      throw new Error(`The Gaslightr UXP native addon is not installed. Complete the SDK setup in docs/SDK_SETUP.md. ${error.message}`);
    }
    if (typeof api.processTile !== "function" || typeof api.capabilities !== "function") throw new Error("Incompatible Gaslightr addon.");
    return api;
  }
  // Invoked only inside the enclosing modal scope. Payloads are JSON strings in
  // ActionDescriptor string fields; pixel memory never travels through messaging.
  async function sendInModal(command, payload) {
    const id = String(++request);
    let timer, listener;
    const reply = new Promise((resolve, reject) => {
      listener = event => {
        if (event.pluginId !== COMPONENT_ID || !event.message || event.message.greq !== id) return;
        if (event.message.gerr) reject(new Error(event.message.gerr)); else resolve(event.message);
      };
      ps.messaging.addSDKMessagingListener(listener);
      timer = setTimeout(() => reject(new Error("The Photoshop SDK companion did not respond.")), 5000);
    });
    // Attach rejection handling before send: a send failure must not leave an
    // unobserved timeout promise behind.
    reply.catch(() => {});
    try {
      await ps.messaging.sendSDKPluginMessage(COMPONENT_ID, { greq: id, gcmd: command, gcfg: JSON.stringify(payload) });
      return await reply;
    } finally {
      clearTimeout(timer);
      ps.messaging.removeSDKMessagingListener(listener);
    }
  }
  function source() {
    if (!ps.app.documents.length) throw new Error("Open a document and select one pixel layer.");
    const document = ps.app.activeDocument;
    if (document.activeLayers.length !== 1) throw new Error("Select exactly one source layer.");
    const layer = document.activeLayers[0];
    if (layer.kind !== ps.constants.LayerKind.NORMAL) throw new Error("Select a raster pixel layer. Rasterize other layer types explicitly first.");
    return { document, layer, bounds: pixels.rectangle(layer.boundsNoEffects) };
  }
  async function tile(api, src, read, config, context) {
    checkCancel(context);
    const image = await ps.imaging.getPixels({ documentID: src.document.id, layerID: src.layer.id,
      sourceBounds: read, colorSpace: "RGB", colorProfile: pixels.PROFILE, componentSize: 32, applyAlpha: false });
    try {
      if (!image.imageData.width || !image.imageData.height) return null;
      if (image.level !== 0) throw new Error("Full-resolution pixel data was not returned.");
      const bounds = pixels.rectangle(image.sourceBounds), { width, height, components } = image.imageData;
      if (width > 2048 || height > 2048 || width !== bounds.right - bounds.left || height !== bounds.bottom - bounds.top)
        throw new Error("Unexpected Photoshop tile dimensions.");
      const input = pixels.rgba(await image.imageData.getData({ chunky: true }), width, height, components);
      checkCancel(context);
      const output = await api.processTile(input, { width, height, preset: config.preset, detailOnly: config.detailOnly });
      checkCancel(context);
      pixels.blend(input, output, width, height, bounds, src.bounds, config);
      return { input, output, bounds, width, height };
    } finally { image.imageData.dispose(); }
  }
  function validateCapabilities(api, config) {
    const caps = api.capabilities();
    if (caps.protocol !== 1) throw new Error("Gaslightr native protocol version mismatch.");
    if (config.detailOnly && !caps.detailOnly) throw new Error(caps.detailOnlyReason || "Detail-Only mode is unavailable in this build.");
    return caps;
  }
  return {
    isBusy: () => busy,
    cancel: () => { cancelled = true; },
    capabilities: () => native().capabilities(),
    sendSDKMessage: (command, payload) => modal("Gaslightr native message", () => sendInModal(command, payload)),
    preview: value => modal("Gaslightr preview", async context => {
      const config = pixels.settings(value), api = native(), caps = validateCapabilities(api, config), src = source();
      const centerX = Math.floor((src.bounds.left + src.bounds.right) / 2), centerY = Math.floor((src.bounds.top + src.bounds.bottom) / 2);
      const read = pixels.intersect(src.bounds, { left: centerX - 256, top: centerY - 256, right: centerX + 256, bottom: centerY + 256 });
      if (caps.sdkMessaging) await sendInModal("configure", { documentID: src.document.id, layerID: src.layer.id, viewport: read, ...config });
      return tile(api, src, read, config, context);
    }),
    apply: value => modal("Gaslightr enhancement", async context => {
      const config = pixels.settings(value), api = native(), caps = validateCapabilities(api, config), src = source();
      if (caps.sdkMessaging) await sendInModal("configure", { documentID: src.document.id, layerID: src.layer.id, bounds: src.bounds, ...config });
      const history = await context.hostControl.suspendHistory({ documentID: src.document.id, name: "Gaslightr enhancement" });
      let committed = false;
      try {
        const target = await src.document.createLayer({ name: `${src.layer.name} · Gaslightr` });
        const step = 960, total = Math.ceil((src.bounds.right - src.bounds.left) / step) * Math.ceil((src.bounds.bottom - src.bounds.top) / step);
        let completed = 0;
        for (const region of pixels.tiles(src.bounds)) {
          const result = await tile(api, src, region.read, config, context);
          if (result) {
            const core = pixels.intersect(region.core, result.bounds);
            if (core) {
              const data = pixels.crop(result.output, result.width, result.bounds, core);
              const imageData = await ps.imaging.createImageDataFromBuffer(data, { width: core.right - core.left, height: core.bottom - core.top,
                components: 4, chunky: true, colorSpace: "RGB", colorProfile: pixels.PROFILE });
              try {
                checkCancel(context);
                await ps.imaging.putPixels({ documentID: src.document.id, layerID: target.id, imageData, replace: false,
                  targetBounds: { left: core.left, top: core.top } });
              } finally { imageData.dispose(); }
            }
          }
          context.reportProgress({ value: ++completed / total });
        }
        checkCancel(context);
        await context.hostControl.resumeHistory(history, true);
        committed = true;
        return target.id;
      } finally {
        if (!committed) await context.hostControl.resumeHistory(history, false);
      }
    })
  };
}
module.exports = { createBridge, COMPONENT_ID };
