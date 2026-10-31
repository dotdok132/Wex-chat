/**
 * Wex-Chat Automated QR Generator & jsQR Decoder Test Suite
 */
const fs = require('fs');
const vm = require('vm');

const fakeWindow = {};
const qrEngineCode = fs.readFileSync(__dirname + '/../web/qr_engine.js', 'utf8');
const ctx = vm.createContext({ window: fakeWindow, globalThis: fakeWindow });
vm.runInContext(qrEngineCode, ctx);

class MockContext2D {
  constructor(canvas) {
    this.canvas = canvas;
    this.fillStyle = '#000000';
  }

  fillRect(x, y, w, h) {
    let r = 0, g = 0, b = 0;
    if (this.fillStyle === '#ffffff' || this.fillStyle === 'white') {
      r = 255; g = 255; b = 255;
    } else if (this.fillStyle === '#000000' || this.fillStyle === 'black') {
      r = 0; g = 0; b = 0;
    }
    for (let dy = 0; dy < h; dy++) {
      for (let dx = 0; dx < w; dx++) {
        const px = Math.floor(x + dx);
        const py = Math.floor(y + dy);
        if (px >= 0 && px < this.canvas.width && py >= 0 && py < this.canvas.height) {
          const idx = (py * this.canvas.width + px) * 4;
          this.canvas.pixels[idx] = r;
          this.canvas.pixels[idx + 1] = g;
          this.canvas.pixels[idx + 2] = b;
          this.canvas.pixels[idx + 3] = 255;
        }
      }
    }
  }

  getImageData() {
    return { data: this.canvas.pixels, width: this.canvas.width, height: this.canvas.height };
  }
}

class MockCanvas {
  constructor(width, height) {
    this.width = width;
    this.height = height;
    this.pixels = new Uint8ClampedArray(width * height * 4);
    this.ctx = new MockContext2D(this);
  }

  getContext(type) {
    return type === '2d' ? this.ctx : null;
  }
}

function renderKeyMatrixCanvasTest(canvas, payloadStr) {
  const qr = fakeWindow.qrcode(0, 'M');
  qr.addData(payloadStr);
  qr.make();

  const moduleCount = qr.getModuleCount();
  const quietZone = 4;
  const totalModules = moduleCount + quietZone * 2;

  const targetSize = canvas.width || 205;
  const cellSize = Math.max(1, Math.floor(targetSize / totalModules));
  const actualSize = cellSize * totalModules;
  const offset = Math.floor((targetSize - actualSize) / 2);

  const c2d = canvas.getContext('2d');
  c2d.fillStyle = '#ffffff';
  c2d.fillRect(0, 0, canvas.width, canvas.height);

  c2d.fillStyle = '#000000';
  for (let r = 0; r < moduleCount; r++) {
    for (let c = 0; c < moduleCount; c++) {
      if (qr.isDark(r, c)) {
        c2d.fillRect(
          offset + (c + quietZone) * cellSize,
          offset + (r + quietZone) * cellSize,
          cellSize,
          cellSize
        );
      }
    }
  }
}

const testPayloads = [
  'wex://?pk=d10c0e7d589d81648a33503b41dcf9427b2a59a96e810ab562854cf4f65c1a84',
  'wex://?pk=0000000000000000000000000000000000000000000000000000000000000000',
  'wex://?pk=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff&peer=192.168.1.50:9000',
  'wex://join/0000000000000000000000000000000000000000000000000000000000000000?peer=203.0.113.19:9001&wan=203.0.113.19:9001&lan=192.168.1.100:9001&pk=d10c0e7d589d81648a33503b41dcf9427b2a59a96e810ab562854cf4f65c1a84&name=WAN_LAN_Chat',
  'wex://d10c0e7d589d81648a33503b41dcf9427b2a59a96e810ab562854cf4f65c1a84?wan=198.51.100.42:9005&lan=10.0.0.5:9005',
  'wex://invite?code=community-alpha-123',
  'Simple text invite'
];

for (const payload of testPayloads) {
  for (const size of [180, 205, 256, 320]) {
    const canvas = new MockCanvas(size, size);
    renderKeyMatrixCanvasTest(canvas, payload);
    const ctx2d = canvas.getContext('2d');
    const imgData = ctx2d.getImageData();
    const decoded = fakeWindow.jsQR(imgData.data, size, size, { inversionAttempts: 'attemptBoth' });
    if (!decoded || decoded.data !== payload) {
      throw new Error(`Failed to decode payload: "${payload}" on canvas size ${size}, got: ${decoded ? decoded.data : null}`);
    }
  }
}

console.log('[PASS] All ' + (testPayloads.length * 4) + ' QR generation and decoding permutations decoded perfectly by jsQR!');

