// Where the browser packer's libraries come from: jsDelivr, at fixed versions. worker.js loads them and
// probe.js checks that this browser can reach them before the player starts.
export const PYODIDE_URL = 'https://cdn.jsdelivr.net/pyodide/v314.0.7/full/';
export const FFMPEG_URL = 'https://cdn.jsdelivr.net/npm/@ffmpeg/core@0.12.10/dist/esm/';
// ffmpeg.wasm from 0.12.1 on is built with WebAssembly SIMD, which browsers compile only on a processor
// with SSE4.1 ("Wasm SIMD unsupported" elsewhere); 0.12.0 is the last build without it (worker.js)
export const FFMPEG_NOSIMD_URL = 'https://cdn.jsdelivr.net/npm/@ffmpeg/core@0.12.0/dist/esm/';
