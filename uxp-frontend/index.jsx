/* Plain JavaScript + UXP's built-in Spectrum controls; no JSX transform needed. */
"use strict";
const { createBridge } = require("./bridge.js");
const { displayBytes } = require("./pixels.js");
let addon;
const bridge = createBridge(require("photoshop"), () => {
  if (!addon) addon = require("gaslightr.uxpaddon");
  return addon;
});
const state = { intensity: 0.5, structure: 0, feather: 0, style: "default", preset: 0, detailOnly: false };
const element = id => document.getElementById(id);
let preview = null, split = 0.5, timer = null, revision = 0, pendingPreview = false, dragging = false;
const status = message => {
  element("status").textContent = message;
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
  const canvas = element("preview"), context = canvas.getContext("2d");
  if (!context || !preview) return;
  const { width, height } = preview;
  canvas.width = width; canvas.height = height;
  const frame = context.createImageData(width, height), cut = Math.round(width * split);
  frame.data.set(displayBytes(preview.output));
  const before = displayBytes(preview.input);
  for (let y = 0; y < height; ++y) frame.data.set(before.subarray(y * width * 4, (y * width + cut) * 4), y * width * 4);
  context.putImageData(frame, 0, 0);
}
async function run(apply) {
  if (bridge.isBusy()) return;
  clearTimeout(timer); pendingPreview = false;
  const current = ++revision;
  element("apply").disabled = element("refresh").disabled = true; element("cancel").disabled = false;
  status(apply ? "Enhancing layer…" : "Rendering preview…");
  try {
    if (apply) { await bridge.apply({ ...state }); status("Enhanced layer created."); }
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
const canvas = element("preview");
function compare(event) { const bounds = canvas.getBoundingClientRect(); split = Math.max(0, Math.min(1, (event.clientX - bounds.left) / bounds.width)); draw(); }
canvas.addEventListener("mousedown", event => { dragging = true; compare(event); });
canvas.addEventListener("mousemove", event => { if (dragging) compare(event); });
document.addEventListener("mouseup", () => { dragging = false; });
try {
  const caps = bridge.capabilities();
  console.log(`[Gaslightr] native capabilities ${JSON.stringify(caps)}`);
  element("detailOnly").disabled = !caps.detailOnly;
  element("detailStatus").textContent = caps.detailOnly ? "" : caps.detailOnlyReason;
} catch (error) {
  element("detailOnly").disabled = true;
  element("detailStatus").textContent = "Requires the native addon and validated CUDA interop.";
  status(error.message);
}
