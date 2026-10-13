"use strict";
// Every case fails before runtime loading or GPU work. Safe without an NGX DLL.
function checkNativeBoundary(api) {
  const caps = api.capabilities();
  if (caps.protocol !== 1 || caps.detailOnly !== false || caps.sdkMessaging !== false)
    throw new Error("Unexpected addon capabilities.");
  const config = { width: 1, height: 1, preset: 0, detailOnly: false };
  const viewStorage = new Float32Array([NaN, NaN, 0.2, 0.3, 0.4, -0.1, NaN]);
  const checks = [
    ["wrong array type", new Uint8Array(4), config, /Float32Array/],
    ["wrong length", new Float32Array(3), config, /components/],
    ["zero width", new Float32Array(4), { ...config, width: 0 }, /dimensions/],
    ["nonzero byte offset", viewStorage.subarray(2, 6), config, /Alpha/],
    ["non-finite input", new Float32Array([NaN, 0, 0, 1]), config, /non-finite/],
    ["detail-only rejection", new Float32Array([0, 0, 0, 1]), { ...config, detailOnly: true }, /Detail-Only/]
  ];
  for (const [name, input, options, expected] of checks) {
    let rejected = false;
    try { api.processTile(input, options); }
    catch (error) {
      if (!expected.test(error.message)) throw new Error(`${name}: unexpected error: ${error.message}`);
      rejected = true;
    }
    if (!rejected) throw new Error(`${name}: native addon accepted invalid input.`);
    console.log(`[Gaslightr] native check passed: ${name}`);
  }
  return `${checks.length} native boundary checks passed; GPU inference was not run.`;
}
module.exports = { checkNativeBoundary };
