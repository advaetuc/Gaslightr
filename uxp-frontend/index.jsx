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

const state = { intensity: 0.5, structure: 0, feather: 0, style: "default", preset: 0, detailOnly: false, mode: "enhancement" };
const element = id => document.getElementById(id);

let preview = null, split = 0.5, timer = null, revision = 0, pendingPreview = false;
let previewUrlAfter = null, previewUrlBefore = null, isDragging = false;

const status = message => {
  const el = element("status");
  if (el) {
    el.value = String(message);
    el.scrollTop = 0;
  }
  console.log(`[Gaslightr] ${new Date().toISOString()} ${message}`);
};

console.log("[Gaslightr] panel script loaded");

// Dynamic generation of slider controls
for (const [key, label, max, step] of [
  ["intensity", "NR Intensity", 1, 0.01],
  ["structure", "Local Tone / Structure", 1, 0.01],
  ["feather", "Mask Feathering · layer edges (px)", 32, 1]
]) {
  const control = document.createElement("div");
  control.className = "control";
  const caption = document.createElement("label");
  caption.textContent = label;
  const row = document.createElement("div");
  row.className = "control-row";
  const slider = document.createElement("sp-slider");
  const number = document.createElement("input");
  number.type = "number";

  for (const field of [slider, number]) {
    field.setAttribute("min", "0");
    field.setAttribute("max", String(max));
    field.setAttribute("step", String(step));
    field.setAttribute("aria-label", label);
    field.value = state[key];
    field.addEventListener("input", () => {
      const next = Number(field.value);
      if (!Number.isFinite(next)) return;
      state[key] = Math.max(0, Math.min(max, next));
      slider.value = state[key];
      number.value = state[key];
      schedule();
    });
    row.appendChild(field);
  }
  control.appendChild(caption);
  control.appendChild(row);
  element("controls").appendChild(control);
}

// Event listeners for selects and checkboxes
for (const key of ["style", "preset", "detailOnly", "mode"]) {
  const el = element(key);
  if (el) {
    el.addEventListener("change", () => {
      state[key] = key === "detailOnly" ? el.checked : key === "preset" ? Number(el.value) : el.value;
      if (key === "mode") updateModeUI();
      schedule();
    });
  }
}

function updateModeUI() {
  const isValidation = state.mode === "validation";
  const enhancement = element("enhancementSettings");
  if (enhancement) enhancement.style.display = isValidation ? "none" : "";

  const note = element("precisionNote");
  if (note) {
    note.textContent = isValidation
      ? "RGB passes through FP16 textures. Original alpha is preserved."
      : "Linear float32 pipeline · 7×7 luminance structure filter · smoothstep feathering.";
  }

  const applyBtn = element("apply");
  if (applyBtn) {
    applyBtn.textContent = isValidation ? "Create validation layer" : "Enhance layer";
  }

  const backend = element("backendStatus");
  if (backend) {
    backend.textContent = isValidation
      ? "GPU validation mode · raw D3D12 copy · neural inference off"
      : "Local enhancement mode · GPU transport + 7×7 structure filter";
  }

  for (const control of document.querySelectorAll("#controls sp-slider, #controls input, #style, #preset")) {
    control.disabled = isValidation;
  }
}

function schedule() {
  ++revision;
  pendingPreview = true;
  clearTimeout(timer);
  timer = setTimeout(() => {
    if (!bridge.isBusy()) run(false);
  }, 350);
}

// Split slider update
function updateDivider() {
  const pct = Math.max(0, Math.min(100, split * 100));
  const divider = element("previewDivider");
  const beforeImg = element("previewBefore");
  if (divider) divider.style.left = `${pct}%`;
  if (beforeImg) {
    beforeImg.style.clipPath = `inset(0 ${100 - pct}% 0 0)`;
    beforeImg.style.webkitClipPath = `inset(0 ${100 - pct}% 0 0)`;
  }
}

