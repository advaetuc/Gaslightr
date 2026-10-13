"use strict";

function createPreviewBlob(bytes, width, height, BlobType) {
  if (!(bytes instanceof Uint8Array) || bytes.length !== width * height * 4)
    throw new Error("Expected 8-bit RGBA preview data.");
  // Use a dedicated, exact-size allocation, including when passed a subarray.
  const uint8 = new Uint8Array(bytes);
  const options = {
    type: "image/uncompressed", width, height, components: 4,
    componentSize: 8, pixelFormat: "RGBA", colorSpace: "RGB",
    colorProfile: "sRGB IEC61966-2.1", hasAlpha: true
  };
  try { return new BlobType(uint8.buffer, options); }
  catch (error) {
    // Adobe's API table specifies ArrayBuffer, while its runnable examples
    // pass a typed array. Some Photoshop hosts enforce the latter form.
    if (!/invalid type.*arraybuffer/i.test(String(error.message))) throw error;
    return new BlobType(uint8, options);
  }
}

module.exports = { createPreviewBlob };