// Verify multi-endpoint parsing and normalization logic
function testMultiEndpointParsing() {
  function isValidHex64(str) {
    return typeof str === 'string' && /^[0-9a-fA-F]{64}$/.test(str.trim());
  }

  function parseTargetInput(rawInput) {
    const trimmed = (rawInput || '').trim();
    if (!trimmed) return { raw: '', pk: null, ipPort: null, wan: null, lan: null };
    if (isValidHex64(trimmed)) {
      return { raw: trimmed, pk: trimmed.toLowerCase(), ipPort: null, wan: null, lan: null };
    }
    const atIdx = trimmed.indexOf('@');
    if (atIdx === 64 && isValidHex64(trimmed.substring(0, 64))) {
      const ep = trimmed.substring(65);
      return { raw: trimmed, pk: trimmed.substring(0, 64).toLowerCase(), ipPort: ep, wan: ep, lan: null };
    }
    const pkMatch = trimmed.match(/[?&]pk=([0-9a-fA-F]{64})/);
    const peerMatch = trimmed.match(/[?&]peer=([^&#\s]+)/);
    const wanMatch = trimmed.match(/[?&]wan=([^&#\s]+)/);
    const lanMatch = trimmed.match(/[?&]lan=([^&#\s]+)/);
    let extractedPk = pkMatch ? pkMatch[1].toLowerCase() : null;
    let peerEp = peerMatch ? peerMatch[1] : null;
    let wanEp = wanMatch ? wanMatch[1] : null;
    let lanEp = lanMatch ? lanMatch[1] : null;
    if (!extractedPk && trimmed.startsWith('wex://')) {
      const hex64Match = trimmed.match(/\b([0-9a-fA-F]{64})\b/);
      if (hex64Match) extractedPk = hex64Match[1].toLowerCase();
    }
    if (extractedPk || peerEp || wanEp || lanEp) {
      return { raw: trimmed, pk: extractedPk, ipPort: peerEp || wanEp || lanEp, wan: wanEp, lan: lanEp };
    }
    return { raw: trimmed, pk: null, ipPort: trimmed, wan: null, lan: null };
  }

  function normalizeQrPayload(rawKeyOrUri) {
    const clean = (rawKeyOrUri || '').trim();
    if (!clean) return 'wex://?pk=' + '0'.repeat(64);
    if (isValidHex64(clean)) return 'wex://?pk=' + clean.toLowerCase();
    if (clean.startsWith('wex://')) return clean;
    const parsed = parseTargetInput(clean);
    if (parsed.pk) {
      let uri = 'wex://?pk=' + parsed.pk.toLowerCase();
      if (parsed.wan) uri += '&wan=' + encodeURIComponent(parsed.wan);
      if (parsed.lan) uri += '&lan=' + encodeURIComponent(parsed.lan);
      if (parsed.ipPort && !parsed.wan) uri += '&peer=' + encodeURIComponent(parsed.ipPort);
      return uri;
    }
    return clean;
  }

  const uri1 = 'wex://join/0000000000000000000000000000000000000000000000000000000000000000?peer=203.0.113.19:9001&wan=203.0.113.19:9001&lan=192.168.1.100:9001&pk=d10c0e7d589d81648a33503b41dcf9427b2a59a96e810ab562854cf4f65c1a84&name=WAN_LAN_Chat';
  const p1 = parseTargetInput(uri1);
  if (p1.wan !== '203.0.113.19:9001' || p1.lan !== '192.168.1.100:9001' || p1.pk !== 'd10c0e7d589d81648a33503b41dcf9427b2a59a96e810ab562854cf4f65c1a84') {
    throw new Error('Failed to parse uri1: ' + JSON.stringify(p1));
  }

  const uri2 = 'wex://d10c0e7d589d81648a33503b41dcf9427b2a59a96e810ab562854cf4f65c1a84?wan=198.51.100.42:9005&lan=10.0.0.5:9005';
  const p2 = parseTargetInput(uri2);
  if (p2.wan !== '198.51.100.42:9005' || p2.lan !== '10.0.0.5:9005' || p2.pk !== 'd10c0e7d589d81648a33503b41dcf9427b2a59a96e810ab562854cf4f65c1a84') {
    throw new Error('Failed to parse uri2: ' + JSON.stringify(p2));
  }

  const norm1 = normalizeQrPayload(uri1);
  if (norm1 !== uri1) {
    throw new Error('normalizeQrPayload corrupted multi-endpoint uri: ' + norm1);
  }

  console.log('[PASS] Multi-endpoint WAN/LAN parsing and QR payload preservation passed!');
}

testMultiEndpointParsing();