function draw() {
  const afterImg = element("preview");
  const beforeImg = element("previewBefore");

  if (!preview) {
    if (afterImg) afterImg.removeAttribute("src");
    if (beforeImg) beforeImg.removeAttribute("src");
    if (previewUrlAfter) URL.revokeObjectURL(previewUrlAfter);
    if (previewUrlBefore) URL.revokeObjectURL(previewUrlBefore);
    previewUrlAfter = null;
    previewUrlBefore = null;
    return;
  }

  const { width, height } = preview;

  // Generate output blob (After)
  const afterBytes = displayBytes(preview.output);
  const afterBlob = createPreviewBlob(afterBytes, width, height, ImageBlob);
  const nextAfterUrl = URL.createObjectURL(afterBlob);

  // Generate input blob (Before)
  const beforeBytes = displayBytes(preview.input);
  const beforeBlob = createPreviewBlob(beforeBytes, width, height, ImageBlob);
  const nextBeforeUrl = URL.createObjectURL(beforeBlob);

  try {
    if (afterImg) afterImg.src = nextAfterUrl;
    if (beforeImg) beforeImg.src = nextBeforeUrl;
  } catch (error) {
    URL.revokeObjectURL(nextAfterUrl);
    URL.revokeObjectURL(nextBeforeUrl);
    throw error;
  }

  const prevAfter = previewUrlAfter;
  const prevBefore = previewUrlBefore;
  previewUrlAfter = nextAfterUrl;
  previewUrlBefore = nextBeforeUrl;

  if (prevAfter) URL.revokeObjectURL(prevAfter);
  if (prevBefore) URL.revokeObjectURL(prevBefore);

  updateDivider();
}

async function run(apply) {
  if (bridge.isBusy()) return;
  clearTimeout(timer);
  pendingPreview = false;
  const current = ++revision;
  const isValidation = state.mode === "validation";

  element("apply").disabled = element("refresh").disabled = true;
  element("cancel").disabled = false;
  status(apply ? (isValidation ? "Running GPU validation…" : "Enhancing layer…") : "Rendering preview…");

  try {
    if (apply) {
      await bridge.apply({ ...state });
      status(isValidation ? "GPU validation layer created. No neural inference ran." : "Enhanced layer created.");
    } else {
      const result = await bridge.preview({ ...state });
      if (revision === current) {
        preview = result;
        draw();
        status(result ? "Drag divider to compare before and after." : "No pixels in the preview region.");
      }
    }
  } catch (error) {
    status(error.message || String(error));
  } finally {
    element("apply").disabled = element("refresh").disabled = false;
    element("cancel").disabled = true;
    if (pendingPreview) schedule();
  }
}

element("refresh").addEventListener("click", () => run(false));
element("nativeCheck").addEventListener("click", async () => {
  if (bridge.isBusy()) return;
  element("nativeCheck").disabled = true;
  try {
    status(await bridge.checkNative());
  } catch (error) {
    status(error.message || String(error));
  } finally {
    element("nativeCheck").disabled = false;
  }
});

element("apply").addEventListener("click", () => run(true));
element("cancel").addEventListener("click", () => {
  pendingPreview = false;
  clearTimeout(timer);
  ++revision;
  bridge.cancel();
  status("Cancelling after the current tile…");
});

// Pointer drag interaction with setPointerCapture
const stage = element("previewStage");
function setSplitFromPointer(clientX) {
  if (!stage || !preview) return;
  const rect = stage.getBoundingClientRect();
  if (rect.width <= 0) return;
  const clampedX = Math.max(0, Math.min(rect.width, clientX - rect.left));
  split = clampedX / rect.width;
  updateDivider();
}

if (stage) {
  stage.addEventListener("pointerdown", event => {
    if (!preview) return;
    isDragging = true;
    try {
      stage.setPointerCapture(event.pointerId);
    } catch (_) {}
    setSplitFromPointer(event.clientX);
  });

  stage.addEventListener("pointermove", event => {
    if (!isDragging) return;
    setSplitFromPointer(event.clientX);
  });

  const endDrag = event => {
    if (!isDragging) return;
    isDragging = false;
    try {
      if (event && event.pointerId) stage.releasePointerCapture(event.pointerId);
    } catch (_) {}
  };

  stage.addEventListener("pointerup", endDrag);
  stage.addEventListener("pointercancel", endDrag);
  window.addEventListener("pointerup", endDrag);
}

const previewAfterImg = element("preview");
if (previewAfterImg) {
  previewAfterImg.addEventListener("error", () => {
    status("Preview image could not be displayed. Document processing is unaffected.");
  });
}

window.addEventListener("unload", () => {
  if (previewUrlAfter) URL.revokeObjectURL(previewUrlAfter);
  if (previewUrlBefore) URL.revokeObjectURL(previewUrlBefore);
  previewUrlAfter = null;
  previewUrlBefore = null;
});

async function initializeNativeStatus() {
  try {
    const caps = await bridge.capabilities();
    console.log(`[Gaslightr] native capabilities ${JSON.stringify(caps)}`);
    updateModeUI();
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
