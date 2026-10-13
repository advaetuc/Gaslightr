"use strict";
const pixels = require("./pixels.js");
const COMPONENT_ID = "com.advaet.gaslightr.native";

function createBridge(ps, loadNative) {
  let busy = false, cancelled = false, request = 0;
  const checkCancel = context => { if (cancelled || context.isCancelled) throw new Error("Enhancement cancelled."); };
  async function pixelOperation(stage, documentID, layerID, bounds, operation) {
    try { return await operation(); }
    catch (error) {
      const message = `${stage} failed (document ${documentID}, layer ${layerID}, bounds ${JSON.stringify(bounds)}): ${error.message || String(error)}`;
      console.error(`[Gaslightr] ${message}`);
      throw new Error(message);
    }
  }
  const modal = async (name, fn) => {
    if (busy) throw new Error("Gaslightr is already processing an image.");
    busy = true; cancelled = false;
    try { return await ps.core.executeAsModal(fn, { commandName: name }); }
    finally { busy = false; }
  };
  async function native() {
    let api;
    try { api = await loadNative(); } catch (error) {
      throw new Error(`Cannot load the Gaslightr native addon: ${error.message || String(error)}`);
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
    // Snapshot primitive IDs before createLayer changes the active selection.
    return { document, documentID: document.id, layerID: layer.id, layerName: layer.name,
      bounds: pixels.rectangle(layer.boundsNoEffects) };
  }
  async function tile(api, src, read, config, context) {
    checkCancel(context);
    const image = await pixelOperation("Source tile read", src.documentID, src.layerID, read,
      () => ps.imaging.getPixels({ documentID: src.documentID, layerID: src.layerID,
        sourceBounds: read, colorSpace: "RGB", colorProfile: pixels.PROFILE, componentSize: 32, applyAlpha: false }));
    try {
      if (!image.imageData.width || !image.imageData.height) return null;
      if (image.level !== 0) throw new Error("Full-resolution pixel data was not returned.");
      const bounds = pixels.rectangle(image.sourceBounds), { width, height, components } = image.imageData;
      if (width > 2048 || height > 2048 || width !== bounds.right - bounds.left || height !== bounds.bottom - bounds.top)
        throw new Error("Unexpected Photoshop tile dimensions.");
      // Own the bytes retained by preview after PhotoshopImageData.dispose().
      const input = new Float32Array(pixels.rgba(await image.imageData.getData({ chunky: true }), width, height, components));
      const isValidation = config.mode === "validation" || (config.mode !== "enhancement" && api.capabilities().executionMode === "d3d12-validation-copy");
      const output = await api.processTile(input, {
        width, height, preset: config.preset, detailOnly: config.detailOnly,
        intensity: config.intensity, structure: config.structure,
        validationOnly: isValidation
      });
      checkCancel(context);
      if (!(output instanceof Float32Array) || output.length !== input.length)
        throw new Error("Native output is not a complete RGBA32F tile.");
      for (let i = 0; i < output.length; ++i) {
        if (!Number.isFinite(output[i])) throw new Error("Native output contains non-finite pixels.");
        if (i % 4 === 3 && output[i] !== input[i])
          throw new Error("Native output changed source alpha; output layer was rolled back.");
      }
      // Validation mode exposes raw GPU readback without CPU blending.
      // If native addon did not apply enhancement (e.g. in offline mock), apply JS blend fallback.
      if (!isValidation && !api.capabilities().nativeEnhancement)
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
    capabilities: async () => (await native()).capabilities(),
    checkNative: () => modal("Gaslightr native diagnostics", async () => require("./native-checks.js").checkNativeBoundary(await native())),
    sendSDKMessage: (command, payload) => modal("Gaslightr native message", () => sendInModal(command, payload)),
    preview: value => modal("Gaslightr preview", async context => {
      const config = pixels.settings(value), api = await native(), caps = validateCapabilities(api, config), src = source();
      const centerX = Math.floor((src.bounds.left + src.bounds.right) / 2), centerY = Math.floor((src.bounds.top + src.bounds.bottom) / 2);
      const read = pixels.intersect(src.bounds, { left: centerX - 256, top: centerY - 256, right: centerX + 256, bottom: centerY + 256 });
      if (caps.sdkMessaging) await sendInModal("configure", { documentID: src.documentID, layerID: src.layerID, viewport: read, ...config });
      return tile(api, src, read, config, context);
    }),
    apply: value => modal("Gaslightr enhancement", async context => {
      const config = pixels.settings(value), api = await native(), caps = validateCapabilities(api, config), src = source();
      if (caps.sdkMessaging) await sendInModal("configure", { documentID: src.documentID, layerID: src.layerID, bounds: src.bounds, ...config });
      const validation = config.mode === "validation" || (config.mode !== "enhancement" && caps.executionMode === "d3d12-validation-copy");
      const history = await context.hostControl.suspendHistory({ documentID: src.documentID,
        name: validation ? "Gaslightr GPU validation" : "Gaslightr enhancement" });
      let committed = false;
      try {
        let targetID = null, witness = null, writes = 0;
        const step = 960, total = Math.ceil((src.bounds.right - src.bounds.left) / step) * Math.ceil((src.bounds.bottom - src.bounds.top) / step);
        let completed = 0;
        for (const region of pixels.tiles(src.bounds)) {
          const result = await tile(api, src, region.read, config, context);
          if (result) {
            const core = pixels.intersect(region.core, result.bounds);
            if (core) {
              const data = pixels.crop(result.output, result.width, result.bounds, core);
              const coreWidth = core.right - core.left;
              for (let i = 3; i < data.length; i += 4) {
                if (data[i] > (witness ? witness.alpha : 0)) {
                  const pixel = (i - 3) / 4;
                  witness = { bounds: { ...core }, alpha: data[i], left: core.left + pixel % coreWidth,
                    top: core.top + Math.floor(pixel / coreWidth) };
                }
              }
              // Read/process the first source tile before creating the target.
              if (targetID === null) {
                const target = await src.document.createLayer({ name: `${src.layerName} · Gaslightr${validation ? " GPU validation" : ""}` });
                targetID = target.id;
                if (!Number.isSafeInteger(targetID) || targetID === src.layerID)
                  throw new Error("Photoshop did not provide a distinct output layer ID.");
              }
              const imageData = await pixelOperation("Output buffer creation", src.documentID, targetID, core,
                () => ps.imaging.createImageDataFromBuffer(data, { width: core.right - core.left, height: core.bottom - core.top,
                  components: 4, chunky: true, colorSpace: "RGB", colorProfile: pixels.PROFILE }));
              try {
                checkCancel(context);
                // Initialize the empty layer once, then retain all earlier tiles.
                await pixelOperation("Output tile write", src.documentID, targetID, core,
                  () => ps.imaging.putPixels({ documentID: src.documentID, layerID: targetID, imageData, replace: writes === 0,
                    targetBounds: { left: core.left, top: core.top } }));
                ++writes;
              } finally { imageData.dispose(); }
            }
          }
          context.reportProgress({ value: ++completed / total });
        }
        checkCancel(context);
        if (!writes || !witness)
          throw new Error(`Source layer ${src.layerID} supplied no visible pixels. Select the original populated pixel layer; empty output was rolled back.`);
        // A successful putPixels call alone must not commit a blank layer.
        // Read a bounded written tile, then locate the witness in Photoshop's
        // returned bounds. Do not assume that a readback has a 1x1 allocation.
        const probeBounds = witness.bounds;
        const probe = await pixelOperation(`Output verification after ${writes} tile writes`, src.documentID, targetID, probeBounds,
          () => ps.imaging.getPixels({ documentID: src.documentID, layerID: targetID,
            sourceBounds: probeBounds, colorSpace: "RGB", colorProfile: pixels.PROFILE, componentSize: 32, applyAlpha: false }));
        try {
          const { width, height, components } = probe.imageData;
          const bounds = probe.sourceBounds;
          const diagnostic = `layer=${targetID}, size=${width}x${height}, components=${components}, level=${String(probe.level)}, requested=${JSON.stringify(probeBounds)}, returned=${JSON.stringify(bounds)}`;
          if (probe.level !== 0 || !Number.isSafeInteger(width) || !Number.isSafeInteger(height) ||
              width < 1 || height < 1 || width > 2048 || height > 2048 || !bounds ||
              ![bounds.left, bounds.top, bounds.right, bounds.bottom].every(Number.isSafeInteger) ||
              width !== bounds.right - bounds.left || height !== bounds.bottom - bounds.top)
            throw new Error(`Writeback verification received unexpected image metadata (${diagnostic}); operation rolled back.`);
          if (witness.left < bounds.left || witness.left >= bounds.right || witness.top < bounds.top || witness.top >= bounds.bottom)
            throw new Error(`Writeback verification did not return the written pixel at ${witness.left},${witness.top} (${diagnostic}); operation rolled back.`);
          const sample = pixels.rgba(await probe.imageData.getData({ chunky: true }), width, height, components);
          const offset = ((witness.top - bounds.top) * width + witness.left - bounds.left) * 4;
          if (!Number.isFinite(sample[offset + 3]) || sample[offset + 3] <= 0)
            throw new Error(`Writeback verification found transparent output on layer ${targetID}; operation rolled back.`);
          if (![sample[offset], sample[offset + 1], sample[offset + 2]].every(Number.isFinite))
            throw new Error(`Writeback verification found non-finite RGB on layer ${targetID}; operation rolled back.`);
        } finally { probe.imageData.dispose(); }
        checkCancel(context);
        console.log(`[Gaslightr] verified writeback: source=${src.layerID}, target=${targetID}, tiles=${writes}`);
        await context.hostControl.resumeHistory(history, true);
        committed = true;
        return targetID;
      } finally {
        if (!committed) await context.hostControl.resumeHistory(history, false);
      }
    })
  };
}
module.exports = { createBridge, COMPONENT_ID };
