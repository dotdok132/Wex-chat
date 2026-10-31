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
