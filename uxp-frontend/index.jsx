/* Plain JavaScript + UXP's built-in Spectrum controls; no JSX transform needed. */
"use strict";
const { createBridge } = require("./bridge.js");
const { displayBytes } = require("./pixels.js");
const { createPreviewBlob } = require("./preview-image.js");
let addon;
const bridge = createBridge(require("photoshop"), async () => {
  if (!addon) addon = await require("gaslightr.uxpaddon");
  return addon;
});
const state = { intensity: 0.5, structure: 0, feather: 0, style: "default", preset: 0, detailOnly: false };
const element = id => document.getElementById(id);
let preview = null, split = 0.5, timer = null, revision = 0, pendingPreview = false, dragging = false, validationMode = false;
let previewUrl = null;
const status = message => {
  element("status").value = String(message);
  element("status").scrollTop = 0;
  console.log(`[Gaslightr] ${new Date().toISOString()} ${message}`);
};
console.log("[Gaslightr] panel script loaded");
for (const [key, label, max, step] of [["intensity", "NR Intensity", 1, 0.01], ["structure", "Local Tone / Structure", 1, 0.01], ["feather", "Mask Feathering · layer edges (px)", 32, 1]]) {
  const control = document.createElement("div"); control.className = "control";
  const caption = document.createElement("label"); caption.textContent = label;
  const row = document.createElement("div"); row.className = "control-row";
  const slider = document.createElement("sp-slider"), number = document.createElement("input");
  number.type = "number";
  for (const field of [slider, number]) {
    field.setAttribute("min", "0"); field.setAttribute("max", String(max)); field.setAttribute("step", String(step));
    field.setAttribute("aria-label", label); field.value = state[key];
    field.addEventListener("input", () => {
      const next = Number(field.value);
      if (!Number.isFinite(next)) return;
      state[key] = Math.max(0, Math.min(max, next)); slider.value = state[key]; number.value = state[key]; schedule();
    });
    row.appendChild(field);
  }
  control.appendChild(caption); control.appendChild(row); element("controls").appendChild(control);
}
for (const key of ["style", "preset", "detailOnly"]) element(key).addEventListener("change", () => {
  state[key] = key === "detailOnly" ? element(key).checked : key === "preset" ? Number(element(key).value) : element(key).value;
  schedule();
});
function schedule() {
  ++revision; pendingPreview = true; clearTimeout(timer);
  timer = setTimeout(() => { if (!bridge.isBusy()) run(false); }, 350);
}
function draw() {
  const image = element("preview");
  if (!preview) {
    image.removeAttribute("src");
    if (previewUrl) URL.revokeObjectURL(previewUrl);
    previewUrl = null;
    return;
  }
  const { width, height } = preview;
  const frame = displayBytes(preview.output), cut = Math.round(width * split);
  const before = displayBytes(preview.input);
  for (let y = 0; y < height; ++y)
    frame.set(before.subarray(y * width * 4, (y * width + cut) * 4), y * width * 4);
  const blob = createPreviewBlob(frame, width, height, ImageBlob);
  const nextUrl = URL.createObjectURL(blob);
  try { image.src = nextUrl; }
  catch (error) { URL.revokeObjectURL(nextUrl); throw error; }
  const previousUrl = previewUrl;
  previewUrl = nextUrl;
  if (previousUrl) URL.revokeObjectURL(previousUrl);
}
async function run(apply) {
  if (bridge.isBusy()) return;
  clearTimeout(timer); pendingPreview = false;
  const current = ++revision;
  element("apply").disabled = element("refresh").disabled = true; element("cancel").disabled = false;
  status(apply ? (validationMode ? "Running GPU validation…" : "Enhancing layer…") : "Rendering preview…");
  try {
    if (apply) { await bridge.apply({ ...state }); status(validationMode ? "GPU validation layer created. No neural inference ran." : "Enhanced layer created."); }
    else {
      const result = await bridge.preview({ ...state });
      if (revision === current) { preview = result; draw(); status(result ? "Drag across the preview to compare." : "No pixels in the preview region."); }
    }
  } catch (error) { status(error.message || String(error)); }
  finally {
    element("apply").disabled = element("refresh").disabled = false; element("cancel").disabled = true;
    if (pendingPreview) schedule();
  }
}
element("refresh").addEventListener("click", () => run(false));
element("nativeCheck").addEventListener("click", async () => {
  if (bridge.isBusy()) return;
  element("nativeCheck").disabled = true;
  try { status(await bridge.checkNative()); }
  catch (error) { status(error.message || String(error)); }
  finally { element("nativeCheck").disabled = false; }
});
element("apply").addEventListener("click", () => run(true));
element("cancel").addEventListener("click", () => { pendingPreview = false; clearTimeout(timer); ++revision; bridge.cancel(); status("Cancelling after the current tile…"); });
const previewImage = element("preview");
function compare(event) {
  const bounds = previewImage.getBoundingClientRect();
  if (!preview || bounds.width <= 0) return;
  split = Math.max(0, Math.min(1, (event.clientX - bounds.left) / bounds.width));
  try { draw(); }
  catch (error) { dragging = false; status(error.message || String(error)); }
}
previewImage.addEventListener("mousedown", event => { dragging = true; compare(event); });
previewImage.addEventListener("mousemove", event => { if (dragging) compare(event); });
previewImage.addEventListener("error", () => status("Preview image could not be displayed. Document processing is unaffected."));
window.addEventListener("unload", () => {
  if (previewUrl) URL.revokeObjectURL(previewUrl);
  previewUrl = null;
});
document.addEventListener("mouseup", () => { dragging = false; });
async function initializeNativeStatus() {
  try {
    const caps = await bridge.capabilities();
    console.log(`[Gaslightr] native capabilities ${JSON.stringify(caps)}`);
    validationMode = caps.executionMode === "d3d12-validation-copy";
    element("backendStatus").textContent = validationMode
      ? "GPU validation mode · neural inference off"
      : (caps.neuralInference === false ? caps.neuralInferenceReason : "Neural processing");
    if (validationMode) {
      element("enhancementSettings").style.display = "none";
      element("precisionNote").textContent = "RGB passes through FP16 textures. Original alpha is preserved.";
      element("apply").textContent = "Create validation layer";
      for (const control of document.querySelectorAll("#controls sp-slider, #controls input, #style, #preset")) control.disabled = true;
    }
    element("detailOnly").disabled = !caps.detailOnly;
    element("detailStatus").textContent = caps.detailOnly ? "" : caps.detailOnlyReason;
  } catch (error) {
    element("backendStatus").textContent = "Native processing unavailable.";
    element("detailOnly").disabled = true;
    element("detailStatus").textContent = "Requires the native addon and validated CUDA interop.";
    status(error.message);
  }
}
initializeNativeStatus();
