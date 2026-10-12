"use strict";

const PROFILE = "sRGB IEC61966-2.1";
function settings(value = {}) {
  const number = (key, fallback, max) => {
    const n = value[key] === undefined ? fallback : Number(value[key]);
    if (!Number.isFinite(n) || n < 0 || n > max) throw new Error(`Invalid ${key}`);
    return n;
  };
  const style = value.style || "default";
  if (!["default", "natural", "cinematic"].includes(style)) throw new Error("Invalid style");
  const preset = number("preset", 0, 2);
  if (!Number.isInteger(preset)) throw new Error("Invalid performance preset");
  return { intensity: number("intensity", 0.5, 1), structure: number("structure", 0, 1),
    feather: number("feather", 0, 32), preset, style, detailOnly: value.detailOnly === true };
}
function rectangle(b) {
  const result = { left: Math.floor(b.left), top: Math.floor(b.top), right: Math.ceil(b.right), bottom: Math.ceil(b.bottom) };
  if (!Object.values(result).every(Number.isSafeInteger) || result.right <= result.left || result.bottom <= result.top)
    throw new Error("The source layer has no valid pixel bounds.");
  return result;
}
function intersect(a, b) {
  const r = { left: Math.max(a.left, b.left), top: Math.max(a.top, b.top), right: Math.min(a.right, b.right), bottom: Math.min(a.bottom, b.bottom) };
  return r.right > r.left && r.bottom > r.top ? r : null;
}
function* tiles(bounds, size = 1024, halo = 32) {
  if (![1024, 2048].includes(size) || halo < 3 || halo * 2 >= size) throw new Error("Invalid tile dimensions");
  const step = size - halo * 2;
  for (let top = bounds.top; top < bounds.bottom; top += step) {
    for (let left = bounds.left; left < bounds.right; left += step) {
      const core = { left, top, right: Math.min(left + step, bounds.right), bottom: Math.min(top + step, bounds.bottom) };
      const read = intersect(bounds, { left: left - halo, top: top - halo, right: core.right + halo, bottom: core.bottom + halo });
      yield { core, read };
    }
  }
}
function rgba(data, width, height, components) {
  if (!(data instanceof Float32Array) || ![3, 4].includes(components) || data.length !== width * height * components)
    throw new Error("Expected chunky 32-bit RGB or RGBA pixels.");
  if (components === 4) return data;
  const result = new Float32Array(width * height * 4);
  for (let i = 0, j = 0; i < data.length; i += 3, j += 4) {
    result[j] = data[i]; result[j + 1] = data[i + 1]; result[j + 2] = data[i + 2]; result[j + 3] = 1;
  }
  return result;
}
function crop(data, width, bounds, core) {
  const w = core.right - core.left, h = core.bottom - core.top;
  const result = new Float32Array(w * h * 4);
  for (let y = 0; y < h; ++y) {
    const start = ((y + core.top - bounds.top) * width + core.left - bounds.left) * 4;
    result.set(data.subarray(start, start + w * 4), y * w * 4);
  }
  return result;
}
// Controls are a defined post-inference blend, not undocumented NGX parameters.
// A 7x7 luminance box filter supplies the structure signal; tile halos hide its support.
function blend(source, enhanced, width, height, bounds, layerBounds, config) {
  if (!(enhanced instanceof Float32Array) || enhanced.length !== source.length) throw new Error("Invalid native output buffer");
  const integral = new Float64Array((width + 1) * (height + 1));
  for (let y = 0; y < height; ++y) {
    let sum = 0;
    for (let x = 0; x < width; ++x) {
      const i = (y * width + x) * 4;
      for (let c = 0; c < 3; ++c) if (!Number.isFinite(enhanced[i + c])) throw new Error("The neural runtime returned non-finite pixels.");
      sum += 0.2126729 * enhanced[i] + 0.7151522 * enhanced[i + 1] + 0.0721750 * enhanced[i + 2];
      integral[(y + 1) * (width + 1) + x + 1] = integral[y * (width + 1) + x + 1] + sum;
    }
  }
  const strength = config.intensity * ({ default: 1, natural: 0.65, cinematic: 1.25 }[config.style]);
  for (let y = 0; y < height; ++y) for (let x = 0; x < width; ++x) {
    const i = (y * width + x) * 4;
    const x0 = Math.max(0, x - 3), y0 = Math.max(0, y - 3), x1 = Math.min(width, x + 4), y1 = Math.min(height, y + 4), stride = width + 1;
    const mean = (integral[y1 * stride + x1] - integral[y0 * stride + x1] - integral[y1 * stride + x0] + integral[y0 * stride + x0]) / ((x1 - x0) * (y1 - y0));
    const lum = 0.2126729 * enhanced[i] + 0.7151522 * enhanced[i + 1] + 0.0721750 * enhanced[i + 2];
    const distance = Math.min(bounds.left + x - layerBounds.left + 0.5, layerBounds.right - bounds.left - x - 0.5,
      bounds.top + y - layerBounds.top + 0.5, layerBounds.bottom - bounds.top - y - 0.5);
    const t = config.feather ? Math.max(0, Math.min(1, distance / config.feather)) : 1;
    const amount = strength * t * t * (3 - 2 * t);
    for (let c = 0; c < 3; ++c) enhanced[i + c] = source[i + c] + amount * (enhanced[i + c] + config.structure * (lum - mean) - source[i + c]);
    enhanced[i + 3] = source[i + 3];
  }
  return enhanced;
}
function displayBytes(pixels) {
  const result = new Uint8ClampedArray(pixels.length);
  for (let i = 0; i < pixels.length; ++i) {
    const value = pixels[i];
    const encoded = i % 4 === 3 ? value : (value <= 0.0031308 ? 12.92 * value : 1.055 * Math.pow(value, 1 / 2.4) - 0.055);
    result[i] = Math.round(Math.max(0, Math.min(1, encoded)) * 255);
  }
  return result;
}
module.exports = { PROFILE, settings, rectangle, intersect, tiles, rgba, crop, blend, displayBytes };
