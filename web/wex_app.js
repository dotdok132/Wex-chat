/**
 * Wex-Chat — Friendly Consumer Messenger Controller
 * File: web/wex_app.js
 * Clean, cozy, human-friendly UI with ISO/IEC 18004 QR Generator & In-App Camera/Image QR Scanner.
 * Strictly zero emojis across the entire file.
 */

(function () {
  'use strict';

  const CONTACTS_STORAGE_KEY = 'wex_contacts_v2';

  /* ======================== APPLICATION STATE ======================== */
  let ws = null;
  let reconnectTimer = null;

  let activeTab = 'dms'; // 'dms' | 'swarms' | 'net' | 'key'
  let activeChatMode = 'dm'; // 'dm' | 'swarm'
  let currentSpaceId = '';
  let currentChannel = 'general';
  let currentDmPeerPk = null;

  /* Local Profile & Connection State */
  let myNick = 'Пользователь';
  let myStatus = 'В сети';
  let myUdpPort = 9001;
  let myPublicKey = '';
  let myX25519Key = '';
  let myInviteUri = '';
  let myWanEndpoint = '';
  let myDhtCount = 0;
  let myOfflineShards = 0;
  let myExportedSeed = '';
  window.myPublicKey = '';

  /* Collections */
  let contacts = {}; // keyed by 64-char public_key: { public_key, nickname, ip, port, online, use_relay, last_msg, last_ts, unread }
  let spacesCache = [];
  let peersCache = [];
  let dhtNodesCache = [];
  let historyCache = [];
  let blobCache = {};

  /* Voice & Audio State */
  let voiceConnected = false;
  let voiceChannelName = 'voice-lounge';
  let audioCtx = null;
  let micStream = null;

  /* QR Scanner State */
  let qrScannerStream = null;
  let qrScannerTimer = null;
  let scannerFacingMode = 'environment';
  let barcodeDetectorInstance = null;

  /* Modal State */
  let pendingModalAction = null;
  let pendingTargetSig = null;

  /* ======================== DOM HELPERS ======================== */
  function getEl(...ids) {
    for (let i = 0; i < ids.length; i++) {
      if (!ids[i]) continue;
      const el = document.getElementById(ids[i]);
      if (el) return el;
    }
    return null;
  }

  function setText(elOrIds, text) {
    const ids = Array.isArray(elOrIds) ? elOrIds : [elOrIds];
    ids.forEach((idOrEl) => {
      const el = typeof idOrEl === 'string' ? document.getElementById(idOrEl) : idOrEl;
      if (el) el.textContent = text;
    });
  }

  function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text == null ? '' : String(text);
    return div.innerHTML;
  }

  function formatTimestamp(tsMs) {
    if (!tsMs) {
      const now = new Date();
      return now.toTimeString().substring(0, 5);
    }
    const d = new Date(Number(tsMs));
    if (isNaN(d.getTime())) {
      const now = new Date();
      return now.toTimeString().substring(0, 5);
    }
    return d.toTimeString().substring(0, 5);
  }

  function dmChannelForPk(pk) {
    if (!pk) return '';
    return 'dm:' + pk.toLowerCase().substring(0, 58);
  }

  function isValidHex64(str) {
    return typeof str === 'string' && /^[0-9a-fA-F]{64}$/.test(str.trim());
  }

  /* ======================== PERSISTENT CONTACT BOOK (wex_contacts_v2) ======================== */
  function loadContacts() {
    try {
      const raw = localStorage.getItem(CONTACTS_STORAGE_KEY);
      if (raw) {
        const parsed = JSON.parse(raw);
        if (parsed && typeof parsed === 'object') {
          contacts = parsed;
          Object.keys(contacts).forEach((pk) => {
            contacts[pk].online = false;
          });
        }
      }
    } catch (e) {
      contacts = {};
    }
  }

  function saveContacts() {
    try {
      localStorage.setItem(CONTACTS_STORAGE_KEY, JSON.stringify(contacts));
    } catch (e) {}
  }

  function findContactKeyByPrefix(prefix58) {
    if (!prefix58) return null;
    const clean = prefix58.toLowerCase();
    const keys = Object.keys(contacts);
    for (let i = 0; i < keys.length; i++) {
      if (keys[i].toLowerCase().startsWith(clean)) {
        return keys[i];
      }
    }
    return null;
  }

  function upsertContact(pk, patch) {
    if (!pk || typeof pk !== 'string') return null;
    const normPk = pk.trim().toLowerCase();
    if (normPk.length < 58) return null;
    if (myPublicKey && normPk === myPublicKey.toLowerCase()) return null;

    let key = normPk;
    if (normPk.length < 64) {
      const existingFull = findContactKeyByPrefix(normPk);
      if (existingFull) key = existingFull;
    } else {
      const prefix58 = normPk.substring(0, 58);
      if (contacts[prefix58] && prefix58 !== normPk) {
        contacts[normPk] = Object.assign({}, contacts[prefix58], { public_key: normPk });
        delete contacts[prefix58];
      }
    }

    const defaultNick = 'Друг ' + key.substring(0, 4).toUpperCase();
    const existing = contacts[key] || {
      public_key: key,
      nickname: defaultNick,
      ip: '',
      port: 0,
      online: false,
      use_relay: false,
      last_msg: '',
      last_ts: 0,
      unread: 0
    };

    const updated = Object.assign({}, existing, patch || {}, { public_key: key });
    if (
      !updated.nickname ||
      updated.nickname === 'Anon' ||
      updated.nickname.startsWith('Peer-') ||
      updated.nickname.startsWith('Друг ')
    ) {
      if (patch && patch.nickname && patch.nickname !== 'Anon') {
        updated.nickname = patch.nickname;
      } else if (!existing.nickname || existing.nickname.startsWith('Peer-')) {
        updated.nickname = defaultNick;
      }
    }
    contacts[key] = updated;
    return updated;
  }

  function mergePeersAndHistoryIntoContacts(peersList, historyList) {
    Object.keys(contacts).forEach((k) => {
      contacts[k].online = false;
    });

    if (Array.isArray(peersList)) {
      peersList.forEach((p) => {
        if (!p || !p.public_key) return;
        upsertContact(p.public_key, {
          nickname: p.nickname || ('Друг ' + p.public_key.substring(0, 4).toUpperCase()),
          ip: p.ip || '',
          port: p.port || 0,
          online: true,
          use_relay: Boolean(p.use_relay)
        });
      });
    }

    if (Array.isArray(historyList)) {
      historyList.forEach((m) => {
        if (!m) return;
        const isDm = typeof m.channel === 'string' && m.channel.startsWith('dm:');
        const senderPk = m.author_pk && isValidHex64(m.author_pk) ? m.author_pk.toLowerCase() : null;
        const peerPk = m.peer_pk && isValidHex64(m.peer_pk) ? m.peer_pk.toLowerCase() : null;

        if (isDm) {
          const dmPrefix = m.channel.substring(3).toLowerCase();
          let targetPk = null;

          if (senderPk && (!myPublicKey || senderPk !== myPublicKey.toLowerCase())) {
            targetPk = senderPk;
          } else if (peerPk && (!myPublicKey || peerPk !== myPublicKey.toLowerCase())) {
            targetPk = peerPk;
          } else {
            targetPk = findContactKeyByPrefix(dmPrefix) || dmPrefix;
          }

          if (targetPk && (!myPublicKey || !myPublicKey.toLowerCase().startsWith(targetPk))) {
            const isFromOther = m.author && m.author !== myNick;
            const patch = {
              last_msg: m.content || '',
              last_ts: m.timestamp_ms || Date.now()
            };
            if (isFromOther && m.author) {
              patch.nickname = m.author;
            }
            upsertContact(targetPk, patch);
          }
        } else if (senderPk && (!myPublicKey || senderPk !== myPublicKey.toLowerCase())) {
          upsertContact(senderPk, {
            nickname: m.author || ('Друг ' + senderPk.substring(0, 4).toUpperCase())
          });
        }
      });
    }

    saveContacts();
  }

  /* ======================== PARSE URI / PUBLIC KEY / TARGET ======================== */
  function parseTargetInput(rawInput) {
    const trimmed = (rawInput || '').trim();
    if (!trimmed) return { raw: '', pk: null, ipPort: null };

    if (isValidHex64(trimmed)) {
      return { raw: trimmed, pk: trimmed.toLowerCase(), ipPort: null };
    }

    const atIdx = trimmed.indexOf('@');
    if (atIdx === 64 && isValidHex64(trimmed.substring(0, 64))) {
      return {
        raw: trimmed,
        pk: trimmed.substring(0, 64).toLowerCase(),
        ipPort: trimmed.substring(65)
      };
    }

    const pkMatch = trimmed.match(/[?&]pk=([0-9a-fA-F]{64})/);
    const peerMatch = trimmed.match(/[?&]peer=([^&#\s]+)/);
    if (pkMatch) {
      return {
        raw: trimmed,
        pk: pkMatch[1].toLowerCase(),
        ipPort: peerMatch ? peerMatch[1] : null
      };
    }

    // Also detect any embedded 64-char hex key inside a scanned string
    const hex64Match = trimmed.match(/\b([0-9a-fA-F]{64})\b/);
    if (hex64Match && trimmed.startsWith('wex://')) {
      return {
        raw: trimmed,
        pk: hex64Match[1].toLowerCase(),
        ipPort: peerMatch ? peerMatch[1] : null
      };
    }

    return { raw: trimmed, pk: null, ipPort: trimmed };
  }

  /* ======================== MASTER-DETAIL & TAB NAVIGATION ======================== */
  function normalizeTabName(tabName) {
    if (tabName === 'network' || tabName === 'net') return 'net';
    if (tabName === 'identity' || tabName === 'key') return 'key';
    if (tabName === 'swarms' || tabName === 'spaces') return 'swarms';
    return 'dms';
  }

  function selectTab(tabName) {
    activeTab = normalizeTabName(tabName);
    document.body.dataset.tab = activeTab;

    const btnMap = {
      dms: ['tab-btn-dms'],
      swarms: ['tab-btn-swarms'],
      net: ['tab-btn-net', 'tab-btn-network'],
      key: ['tab-btn-key', 'tab-btn-identity']
    };

    document.querySelectorAll('.mode-tab-btn, [data-tab], .tab-btn').forEach((btn) => {
      const attr = normalizeTabName(btn.getAttribute('data-tab') || '');
      const isMatch = attr === activeTab || (btnMap[activeTab] && btnMap[activeTab].includes(btn.id));
      btn.classList.toggle('active', isMatch);
    });

    const panelGroups = {
      dms: ['panel-dms', 'tab-dms', 'view-dms'],
      swarms: ['panel-swarms', 'tab-swarms', 'view-swarms'],
      net: ['panel-net', 'panel-network', 'tab-network', 'view-network'],
      key: ['panel-key', 'panel-identity', 'tab-identity', 'view-identity']
    };

    Object.keys(panelGroups).forEach((key) => {
      panelGroups[key].forEach((id) => {
        const el = document.getElementById(id);
        if (el) {
          el.classList.toggle('active', key === activeTab);
        }
      });
    });

    if (activeTab === 'key') {
      if (myPublicKey) renderAllKeyCanvases(myPublicKey);
      if (!myExportedSeed && ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ action: 'export_secret' }));
      }
    }
  }

  function switchModeTab(tabName) {
    selectTab(tabName);
  }

  function openDmConversation(pk) {
    if (!pk) return;
    const normPk = pk.trim().toLowerCase();
    const resolvedKey = normPk.length < 64 ? (findContactKeyByPrefix(normPk) || normPk) : normPk;
    const contact = upsertContact(resolvedKey, { unread: 0 });
    saveContacts();

    activeChatMode = 'dm';
    currentDmPeerPk = contact ? contact.public_key : resolvedKey;
    currentChannel = dmChannelForPk(currentDmPeerPk);

    document.body.classList.add('chat-active');
    document.body.classList.remove('drawer-open');

    if (ws && ws.readyState === WebSocket.OPEN && isValidHex64(currentDmPeerPk)) {
      ws.send(JSON.stringify({
        action: 'sync_dm',
        target_pk: currentDmPeerPk
      }));
    }

    updateChatHeader();
    renderContactList();
    renderChannelsList();
    renderHistory(historyCache);

    const inputEl = getEl('msg-input', 'chat-input');
    if (inputEl && window.innerWidth >= 768) {
      inputEl.focus();
    }
  }

  function openSwarmChannel(spaceId, channelName) {
    if (spaceId && spaceId !== currentSpaceId) {
      currentSpaceId = spaceId;
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ action: 'switch_space', space_id: spaceId }));
      }
    }
    const ch = channelName || 'general';
    activeChatMode = 'swarm';
    currentDmPeerPk = null;
    currentChannel = ch;

    document.body.classList.add('chat-active');
    document.body.classList.remove('drawer-open');

    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ action: 'switch_channel', name: ch }));
    }

    updateChatHeader();
    renderContactList();
    renderSpacesList();
    renderChannelsList();
    renderHistory(historyCache);
  }

  function closeActiveChat() {
    document.body.classList.remove('chat-active');
    document.body.classList.remove('drawer-open');
  }

  function closeMobileConversation() {
    closeActiveChat();
  }

  function updateChatHeader() {
    const prefixEl = getEl('channel-prefix', 'chat-header-prefix');
    const titleEl = getEl('active-channel-name', 'chat-header-title');
    const topicEl = getEl('active-channel-topic', 'chat-header-subtitle');
    const inputEl = getEl('msg-input', 'chat-input');

    if (activeChatMode === 'dm' && currentDmPeerPk) {
      const c = contacts[currentDmPeerPk] || {
        nickname: 'Друг ' + currentDmPeerPk.substring(0, 4).toUpperCase(),
        public_key: currentDmPeerPk,
        online: false
      };

      if (prefixEl) prefixEl.textContent = '@';
      if (titleEl) titleEl.textContent = c.nickname || 'Собеседник';
      if (topicEl) topicEl.textContent = c && c.online ? 'В сети' : 'Личный чат';
      if (inputEl) inputEl.placeholder = 'Написать сообщение...';
    } else {
      const sp = spacesCache.find((s) => s.id === currentSpaceId);
      const spName = sp ? sp.name : 'Уютное сообщество';
      if (prefixEl) prefixEl.textContent = '#';
      if (titleEl) titleEl.textContent = currentChannel;
      if (topicEl) topicEl.textContent = spName;
      if (inputEl) inputEl.placeholder = 'Написать сообщение...';
    }
  }

  /* =========================================================================
     ISO/IEC 18004 QR CODE GENERATOR (VERSION 4-L, 33x33 MATRIX, REED-SOLOMON)
     + PURE-JS OPTICAL MATRIX DECODER FOR IN-APP CAMERA & IMAGE SCANNING
     ========================================================================= */
  const QR_V4_SIZE = 33;
  const QR_V4_DATA_CODEWORDS = 80;
  const QR_V4_EC_CODEWORDS = 20;

  const GF_EXP = new Uint8Array(512);
  const GF_LOG = new Uint8Array(256);
  (function initGaloisField() {
    let x = 1;
    for (let i = 0; i < 255; i++) {
      GF_EXP[i] = x;
      GF_LOG[x] = i;
      x <<= 1;
      if (x & 0x100) x ^= 0x11d;
    }
    for (let i = 255; i < 512; i++) {
      GF_EXP[i] = GF_EXP[i - 255];
    }
  })();

  function gfMul(a, b) {
    if (a === 0 || b === 0) return 0;
    return GF_EXP[GF_LOG[a] + GF_LOG[b]];
  }

  function computeReedSolomonEc(dataBytes, ecLen) {
    let gen = [1];
    for (let i = 0; i < ecLen; i++) {
      const next = new Array(gen.length + 1).fill(0);
      const root = GF_EXP[i];
      for (let j = 0; j < gen.length; j++) {
        next[j] ^= gen[j];
        next[j + 1] ^= gfMul(gen[j], root);
      }
      gen = next;
    }

    const msg = new Uint8Array(dataBytes.length + ecLen);
    msg.set(dataBytes, 0);

    for (let i = 0; i < dataBytes.length; i++) {
      const coef = msg[i];
      if (coef !== 0) {
        for (let j = 0; j < gen.length; j++) {
          msg[i + j] ^= gfMul(gen[j], coef);
        }
      }
    }
    return msg.slice(dataBytes.length);
  }

  function createQrV4FunctionTemplate() {
    const N = QR_V4_SIZE;
    const isFunc = Array.from({ length: N }, () => new Uint8Array(N));
    const mod = Array.from({ length: N }, () => new Uint8Array(N));

    function placeFinder(ox, oy) {
      for (let r = -1; r <= 7; r++) {
        for (let c = -1; c <= 7; c++) {
          const rr = oy + r;
          const cc = ox + c;
          if (rr < 0 || rr >= N || cc < 0 || cc >= N) continue;
          isFunc[rr][cc] = 1;
          if (r >= 0 && r <= 6 && c >= 0 && c <= 6) {
            const border = r === 0 || r === 6 || c === 0 || c === 6;
            const inner = r >= 2 && r <= 4 && c >= 2 && c <= 4;
            mod[rr][cc] = (border || inner) ? 1 : 0;
          } else {
            mod[rr][cc] = 0;
          }
        }
      }
    }

    placeFinder(0, 0);
    placeFinder(N - 7, 0);
    placeFinder(0, N - 7);

    // Alignment pattern at (26, 26) for Version 4
    const ax = 26;
    const ay = 26;
    for (let r = -2; r <= 2; r++) {
      for (let c = -2; c <= 2; c++) {
        const rr = ay + r;
        const cc = ax + c;
        isFunc[rr][cc] = 1;
        const dist = Math.max(Math.abs(r), Math.abs(c));
        mod[rr][cc] = (dist === 2 || dist === 0) ? 1 : 0;
      }
    }

    // Timing patterns on row 6 and col 6
    for (let i = 8; i < N - 8; i++) {
      isFunc[6][i] = 1;
      mod[6][i] = (i % 2 === 0) ? 1 : 0;
      isFunc[i][6] = 1;
      mod[i][6] = (i % 2 === 0) ? 1 : 0;
    }

    // Dark module and reserved format information areas
    isFunc[N - 8][8] = 1;
    mod[N - 8][8] = 1;

    for (let i = 0; i <= 8; i++) {
      if (i !== 6) {
        isFunc[8][i] = 1;
        isFunc[i][8] = 1;
      }
    }
    for (let i = 0; i < 8; i++) {
      isFunc[8][N - 1 - i] = 1;
    }
    for (let i = 0; i < 7; i++) {
      isFunc[N - 1 - i][8] = 1;
    }

    // Write Format Info for Level L (01) and Mask 0 (000) -> BCH(15,5)
    let fmtData = (0x01 << 3) | 0x00; // 01000
    let rem = fmtData << 10;
    for (let i = 14; i >= 10; i--) {
      if ((rem >>> i) & 1) rem ^= (0x537 << (i - 10));
    }
    const fmtBits = ((fmtData << 10) | rem) ^ 0x5412;

    const fmtCoordsA = [
      [8, 0], [8, 1], [8, 2], [8, 3], [8, 4], [8, 5], [8, 7], [8, 8],
      [7, 8], [5, 8], [4, 8], [3, 8], [2, 8], [1, 8], [0, 8]
    ];
    const fmtCoordsB = [
      [N - 1, 8], [N - 2, 8], [N - 3, 8], [N - 4, 8], [N - 5, 8], [N - 6, 8], [N - 7, 8],
      [8, N - 8], [8, N - 7], [8, N - 6], [8, N - 5], [8, N - 4], [8, N - 3], [8, N - 2], [8, N - 1]
    ];

    for (let i = 0; i < 15; i++) {
      const bit = (fmtBits >>> i) & 1;
      mod[fmtCoordsA[i][0]][fmtCoordsA[i][1]] = bit;
      mod[fmtCoordsB[i][0]][fmtCoordsB[i][1]] = bit;
    }

    return { N, isFunc, mod };
  }

  function normalizeQrPayload(rawKeyOrUri) {
    const clean = (rawKeyOrUri || '').trim();
    if (isValidHex64(clean)) {
      return 'wex://?pk=' + clean.toLowerCase();
    }
    const parsed = parseTargetInput(clean);
    if (parsed.pk) {
      return 'wex://?pk=' + parsed.pk.toLowerCase();
    }
    return clean.substring(0, 74) || 'wex://?pk=' + '0'.repeat(64);
  }

  function buildIsoQrV4Matrix(payloadStr) {
    const text = normalizeQrPayload(payloadStr);
    const utf8 = new TextEncoder().encode(text);
    const len = Math.min(utf8.length, 78);

    const bits = [];
    function pushBits(val, count) {
      for (let i = count - 1; i >= 0; i--) {
        bits.push((val >>> i) & 1);
      }
    }

    // Byte mode indicator (0100) + 8-bit byte count
    pushBits(0x04, 4);
    pushBits(len, 8);
    for (let i = 0; i < len; i++) {
      pushBits(utf8[i], 8);
    }

    // Terminator up to 4 zero bits
    const maxDataBits = QR_V4_DATA_CODEWORDS * 8;
    for (let i = 0; i < 4 && bits.length < maxDataBits; i++) {
      bits.push(0);
    }
    while (bits.length % 8 !== 0) {
      bits.push(0);
    }

    const dataBytes = new Uint8Array(QR_V4_DATA_CODEWORDS);
    const filledBytes = bits.length >>> 3;
    for (let i = 0; i < filledBytes; i++) {
      let b = 0;
      for (let j = 0; j < 8; j++) {
        b = (b << 1) | bits[i * 8 + j];
      }
      dataBytes[i] = b;
    }
    const padBytes = [0xec, 0x11];
    for (let i = filledBytes; i < QR_V4_DATA_CODEWORDS; i++) {
      dataBytes[i] = padBytes[(i - filledBytes) & 1];
    }

    const ecBytes = computeReedSolomonEc(dataBytes, QR_V4_EC_CODEWORDS);
    const allBytes = new Uint8Array(QR_V4_DATA_CODEWORDS + QR_V4_EC_CODEWORDS);
    allBytes.set(dataBytes, 0);
    allBytes.set(ecBytes, QR_V4_DATA_CODEWORDS);

    const { N, isFunc, mod } = createQrV4FunctionTemplate();

    let bitIdx = 0;
    const totalBits = allBytes.length * 8;
    let upward = true;

    for (let rightCol = N - 1; rightCol >= 1; rightCol -= 2) {
      if (rightCol === 6) rightCol = 5;
      for (let vert = 0; vert < N; vert++) {
        const r = upward ? (N - 1 - vert) : vert;
        for (let lr = 0; lr < 2; lr++) {
          const c = rightCol - lr;
          if (isFunc[r][c]) continue;
          let bit = 0;
          if (bitIdx < totalBits) {
            bit = (allBytes[bitIdx >>> 3] >>> (7 - (bitIdx & 7))) & 1;
            bitIdx++;
          }
          // Mask 0: (r + c) % 2 === 0
          if ((r + c) % 2 === 0) bit ^= 1;
          mod[r][c] = bit;
        }
      }
      upward = !upward;
    }

    return mod;
  }

  function decodeIsoQrV4Matrix(modGrid) {
    if (!modGrid || modGrid.length !== QR_V4_SIZE) return null;
    const { N, isFunc } = createQrV4FunctionTemplate();

    const bits = [];
    let upward = true;
    for (let rightCol = N - 1; rightCol >= 1; rightCol -= 2) {
      if (rightCol === 6) rightCol = 5;
      for (let vert = 0; vert < N; vert++) {
        const r = upward ? (N - 1 - vert) : vert;
        for (let lr = 0; lr < 2; lr++) {
          const c = rightCol - lr;
          if (isFunc[r][c]) continue;
          let bit = modGrid[r][c] ? 1 : 0;
          if ((r + c) % 2 === 0) bit ^= 1;
          bits.push(bit);
        }
      }
      upward = !upward;
    }

    function readBits(offset, count) {
      let val = 0;
      for (let i = 0; i < count; i++) {
        val = (val << 1) | (bits[offset + i] || 0);
      }
      return val;
    }

    const mode = readBits(0, 4);
    if (mode !== 0x04) return null;
    const byteLen = readBits(4, 8);
    if (byteLen <= 0 || byteLen > 78) return null;

    const out = new Uint8Array(byteLen);
    for (let i = 0; i < byteLen; i++) {
      out[i] = readBits(12 + i * 8, 8);
    }

    try {
      const str = new TextDecoder().decode(out);
      const parsed = parseTargetInput(str);
      if (parsed.pk || str.startsWith('wex://')) {
        return str;
      }
    } catch (e) {}
    return null;
  }

  function decodeWexQrFromImageData(imageData) {
    if (!imageData || !imageData.data || !imageData.width || !imageData.height) return null;
    const { data, width, height } = imageData;

    // 1. Compute luminance threshold and locate bounding box of dark QR modules
    let minLum = 255;
    let maxLum = 0;
    for (let i = 0; i < data.length; i += 16) {
      const lum = (data[i] * 77 + data[i + 1] * 150 + data[i + 2] * 29) >> 8;
      if (lum < minLum) minLum = lum;
      if (lum > maxLum) maxLum = lum;
    }
    const threshold = (minLum + maxLum) >> 1;

    function isDarkPixel(x, y) {
      const idx = (y * width + x) * 4;
      if (data[idx + 3] < 128) return false;
      const lum = (data[idx] * 77 + data[idx + 1] * 150 + data[idx + 2] * 29) >> 8;
      return lum < threshold;
    }

    // Restrict scan region to central 92% of image to ignore outer frame borders
    const marginX = Math.floor(width * 0.04);
    const marginY = Math.floor(height * 0.04);

    let minX = width;
    let maxX = 0;
    let minY = height;
    let maxY = 0;

    for (let y = marginY; y < height - marginY; y++) {
      for (let x = marginX; x < width - marginX; x++) {
        if (isDarkPixel(x, y)) {
          if (x < minX) minX = x;
          if (x > maxX) maxX = x;
          if (y < minY) minY = y;
          if (y > maxY) maxY = y;
        }
      }
    }

    const boxW = maxX - minX + 1;
    const boxH = maxY - minY + 1;
    if (boxW < QR_V4_SIZE || boxH < QR_V4_SIZE) return null;

    const N = QR_V4_SIZE;
    const modGrid = Array.from({ length: N }, () => new Uint8Array(N));

    for (let r = 0; r < N; r++) {
      const sy = Math.min(height - 1, Math.floor(minY + ((r + 0.5) * boxH) / N));
      for (let c = 0; c < N; c++) {
        const sx = Math.min(width - 1, Math.floor(minX + ((c + 0.5) * boxW) / N));
        modGrid[r][c] = isDarkPixel(sx, sy) ? 1 : 0;
      }
    }

    return decodeIsoQrV4Matrix(modGrid);
  }

  function renderKeyMatrixCanvas(canvasOrId, hexKey) {
    const canvas = typeof canvasOrId === 'string' ? document.getElementById(canvasOrId) : canvasOrId;
    if (!canvas || !canvas.getContext) return;

    const matrix = buildIsoQrV4Matrix(hexKey || myPublicKey || '0'.repeat(64));
    const N = matrix.length;
    const quietZone = 4;
    const totalModules = N + quietZone * 2;

    const targetSize = canvas.width || 205;
    const cellSize = Math.max(3, Math.floor(targetSize / totalModules));
    const actualSize = cellSize * totalModules;
    const offset = Math.floor((targetSize - actualSize) / 2);

    const ctx = canvas.getContext('2d');
    const bgColor = '#f5f3ff';
    const fgPrimary = '#181622';

    ctx.fillStyle = bgColor;
    ctx.fillRect(0, 0, canvas.width, canvas.height);

    ctx.fillStyle = fgPrimary;
    for (let r = 0; r < N; r++) {
      for (let c = 0; c < N; c++) {
        if (matrix[r][c]) {
          ctx.fillRect(
            offset + (c + quietZone) * cellSize,
            offset + (r + quietZone) * cellSize,
            cellSize,
            cellSize
          );
        }
      }
    }
  }

  function renderAllKeyCanvases(hexKey) {
    const keyToRender = hexKey || myPublicKey;
    if (!keyToRender) return;
    const ids = ['profile-qr-canvas', 'invite-qr-canvas', 'identity-qr-canvas', 'qr-canvas', 'key-matrix-canvas'];
    ids.forEach((id) => {
      const c = document.getElementById(id);
      if (c) renderKeyMatrixCanvas(c, keyToRender);
    });
  }

  /* ======================== IN-APP CAMERA & IMAGE QR SCANNER ======================== */
  function ensureQrScannerElements() {
    // Ensure #invite-qr-canvas exists inside #modal-invite if not already present
    const inviteModal = document.getElementById('modal-invite');
    if (inviteModal && !document.getElementById('invite-qr-canvas')) {
      const card = inviteModal.querySelector('.modal-card');
      const header = inviteModal.querySelector('.modal-header');
      if (card && header) {
        const qrWrap = document.createElement('div');
        qrWrap.className = 'qr-container';
        qrWrap.innerHTML = `
          <canvas id="invite-qr-canvas" width="205" height="205"></canvas>
          <span class="qr-caption">Покажите этот QR-код другу или отсканируйте его камерой</span>
        `;
        header.insertAdjacentElement('afterend', qrWrap);
      }
    }

    // Ensure #modal-qr-scanner exists in DOM if not already present
    if (!document.getElementById('modal-qr-scanner')) {
      const modal = document.createElement('div');
      modal.className = 'modal-overlay';
      modal.id = 'modal-qr-scanner';
      modal.onclick = (e) => {
        if (e.target === modal) closeQrScannerModal();
      };
      modal.innerHTML = `
        <div class="modal-card">
          <div class="modal-header">
            <span class="modal-title">Сканировать QR-код друга</span>
            <button class="modal-close" onclick="closeQrScannerModal()">&times;</button>
          </div>
          <div class="qr-scanner-viewport" style="position:relative; width:100%; aspect-ratio:1/1; max-height:280px; background:#12111a; border-radius:16px; overflow:hidden; display:flex; align-items:center; justify-content:center;">
            <video id="qr-video" playsinline muted autoplay style="width:100%; height:100%; object-fit:cover;"></video>
            <canvas id="qr-scan-canvas" width="320" height="320" style="display:none;"></canvas>
          </div>
          <div id="qr-scanner-status" style="font-size:13px; color:#a39ec4; text-align:center;">
            Наведите камеру на QR-код друга или выберите фото из галереи
          </div>
          <input type="file" id="qr-image-input" accept="image/*" style="display:none" onchange="handleQrImageFile(event)">
          <div class="modal-actions">
            <button class="btn" onclick="switchScannerCamera()">Сменить камеру</button>
            <button class="btn primary" onclick="document.getElementById('qr-image-input').click()">Выбрать фото QR</button>
          </div>
        </div>
      `;
      document.body.appendChild(modal);
    }
  }

  function stopQrScannerCamera() {
    if (qrScannerTimer) {
      clearInterval(qrScannerTimer);
      qrScannerTimer = null;
    }
    if (qrScannerStream) {
      qrScannerStream.getTracks().forEach((t) => {
        try { t.stop(); } catch (e) {}
      });
      qrScannerStream = null;
    }
    const video = document.getElementById('qr-video');
    if (video) {
      video.srcObject = null;
    }
  }

  function onQrPayloadDecoded(decodedText) {
    if (!decodedText) return false;
    const parsed = parseTargetInput(decodedText);
    if (!parsed.pk && !decodedText.startsWith('wex://')) {
      return false;
    }

    closeQrScannerModal();
    playIncomingDmTick();
    showToast('QR-код успешно отсканирован');
    addContactOrConnect(decodedText);
    return true;
  }

  async function scanCurrentVideoFrame() {
    const video = document.getElementById('qr-video');
    const scanCanvas = document.getElementById('qr-scan-canvas');
    if (!video || video.readyState < 2 || ! video.videoWidth) return;

    // 1. Primary native detector (BarcodeDetector in Android WebView / Chrome)
    if (barcodeDetectorInstance) {
      try {
        const barcodes = await barcodeDetectorInstance.detect(video);
        if (barcodes && barcodes.length > 0 && barcodes[0].rawValue) {
          if (onQrPayloadDecoded(barcodes[0].rawValue)) return;
        }
      } catch (e) {}
    }

    // 2. Built-in pure-JS optical matrix decoder fallback
    if (scanCanvas && scanCanvas.getContext) {
      const ctx = scanCanvas.getContext('2d', { willReadFrequently: true });
      const w = scanCanvas.width || 320;
      const h = scanCanvas.height || 320;
      ctx.drawImage(video, 0, 0, w, h);
      try {
        const imgData = ctx.getImageData(0, 0, w, h);
        const decoded = decodeWexQrFromImageData(imgData);
        if (decoded) {
          onQrPayloadDecoded(decoded);
        }
      } catch (e) {}
    }
  }

  async function startQrScannerCamera() {
    stopQrScannerCamera();
    const statusEl = document.getElementById('qr-scanner-status');
    const video = document.getElementById('qr-video');

    if ('BarcodeDetector' in window && !barcodeDetectorInstance) {
      try {
        barcodeDetectorInstance = new window.BarcodeDetector({ formats: ['qr_code'] });
      } catch (e) {
        barcodeDetectorInstance = null;
      }
    }

    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
      if (statusEl) {
        statusEl.textContent = 'Камера недоступна в этом режиме — нажмите «Выбрать фото QR» ниже';
      }
      return;
    }

    try {
      qrScannerStream = await navigator.mediaDevices.getUserMedia({
        video: {
          facingMode: scannerFacingMode,
          width: { ideal: 640 },
          height: { ideal: 640 }
        },
        audio: false
      });

      if (video) {
        video.srcObject = qrScannerStream;
        await video.play().catch(() => {});
      }
      if (statusEl) {
        statusEl.textContent = 'Наведите камеру на QR-код друга';
      }

      qrScannerTimer = setInterval(scanCurrentVideoFrame, 150);
    } catch (err) {
      if (statusEl) {
        statusEl.textContent = 'Нет доступа к камере — выберите скриншот или фото QR-кода';
      }
    }
  }

  function openQrScannerModal() {
    ensureQrScannerElements();
    closeModals();
    const modal = document.getElementById('modal-qr-scanner');
    if (modal) {
      modal.classList.add('open');
    }
    startQrScannerCamera();
  }

  function closeQrScannerModal() {
    stopQrScannerCamera();
    const modal = document.getElementById('modal-qr-scanner');
    if (modal) {
      modal.classList.remove('open');
    }
  }

  function switchScannerCamera() {
    scannerFacingMode = (scannerFacingMode === 'environment') ? 'user' : 'environment';
    startQrScannerCamera();
  }

  function handleQrImageFile(event) {
    const file = event && event.target && event.target.files ? event.target.files[0] : null;
    if (!file) return;

    const statusEl = document.getElementById('qr-scanner-status');
    const reader = new FileReader();
    reader.onload = () => {
      const img = new Image();
      img.onload = async () => {
        ensureQrScannerElements();
        const scanCanvas = document.getElementById('qr-scan-canvas');
        if (!scanCanvas) return;

        scanCanvas.width = img.width || 320;
        scanCanvas.height = img.height || 320;
        const ctx = scanCanvas.getContext('2d', { willReadFrequently: true });
        ctx.drawImage(img, 0, 0, scanCanvas.width, scanCanvas.height);

        // 1. Try native BarcodeDetector first
        if ('BarcodeDetector' in window) {
          try {
            if (!barcodeDetectorInstance) {
              barcodeDetectorInstance = new window.BarcodeDetector({ formats: ['qr_code'] });
            }
            const codes = await barcodeDetectorInstance.detect(scanCanvas);
            if (codes && codes.length > 0 && codes[0].rawValue) {
              if (onQrPayloadDecoded(codes[0].rawValue)) return;
            }
          } catch (e) {}
        }

        // 2. Built-in pure-JS optical matrix decoder
        try {
          const imgData = ctx.getImageData(0, 0, scanCanvas.width, scanCanvas.height);
          const decoded = decodeWexQrFromImageData(imgData);
          if (decoded && onQrPayloadDecoded(decoded)) {
            return;
          }
        } catch (e) {}

        if (statusEl) {
          statusEl.textContent = 'Не удалось распознать QR-код на изображении. Попробуйте другое фото.';
        }
        showToast('QR-код на фото не найден');
      };
      img.src = String(reader.result);
    };
    reader.readAsDataURL(file);
    event.target.value = '';
  }

  /* ======================== SOFT AUDIO NOTIFICATION ======================== */
  function playIncomingDmTick() {
    try {
      const AudioContextClass = window.AudioContext || window.webkitAudioContext;
      if (!AudioContextClass) return;
      if (!audioCtx) {
        audioCtx = new AudioContextClass();
      }
      if (audioCtx.state === 'suspended') {
        audioCtx.resume().catch(() => {});
      }
      const now = audioCtx.currentTime;
      const osc = audioCtx.createOscillator();
      const gain = audioCtx.createGain();

      osc.type = 'sine';
      osc.frequency.setValueAtTime(587.33, now);
      osc.frequency.exponentialRampToValueAtTime(880.0, now + 0.065);

      gain.gain.setValueAtTime(0.001, now);
      gain.gain.exponentialRampToValueAtTime(0.06, now + 0.012);
      gain.gain.exponentialRampToValueAtTime(0.0001, now + 0.11);

      osc.connect(gain);
      gain.connect(audioCtx.destination);

      osc.start(now);
      osc.stop(now + 0.12);
    } catch (e) {}
  }

  /* ======================== NATIVE ANDROID BRIDGE & CLIPBOARD ======================== */
  function showToast(msg) {
    const t = getEl('toast', 'app-toast');
    if (!t) return;
    t.textContent = msg;
    t.classList.add('show');
    clearTimeout(t._hideTimer);
    t._hideTimer = setTimeout(() => t.classList.remove('show'), 2200);
  }

  function copyTextWithToast(text, toastMsg) {
    if (!text) return;
    if (window.AndroidBridge && typeof window.AndroidBridge.copyText === 'function') {
      window.AndroidBridge.copyText(String(text));
      showToast(toastMsg || 'Скопировано!');
      return;
    }
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(String(text)).then(() => {
        showToast(toastMsg || 'Скопировано!');
      }).catch(() => fallbackCopy(String(text), toastMsg));
    } else {
      fallbackCopy(String(text), toastMsg);
    }
  }

  function fallbackCopy(text, toastMsg) {
    const ta = document.createElement('textarea');
    ta.value = text;
    ta.style.position = 'fixed';
    ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.select();
    try {
      document.execCommand('copy');
      showToast(toastMsg || 'Скопировано!');
    } catch (e) {}
    document.body.removeChild(ta);
  }

  async function pasteIntoInput(inputId) {
    const el = typeof inputId === 'string' ? document.getElementById(inputId) : inputId;
    if (!el) return;
    if (window.AndroidBridge && typeof window.AndroidBridge.pasteText === 'function') {
      const clip = window.AndroidBridge.pasteText();
      if (clip) {
        el.value = String(clip).trim();
        showToast('Вставлено из буфера');
        return;
      }
    }
    if (navigator.clipboard && navigator.clipboard.readText) {
      try {
        const clip = await navigator.clipboard.readText();
        if (clip) {
          el.value = clip.trim();
          showToast('Вставлено из буфера');
          return;
        }
      } catch (e) {}
    }
    el.focus();
  }

  function shareInviteNative(customText) {
    const link = customText || myInviteUri || (myPublicKey ? `wex://join/${currentSpaceId || 'dm'}?peer=${myWanEndpoint || ('127.0.0.1:' + myUdpPort)}&pk=${myPublicKey}` : '');
    if (!link) return;
    if (window.AndroidBridge && typeof window.AndroidBridge.shareText === 'function') {
      window.AndroidBridge.shareText(link);
    } else if (navigator.share) {
      navigator.share({ title: 'Приглашение в Wex-Chat', text: link }).catch(() => {});
    } else {
      copyTextWithToast(link, 'Ссылка-приглашение скопирована!');
    }
  }

  /* ======================== RENDER FUNCTIONS ======================== */
  function renderContactList() {
    const container = getEl('dm-container', 'dm-list', 'contacts-list');
    if (!container) return;

    const list = Object.values(contacts);

    const totalUnread = list.reduce((acc, c) => acc + (c.unread || 0), 0);
    setText(['badge-dms'], String(totalUnread > 0 ? totalUnread : list.length));

    list.sort((a, b) => {
      if (a.online !== b.online) return a.online ? -1 : 1;
      if ((b.last_ts || 0) !== (a.last_ts || 0)) return (b.last_ts || 0) - (a.last_ts || 0);
      return (a.nickname || '').localeCompare(b.nickname || '');
    });

    container.innerHTML = '';

    if (list.length === 0) {
      container.innerHTML = `
        <div class="empty-list-hint">
          Здесь пока нет чатов. Нажмите «Написать другу» или «Сканировать QR» выше, чтобы начать общение!
        </div>
      `;
      return;
    }

    list.forEach((c) => {
      const dmCh = dmChannelForPk(c.public_key);
      const isActive = activeChatMode === 'dm' && (currentDmPeerPk === c.public_key || currentChannel === dmCh);
      const item = document.createElement('div');
      item.className = 'peer-item channel-item' + (isActive ? ' active' : '');
      item.setAttribute('data-pk', c.public_key);

      const dotClass = c.online ? 'online' : 'offline';
      const statusBadge = c.online ? 'В сети' : 'Не в сети';
      const preview = c.last_msg
        ? (c.last_msg.length > 36 ? c.last_msg.substring(0, 36) + '…' : c.last_msg)
        : (c.online ? 'На связи' : 'Нажмите, чтобы написать');

      item.innerHTML = `
        <div class="peer-left">
          <span class="peer-dot ${dotClass}"></span>
          <div class="peer-meta">
            <span class="peer-name">${escapeHtml(c.nickname)}</span>
            <span class="peer-sub">${escapeHtml(preview)}</span>
          </div>
        </div>
        <div class="item-right">
          ${c.unread > 0
            ? `<span class="crypto-tag unread">+${c.unread}</span>`
            : `<span class="crypto-tag">${statusBadge}</span>`
          }
        </div>
      `;

      item.onclick = () => openDmConversation(c.public_key);
      container.appendChild(item);
    });
  }

  function renderPeersList() {
    const container = getEl('peer-container', 'lan-peers-list');
    if (!container) return;
    container.innerHTML = '';

    if (!peersCache || peersCache.length === 0) {
      container.innerHTML = '<div class="empty-list-hint">Ищем друзей поблизости и в сети...</div>';
      return;
    }

    peersCache.forEach((p) => {
      const item = document.createElement('div');
      item.className = 'peer-item';
      item.innerHTML = `
        <div class="peer-left">
          <span class="peer-dot online"></span>
          <div class="peer-meta">
            <span class="peer-name">${escapeHtml(p.nickname || 'Друг')}</span>
            <span class="peer-sub">В сети • На связи</span>
          </div>
        </div>
        <span class="crypto-tag">Написать</span>
      `;
      item.onclick = () => {
        if (p.public_key) {
          openDmConversation(p.public_key);
        }
      };
      container.appendChild(item);
    });
  }

  function renderSpacesList() {
    const container = getEl('guilds-bar', 'spaces-list');
    setText(['badge-swarms'], String(spacesCache.length));
    if (!container) return;
    container.innerHTML = '';

    spacesCache.forEach((sp) => {
      const active = sp.id === currentSpaceId;
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'swarm-pill guild-icon' + (active ? ' active' : '');
      btn.textContent = sp.name || 'Сообщество';
      btn.title = sp.name || 'Сообщество';
      btn.onclick = () => switchSpace(sp.id);
      container.appendChild(btn);
    });

    const addBtn = document.createElement('button');
    addBtn.type = 'button';
    addBtn.className = 'swarm-pill add-btn guild-icon';
    addBtn.textContent = '+ Создать';
    addBtn.onclick = () => openInputModal('create_space');
    container.appendChild(addBtn);
  }

  function renderChannelsList() {
    const container = getEl('channel-container', 'swarm-channels-list');
    if (!container) return;
    container.innerHTML = '';

    const activeSp = spacesCache.find((s) => s.id === currentSpaceId) || spacesCache[0];
    setText(['server-title'], activeSp ? activeSp.name : 'Моё сообщество');
    setText(['active-swarm-id-label'], 'Общие комнаты и каналы');

    const chs = (activeSp && Array.isArray(activeSp.channels)) ? activeSp.channels : ['general'];
    chs.forEach((ch) => {
      const isAct = activeChatMode === 'swarm' && currentChannel === ch;
      const div = document.createElement('div');
      div.className = 'channel-item text-ch' + (isAct ? ' active' : '');
      div.textContent = ch;
      div.onclick = () => openSwarmChannel(currentSpaceId, ch);
      container.appendChild(div);
    });
  }

  function renderNetworkTelemetry() {
    const isOnline = Boolean(ws && ws.readyState === WebSocket.OPEN);
    setText(['net-daemon-state'], isOnline ? 'Подключено' : 'Подключение...');
    setText(['net-udp-port'], isOnline ? 'Активно' : 'Ожидание');
    setText(['net-wan-endpoint'], myWanEndpoint ? 'Прямое соединение' : 'Автоматический маршрут');
    setText(['net-dht-count'], String(myDhtCount));
    setText(['net-quas-count'], myOfflineShards > 0 ? `${myOfflineShards} в очереди` : 'Все доставлены');

    const pulseDot = getEl('node-pulse-indicator');
    if (pulseDot) {
      pulseDot.style.background = isOnline ? '#34d399' : '#fbbf24';
    }

    const diagLines = [
      `Состояние: ${isOnline ? 'В сети • На связи' : 'Подключение...'}`,
      `Друзей онлайн: ${peersCache.length}`,
      `Узлов в общей сети: ${myDhtCount}`,
      `Ожидают доставки: ${myOfflineShards}`
    ];

    setText(['profile-diag-box'], diagLines.join('\n'));
  }

  function renderIdentityPanel() {
    window.myPublicKey = myPublicKey;
    setText(['my-nickname'], myNick);
    setText(['my-avatar'], (myNick || 'Я').charAt(0).toUpperCase());

    const isConnected = Boolean(ws && ws.readyState === WebSocket.OPEN);
    const friendlyStatus = !isConnected
      ? 'Подключение...'
      : (peersCache.length > 0 ? `В сети • Друзей онлайн: ${peersCache.length}` : 'В сети • На связи');
    setText(['my-node-status'], friendlyStatus);

    const inviteUri = myInviteUri || (myPublicKey ? `wex://join/${currentSpaceId}?peer=127.0.0.1:${myUdpPort}&pk=${myPublicKey}` : 'Загрузка...');
    setText(['key-pubkey-display', 'invite-pk-box', 'identity-pk-box'], myPublicKey || 'Загрузка...');
    setText(['invite-link-box', 'identity-invite-box'], inviteUri);

    const nickInput = getEl('profile-nick-input');
    if (nickInput && document.activeElement !== nickInput) {
      nickInput.value = myNick;
    }

    if (myPublicKey) {
      renderAllKeyCanvases(myPublicKey);
    }
  }

  /* ======================== MESSAGE FEED RENDERING ======================== */
  function messageMatchesCurrentConversation(msg) {
    if (!msg) return false;
    const msgCh = msg.channel || 'general';

    if (activeChatMode === 'dm' && currentDmPeerPk) {
      const expectedDmCh = dmChannelForPk(currentDmPeerPk);
      if (msgCh.toLowerCase() === expectedDmCh.toLowerCase()) return true;

      const peerPrefix = currentDmPeerPk.toLowerCase().substring(0, 58);
      if (msgCh.toLowerCase() === 'dm:' + peerPrefix) return true;

      const aPk = (msg.author_pk || '').toLowerCase();
      const pPk = (msg.peer_pk || '').toLowerCase();
      if (msgCh.startsWith('dm:') && (aPk.startsWith(peerPrefix) || pPk.startsWith(peerPrefix))) {
        return true;
      }
      return false;
    }

    if (activeChatMode === 'swarm') {
      return msgCh === currentChannel;
    }
    return false;
  }

  function normalizeAndSortHistory(list) {
    if (!Array.isArray(list)) return [];
    const seen = new Set();
    const unique = [];
    for (let i = 0; i < list.length; i++) {
      const m = list[i];
      if (!m) continue;
      if (m.signature) {
        if (seen.has(m.signature)) continue;
        seen.add(m.signature);
      }
      unique.push(m);
    }
    unique.sort((a, b) => (Number(a.timestamp_ms) || 0) - (Number(b.timestamp_ms) || 0));
    return unique;
  }

  function renderHistory(historyList) {
    const feed = getEl('messages', 'chat-messages');
    if (!feed) return;
    feed.innerHTML = '';

    const sorted = normalizeAndSortHistory(historyList || []);
    const filtered = sorted.filter(messageMatchesCurrentConversation);
    if (filtered.length === 0) {
      if (activeChatMode === 'dm' && currentDmPeerPk) {
        const c = contacts[currentDmPeerPk];
        const name = c ? c.nickname : 'Собеседник';
        feed.innerHTML = `
          <div class="empty-state">
            <span class="empty-state-tag">Личный чат</span>
            <h3>Начните диалог с ${escapeHtml(name)}</h3>
            <p>Напишите первое сообщение ниже. Даже если собеседник сейчас не в сети, сообщение бережно сохранится и дойдет, как только он появится.</p>
          </div>`;
      } else {
        feed.innerHTML = `
          <div class="empty-state">
            <span class="empty-state-tag">Комната сообщества</span>
            <h3>Добро пожаловать в #${escapeHtml(currentChannel)}</h3>
            <p>Здесь пока тихо. Напишите первое сообщение или поделитесь ссылкой-приглашением с друзьями.</p>
          </div>`;
      }
      return;
    }

    filtered.forEach((m) => appendMessageToDom(m));
  }

  function appendMessageToDom(msg) {
    const feed = getEl('messages', 'chat-messages');
    if (!feed) return;

    const emptyEl = feed.querySelector('.empty-state');
    if (emptyEl) emptyEl.remove();

    const sig = msg.signature || ('local-' + (msg.timestamp_ms || Date.now()));
    if (sig && document.getElementById('msg-' + sig)) {
      return;
    }

    const group = document.createElement('div');
    group.className = 'msg-group';
    group.id = 'msg-' + sig;
    group.onclick = (e) => {
      if (e.target.closest('.action-btn') || e.target.closest('.attachment-card')) return;
      const wasSelected = group.classList.contains('selected');
      document.querySelectorAll('.msg-group.selected').forEach((el) => el.classList.remove('selected'));
      if (!wasSelected) group.classList.add('selected');
    };

    const isMine = (msg.author === myNick) || (myPublicKey && msg.author_pk && msg.author_pk.toLowerCase() === myPublicKey.toLowerCase());
    const isDmMsg = (msg.channel && msg.channel.startsWith('dm:')) || Boolean(msg.encrypted);
    if (isMine) group.classList.add('mine');

    const initial = (msg.author || 'Д').charAt(0).toUpperCase();

    let contentHtml = escapeHtml(msg.content || '');
    if (msg.edited) {
      contentHtml += ' <span style="font-size:11px; opacity:0.75;">(изм.)</span>';
    }

    const rawContent = msg.content || '';
    if (rawContent.startsWith('[FILE:') || rawContent.startsWith('[attachment:')) {
      const closeIdx = rawContent.indexOf(']');
      const prefixLen = rawContent.startsWith('[FILE:') ? 6 : 12;
      if (closeIdx > prefixLen) {
        const meta = rawContent.substring(prefixLen, closeIdx).split(':');
        const hash = meta[0];
        const fname = meta[1] || 'file';
        const fsize = Number(meta[2] || 0);
        const mime = meta[3] || 'application/octet-stream';
        const isImg = mime.startsWith('image/');

        if (ws && ws.readyState === WebSocket.OPEN && !blobCache[hash]) {
          ws.send(JSON.stringify({ action: 'get_blob', hash: hash }));
        }
        const cachedB64 = blobCache[hash];
        const imgDisplay = (isImg && cachedB64) ? 'style="display:block"' : 'style="display:none"';
        const imgSrc = (isImg && cachedB64) ? `src="data:${mime};base64,${cachedB64}"` : '';

        contentHtml = `
          <div class="attachment-card" onclick="downloadBlobAttachment('${escapeHtml(hash)}', '${escapeHtml(fname)}', '${escapeHtml(mime)}')">
            <span>Файл: <strong>${escapeHtml(fname)}</strong> (${Math.max(1, Math.round(fsize / 1024))} КБ) — Скачать</span>
          </div>
          ${isImg ? `<div class="attachment-preview"><img id="img-${escapeHtml(hash)}" ${imgSrc} ${imgDisplay}/></div>` : ''}
        `;
      }
    }

    const timeStr = msg.time || formatTimestamp(msg.timestamp_ms);

    group.innerHTML = `
      <div class="msg-avatar">${escapeHtml(initial)}</div>
      <div class="msg-body">
        <div class="msg-meta">
          <span class="msg-author">${escapeHtml(msg.author || 'Собеседник')}</span>
          <span class="msg-time">${escapeHtml(timeStr)}</span>
        </div>
        <div class="msg-content" id="content-${escapeHtml(sig)}">${contentHtml}</div>
        <div class="reaction-bar" id="reacts-${escapeHtml(sig)}">${escapeHtml(msg.reactions || '')}</div>
        <div class="msg-actions">
          <button class="action-btn" title="Нравится" aria-label="Нравится" onclick="sendReaction('${escapeHtml(sig)}', '+1')">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M20.84 4.61a5.5 5.5 0 0 0-7.78 0L12 5.67l-1.06-1.06a5.5 5.5 0 0 0-7.78 7.78l1.06 1.06L12 21.23l7.78-7.78 1.06-1.06a5.5 5.5 0 0 0 0-7.78z"/></svg>
          </button>
          <button class="action-btn" title="Копировать" aria-label="Копировать" onclick="copyMessageBySig('${escapeHtml(sig)}')">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>
          </button>
          ${isMine ? `<button class="action-btn" title="Изменить" aria-label="Изменить" onclick="promptEditMessage('${escapeHtml(sig)}')"><svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 20h9"/><path d="M16.5 3.5a2.121 2.121 0 0 1 3 3L7 19l-4 1 1-4L16.5 3.5z"/></svg></button>` : ''}
          ${(isMine || isDmMsg) ? `<button class="action-btn" title="Удалить у обоих" aria-label="Удалить у обоих" onclick="deleteMessage('${escapeHtml(sig)}')"><svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/><line x1="10" y1="11" x2="10" y2="17"/><line x1="14" y1="11" x2="14" y2="17"/></svg></button>` : ''}
        </div>
      </div>
    `;

    feed.appendChild(group);
    feed.scrollTop = feed.scrollHeight;
  }

  /* ======================== ACTIONS & COMMAND DISPATCHERS ======================== */
  function addContactOrConnect(rawTarget, customNick) {
    const parsed = parseTargetInput(rawTarget);
    if (!parsed.raw) {
      showToast('Вставьте код друга или ссылку-приглашение');
      return;
    }

    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({
        action: 'connect_peer',
        target: parsed.raw
      }));
    }

    if (parsed.pk) {
      const nick = (customNick && customNick.trim())
        ? customNick.trim()
        : (contacts[parsed.pk] ? contacts[parsed.pk].nickname : ('Друг ' + parsed.pk.substring(0, 4).toUpperCase()));

      upsertContact(parsed.pk, {
        nickname: nick,
        online: contacts[parsed.pk] ? contacts[parsed.pk].online : false
      });
      saveContacts();
      selectTab('dms');
      openDmConversation(parsed.pk);
      closeModals();
      showToast('Чат с другом открыт!');
    } else {
      closeModals();
      showToast('Подключаемся к другу...');
    }
  }

  function submitConnectPeer() {
    const input = getEl('connect-target-input', 'add-contact-input');
    if (!input || !input.value.trim()) return;
    const val = input.value.trim();
    input.value = '';
    addContactOrConnect(val);
  }

  function submitCurrentMessage() {
    const inputEl = getEl('msg-input', 'chat-input');
    if (!inputEl) return;
    const text = inputEl.value.trim();
    if (!text) return;

    if (!ws || ws.readyState !== WebSocket.OPEN) {
      showToast('Подождите секунду, идет подключение...');
      return;
    }

    if (activeChatMode === 'dm' && currentDmPeerPk) {
      ws.send(JSON.stringify({
        action: 'send_dm',
        target_pk: currentDmPeerPk,
        text: text
      }));
      upsertContact(currentDmPeerPk, {
        last_msg: text,
        last_ts: Date.now()
      });
      saveContacts();
      renderContactList();
    } else {
      ws.send(JSON.stringify({
        action: 'send_message',
        space_id: currentSpaceId,
        channel: currentChannel,
        text: text
      }));
    }

    inputEl.value = '';
  }

  function switchSpace(spaceId) {
    const sp = spacesCache.find((s) => s.id === spaceId);
    if (!sp) return;
    currentSpaceId = sp.id;
    renderSpacesList();
    renderChannelsList();
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ action: 'switch_space', space_id: spaceId }));
    }
    if (sp.channels && sp.channels.length > 0) {
      openSwarmChannel(sp.id, sp.channels[0]);
    }
  }

  function switchChannel(ch) {
    openSwarmChannel(currentSpaceId, ch);
  }

  function startDirectMessage(peerOrPk) {
    if (!peerOrPk) return;
    if (typeof peerOrPk === 'string') {
      openDmConversation(peerOrPk);
    } else if (peerOrPk.public_key) {
      upsertContact(peerOrPk.public_key, {
        nickname: peerOrPk.nickname || ('Друг ' + peerOrPk.public_key.substring(0, 4).toUpperCase()),
        ip: peerOrPk.ip || '',
        port: peerOrPk.port || 0,
        online: true
      });
      saveContacts();
      openDmConversation(peerOrPk.public_key);
    }
  }

  function saveProfileNick() {
    const input = getEl('profile-nick-input');
    if (!input) return;
    const newNick = input.value.trim();
    if (newNick && ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ action: 'set_profile', nickname: newNick, status: 'Online' }));
      showToast('Ваше имя сохранено!');
    }
    closeModals();
  }

  function toggleSeedReveal() {
    const box = getEl('seed-hex-box');
    if (!box) return;
    const willReveal = !box.classList.contains('revealed');
    box.classList.toggle('revealed', willReveal);
    if (willReveal && !myExportedSeed && ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ action: 'export_secret' }));
    }
  }

  function exportMasterSeed() {
    if (myExportedSeed) {
      copyTextWithToast(myExportedSeed, 'Резервный код скопирован!');
      return;
    }
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ action: 'export_secret' }));
    }
  }

  function copyMasterSeed() {
    exportMasterSeed();
  }

  function importMasterSeed(seedHexArg) {
    const input = getEl('seed-import-input', 'import-seed-input');
    const seedHex = (seedHexArg || (input ? input.value : '')).trim();
    if (!isValidHex64(seedHex)) {
      showToast('Пожалуйста, вставьте 64-значный резервный код');
      return;
    }
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ action: 'import_secret', seed_hex: seedHex }));
      if (input) input.value = '';
      showToast('Профиль успешно восстановлен!');
    }
  }

  function submitImportSeed() {
    importMasterSeed();
  }

  function triggerLanBeacon() {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ action: 'lan_beacon' }));
      showToast('Ищем друзей в локальной сети...');
    }
  }

  function triggerLanDiscovery() {
    triggerLanBeacon();
  }

  function triggerStunProbe() {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ action: 'stun_probe' }));
      showToast('Обновляем подключение к сети...');
    }
  }

  function triggerPullOffline() {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ action: 'pull_offline' }));
      showToast('Проверяем новые сообщения...');
    }
  }

  /* ======================== FILE BLOBS, REACTIONS & VOICE ======================== */
  function handleFileUpload(e) {
    const file = e && e.target && e.target.files ? e.target.files[0] : null;
    if (!file) return;
    if (file.size > 2 * 1024 * 1024) {
      showToast('Максимальный размер файла — 2 МБ');
      return;
    }
    const reader = new FileReader();
    reader.onload = () => {
      const commaIdx = String(reader.result).indexOf(',');
      const b64 = commaIdx >= 0 ? String(reader.result).substring(commaIdx + 1) : '';
      if (ws && ws.readyState === WebSocket.OPEN && b64) {
        ws.send(JSON.stringify({
          action: 'upload_blob',
          filename: file.name,
          mime_type: file.type || 'application/octet-stream',
          channel: currentChannel,
          data_base64: b64
        }));
        showToast('Отправляем файл...');
      }
    };
    reader.readAsDataURL(file);
    e.target.value = '';
  }

  function downloadBlobAttachment(hash, filename, mime) {
    if (blobCache[hash]) {
      const a = document.createElement('a');
      a.href = `data:${mime || 'application/octet-stream'};base64,${blobCache[hash]}`;
      a.download = filename || 'file';
      a.click();
    } else if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ action: 'get_blob', hash: hash }));
      showToast('Загружаем файл...');
    }
  }

  function sendReaction(targetSig, emoji) {
    if (!targetSig || !ws || ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify({
      action: 'send_reaction',
      target_sig: targetSig,
      emoji: emoji
    }));
  }

  function copyMessageBySig(targetSig) {
    if (!targetSig) return;
    const found = historyCache.find((m) => m && m.signature === targetSig);
    if (found && found.content) {
      copyTextWithToast(found.content, 'Текст скопирован');
      return;
    }
    const el = document.getElementById('content-' + targetSig);
    if (el) {
      copyTextWithToast(el.textContent.replace(' (изм.)', ''), 'Текст скопирован');
    }
  }

  function promptEditMessage(targetSig) {
    if (!targetSig) return;
    const el = document.getElementById('content-' + targetSig);
    const oldText = el ? el.textContent.replace(' (изм.)', '').replace(' (ред.)', '') : '';
    openInputModal('edit_message', targetSig, oldText);
  }

  function removeMessageBySignature(targetSig) {
    if (!targetSig) return;

    historyCache = historyCache.filter((m) => m && m.signature !== targetSig);

    const el = document.getElementById('content-' + targetSig) || document.getElementById('msg-' + targetSig);
    if (el) {
      const grp = el.closest('.msg-group');
      if (grp) grp.remove();
      else el.remove();
    }

    // Update contact last_msg preview from remaining history
    Object.keys(contacts).forEach((pk) => {
      const dmCh = dmChannelForPk(pk).toLowerCase();
      const prefix = pk.toLowerCase().substring(0, 58);
      const dmMsgs = historyCache.filter((m) => {
        if (!m || !m.channel) return false;
        const ch = m.channel.toLowerCase();
        return ch === dmCh || ch === 'dm:' + prefix;
      });
      if (dmMsgs.length > 0) {
        const last = dmMsgs[dmMsgs.length - 1];
        contacts[pk].last_msg = last.content || '';
        contacts[pk].last_ts = last.timestamp_ms || contacts[pk].last_ts || 0;
      } else {
        contacts[pk].last_msg = '';
      }
    });
    saveContacts();
    renderContactList();

    const feed = getEl('messages', 'chat-messages');
    if (feed && !feed.querySelector('.msg-group')) {
      renderHistory(historyCache);
    }
  }

  function deleteMessage(targetSig) {
    if (!targetSig) return;
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({
        action: 'delete_message',
        target_sig: targetSig
      }));
    }
    removeMessageBySignature(targetSig);
    showToast('Сообщение удалено');
  }

  async function toggleVoiceConnect(channel) {
    const targetCh = channel || voiceChannelName || 'voice-lounge';
    voiceChannelName = targetCh;

    if (voiceConnected) {
      if (micStream) {
        micStream.getTracks().forEach((t) => t.stop());
        micStream = null;
      }
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ action: 'leave_voice' }));
      }
    } else {
      try {
        if (navigator.mediaDevices && navigator.mediaDevices.getUserMedia) {
          micStream = await navigator.mediaDevices.getUserMedia({ audio: true });
          const AudioContextClass = window.AudioContext || window.webkitAudioContext;
          if (!audioCtx) audioCtx = new AudioContextClass({ sampleRate: 48000 });
          const source = audioCtx.createMediaStreamSource(micStream);
          const processor = audioCtx.createScriptProcessor(1024, 1, 1);
          source.connect(processor);
          processor.connect(audioCtx.destination);
          processor.onaudioprocess = (e) => {
            if (!voiceConnected || !ws || ws.readyState !== WebSocket.OPEN) return;
            const inputData = e.inputBuffer.getChannelData(0);
            const pcm16 = new Int16Array(inputData.length);
            for (let i = 0; i < inputData.length; i++) {
              pcm16[i] = Math.max(-32768, Math.min(32767, inputData[i] * 32768));
            }
            const uint8 = new Uint8Array(pcm16.buffer);
            let binary = '';
            for (let i = 0; i < uint8.byteLength; i++) {
              binary += String.fromCharCode(uint8[i]);
            }
            ws.send(JSON.stringify({
              action: 'voice_data',
              channel: targetCh,
              data_base64: btoa(binary)
            }));
          };
        }
      } catch (err) {
        console.warn('Voice listen-only fallback:', err);
      }
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ action: 'join_voice', channel: targetCh }));
      }
    }
  }

  function playVoiceChunk(b64) {
    if (!voiceConnected || !b64) return;
    try {
      const AudioContextClass = window.AudioContext || window.webkitAudioContext;
      if (!audioCtx) audioCtx = new AudioContextClass({ sampleRate: 48000 });
      const binary = atob(b64);
      const len = binary.length;
      const bytes = new Uint8Array(len);
      for (let i = 0; i < len; i++) bytes[i] = binary.charCodeAt(i);
      const pcm16 = new Int16Array(bytes.buffer);
      const floatData = new Float32Array(pcm16.length);
      for (let i = 0; i < pcm16.length; i++) floatData[i] = pcm16[i] / 32768.0;

      const buffer = audioCtx.createBuffer(1, floatData.length, 48000);
      buffer.getChannelData(0).set(floatData);
      const srcNode = audioCtx.createBufferSource();
      srcNode.buffer = buffer;
      srcNode.connect(audioCtx.destination);
      srcNode.start();
    } catch (e) {}
  }

  /* ======================== MODALS ======================== */
  function closeModals() {
    stopQrScannerCamera();
    document.querySelectorAll('.modal-overlay').forEach((m) => m.classList.remove('open'));
    pendingModalAction = null;
    pendingTargetSig = null;
  }

  function openInviteModal() {
    ensureQrScannerElements();
    renderIdentityPanel();
    if (myPublicKey) {
      renderAllKeyCanvases(myPublicKey);
    }
    const m = getEl('modal-invite');
    if (m) m.classList.add('open');
  }

  function openConnectModal() {
    const m = getEl('modal-connect');
    const inp = getEl('connect-target-input', 'add-contact-input');
    if (inp) inp.value = '';
    if (m) {
      m.classList.add('open');
      setTimeout(() => { if (inp) inp.focus(); }, 60);
    }
  }

  function openProfileModal() {
    selectTab('key');
    const m = getEl('modal-profile');
    if (m) m.classList.add('open');
  }

  function openInputModal(actionType, targetSig, initialValue) {
    pendingModalAction = actionType;
    pendingTargetSig = targetSig || null;

    const titleEl = getEl('modal-input-title');
    const labelEl = getEl('modal-input-label');
    const fieldEl = getEl('modal-input-field');

    if (actionType === 'create_space') {
      if (titleEl) titleEl.textContent = 'Новое сообщество';
      if (labelEl) labelEl.textContent = 'Название сообщества';
      if (fieldEl) {
        fieldEl.placeholder = 'Например: Друзья и близкие';
        fieldEl.value = '';
      }
    } else if (actionType === 'create_channel') {
      if (titleEl) titleEl.textContent = 'Новая комната';
      if (labelEl) labelEl.textContent = 'Название комнаты';
      if (fieldEl) {
        fieldEl.placeholder = 'Например: общение';
        fieldEl.value = '';
      }
    } else if (actionType === 'edit_message') {
      if (titleEl) titleEl.textContent = 'Изменить сообщение';
      if (labelEl) labelEl.textContent = 'Текст сообщения';
      if (fieldEl) {
        fieldEl.placeholder = '';
        fieldEl.value = initialValue || '';
      }
    }

    const m = getEl('modal-input');
    if (m) {
      m.classList.add('open');
      setTimeout(() => { if (fieldEl) fieldEl.focus(); }, 60);
    }
  }

  function submitInputModal() {
    const fieldEl = getEl('modal-input-field');
    const val = fieldEl ? fieldEl.value.trim() : '';
    if (!val || !ws || ws.readyState !== WebSocket.OPEN) {
      closeModals();
      return;
    }

    if (pendingModalAction === 'create_space') {
      ws.send(JSON.stringify({ action: 'create_space', name: val }));
      showToast('Сообщество создано!');
    } else if (pendingModalAction === 'create_channel') {
      ws.send(JSON.stringify({ action: 'create_channel', name: val }));
      showToast('Комната создана!');
    } else if (pendingModalAction === 'edit_message' && pendingTargetSig) {
      ws.send(JSON.stringify({ action: 'edit_message', target_sig: pendingTargetSig, text: val }));
    }
    closeModals();
  }

  function toggleMobileDrawer(forceState) {
    if (typeof forceState === 'boolean') {
      document.body.classList.toggle('drawer-open', forceState);
      if (forceState) document.body.classList.remove('chat-active');
    } else {
      document.body.classList.toggle('drawer-open');
    }
  }

  /* ======================== WEBSOCKET CONNECTION & EVENT LOOP ======================== */
  function handleIncomingWsMessage(data) {
    if (!data || !data.type) return;

    if (data.type === 'status') {
      if (data.identity) {
        myNick = data.identity.nickname || 'Пользователь';
        myStatus = data.identity.status || 'В сети';
        myUdpPort = data.identity.udp_port || 9001;
        myPublicKey = data.identity.public_key || '';
        window.myPublicKey = myPublicKey;
        myX25519Key = data.identity.x25519_key || '';
        myInviteUri = data.identity.invite_uri || '';
        myDhtCount = data.identity.dht_contacts || 0;
        myOfflineShards = data.identity.offline_shards || 0;
        myWanEndpoint = data.identity.external_endpoint || '';
      }

      if (Array.isArray(data.spaces) && data.spaces.length > 0) {
        spacesCache = data.spaces;
        const activeSp = data.spaces.find((s) => s.id === currentSpaceId) || data.spaces[0];
        currentSpaceId = activeSp.id;
      }

      if (Array.isArray(data.peers)) {
        peersCache = data.peers;
      }

      if (Array.isArray(data.dht_nodes)) {
        dhtNodesCache = data.dht_nodes;
      }

      if (Array.isArray(data.history)) {
        historyCache = normalizeAndSortHistory(data.history);
      }

      mergePeersAndHistoryIntoContacts(peersCache, historyCache);

      if (!currentDmPeerPk && activeChatMode === 'dm') {
        const contactKeys = Object.keys(contacts);
        if (contactKeys.length > 0) {
          currentDmPeerPk = contactKeys[0];
          currentChannel = dmChannelForPk(currentDmPeerPk);
        }
      }

      renderIdentityPanel();
      renderContactList();
      renderPeersList();
      renderSpacesList();
      renderChannelsList();
      renderNetworkTelemetry();
      updateChatHeader();
      renderHistory(historyCache);

      if (data.voice) {
        voiceConnected = Boolean(data.voice.connected);
        const vBar = getEl('voice-bar');
        const vBtn = getEl('voice-btn');
        const vBadge = getEl('voice-badge');
        if (vBar) vBar.classList.toggle('active', voiceConnected);
        if (vBtn) vBtn.classList.toggle('connected', voiceConnected);
        if (vBadge) vBadge.textContent = voiceConnected ? 'На связи' : 'Войти';
      }
      return;
    }

    if (data.type === 'peer_joined' || data.type === 'peer_left') {
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ action: 'get_status' }));
      }
      return;
    }

    if (data.type === 'message') {
      const isDup = data.signature && historyCache.some((m) => m.signature === data.signature);
      if (isDup) return;

      const prevLastTs = historyCache.length > 0
        ? (Number(historyCache[historyCache.length - 1].timestamp_ms) || 0)
        : 0;
      const incomingTs = Number(data.timestamp_ms) || Date.now();
      historyCache.push(data);
      historyCache = normalizeAndSortHistory(historyCache);

      const isFromMe = (data.author === myNick) || (myPublicKey && data.author_pk && data.author_pk.toLowerCase() === myPublicKey.toLowerCase());
      const isDm = typeof data.channel === 'string' && data.channel.startsWith('dm:');

      if (isDm) {
        const dmPrefix = data.channel.substring(3).toLowerCase();
        let peerKey = null;
        if (!isFromMe && data.author_pk && isValidHex64(data.author_pk)) {
          peerKey = data.author_pk.toLowerCase();
        } else if (data.peer_pk && isValidHex64(data.peer_pk)) {
          peerKey = data.peer_pk.toLowerCase();
        } else {
          peerKey = findContactKeyByPrefix(dmPrefix) || dmPrefix;
        }

        if (peerKey) {
          const isCurrentlyOpen = activeChatMode === 'dm' && currentDmPeerPk && currentDmPeerPk.startsWith(peerKey.substring(0, 58)) &&
            (window.innerWidth >= 768 || document.body.classList.contains('chat-active'));
          const prevUnread = contacts[peerKey] ? (contacts[peerKey].unread || 0) : 0;

          upsertContact(peerKey, {
            nickname: (!isFromMe && data.author) ? data.author : (contacts[peerKey] ? contacts[peerKey].nickname : undefined),
            last_msg: data.content || '',
            last_ts: incomingTs,
            unread: (!isFromMe && !isCurrentlyOpen) ? prevUnread + 1 : 0
          });
          saveContacts();
          renderContactList();
        }
      }

      if (!isFromMe) {
        playIncomingDmTick();
      }

      if (messageMatchesCurrentConversation(data)) {
        if (incomingTs < prevLastTs) {
          renderHistory(historyCache);
        } else {
          appendMessageToDom(data);
        }
      }
      return;
    }

    if (data.type === 'secret_exported') {
      myExportedSeed = data.seed_hex || '';
      setText(['seed-hex-box', 'exported-seed-box', 'master-seed-box'], myExportedSeed);
      copyTextWithToast(myExportedSeed, 'Резервный код скопирован!');
      return;
    }

    if (data.type === 'reaction_updated') {
      const el = document.getElementById('reacts-' + data.target_sig);
      if (el) el.textContent = data.summary || '';
      return;
    }

    if (data.type === 'message_edited') {
      const el = document.getElementById('content-' + data.target_sig);
      if (el) el.textContent = (data.content || '') + ' (изм.)';
      return;
    }

    if (data.type === 'message_deleted') {
      removeMessageBySignature(data.target_sig);
      return;
    }

    if (data.type === 'voice_state') {
      voiceConnected = Boolean(data.connected);
      const vBar = getEl('voice-bar');
      const vBtn = getEl('voice-btn');
      const vBadge = getEl('voice-badge');
      if (vBar) vBar.classList.toggle('active', voiceConnected);
      if (vBtn) vBtn.classList.toggle('connected', voiceConnected);
      if (vBadge) vBadge.textContent = voiceConnected ? 'На связи' : 'Войти';
      return;
    }

    if (data.type === 'voice_stream') {
      playVoiceChunk(data.data_base64);
      return;
    }

    if (data.type === 'blob_data') {
      blobCache[data.hash] = data.data_base64;
      const img = document.getElementById('img-' + data.hash);
      if (img) {
        img.src = 'data:image/png;base64,' + data.data_base64;
        img.style.display = 'block';
      }
      return;
    }

    if (data.type === 'blob_received') {
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ action: 'get_blob', hash: data.hash }));
      }
    }
  }

  function connectWS() {
    clearTimeout(reconnectTimer);
    const loc = window.location;
    const wsUrl = (loc.protocol === 'https:' ? 'wss://' : 'ws://') + (loc.host || '127.0.0.1:9000');

    try {
      ws = new WebSocket(wsUrl);
    } catch (e) {
      reconnectTimer = setTimeout(connectWS, 1200);
      return;
    }

    ws.onopen = () => {
      ws.send(JSON.stringify({ action: 'get_status' }));
    };

    ws.onmessage = (event) => {
      try {
        const data = JSON.parse(event.data);
        handleIncomingWsMessage(data);
      } catch (e) {}
    };

    ws.onclose = () => {
      setText(['my-node-status'], 'Подключение...');
      renderNetworkTelemetry();
      reconnectTimer = setTimeout(connectWS, 1200);
    };
  }

  /* ======================== VIEWPORT & KEYBOARD LISTENERS ======================== */
  function bindEvents() {
    if (window.visualViewport) {
      const updateViewportHeight = () => {
        const root = getEl('app-root');
        if (root) {
          root.style.height = window.visualViewport.height + 'px';
        }
        const feed = getEl('messages', 'chat-messages');
        if (feed) feed.scrollTop = feed.scrollHeight;
      };
      window.visualViewport.addEventListener('resize', updateViewportHeight);
    }

    const msgInput = getEl('msg-input', 'chat-input');
    if (msgInput) {
      msgInput.addEventListener('keydown', (e) => {
        if (e.key === 'Enter' && !e.shiftKey) {
          e.preventDefault();
          submitCurrentMessage();
        }
      });
    }

    const connectInput = getEl('connect-target-input', 'add-contact-input');
    if (connectInput) {
      connectInput.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') {
          e.preventDefault();
          submitConnectPeer();
        }
      });
    }

    const modalInput = getEl('modal-input-field');
    if (modalInput) {
      modalInput.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') {
          e.preventDefault();
          submitInputModal();
        }
      });
    }
  }

  /* ======================== INITIALIZATION & GLOBAL EXPORTS ======================== */
  function initWexApp() {
    loadContacts();
    ensureQrScannerElements();
    bindEvents();
    selectTab('dms');
    renderContactList();
    connectWS();
  }

  window.selectTab = selectTab;
  window.switchModeTab = switchModeTab;
  window.openDmConversation = openDmConversation;
  window.openSwarmChannel = openSwarmChannel;
  window.closeActiveChat = closeActiveChat;
  window.closeMobileConversation = closeMobileConversation;
  window.addContactOrConnect = addContactOrConnect;
  window.submitConnectPeer = submitConnectPeer;
  window.submitCurrentMessage = submitCurrentMessage;
  window.switchSpace = switchSpace;
  window.switchChannel = switchChannel;
  window.startDirectMessage = startDirectMessage;
  window.copyTextWithToast = copyTextWithToast;
  window.pasteIntoInput = pasteIntoInput;
  window.shareInviteNative = shareInviteNative;
  window.showToast = showToast;
  window.closeModals = closeModals;
  window.openInviteModal = openInviteModal;
  window.openConnectModal = openConnectModal;
  window.openProfileModal = openProfileModal;
  window.openInputModal = openInputModal;
  window.submitInputModal = submitInputModal;
  window.saveProfileNick = saveProfileNick;
  window.toggleSeedReveal = toggleSeedReveal;
  window.exportMasterSeed = exportMasterSeed;
  window.copyMasterSeed = copyMasterSeed;
  window.importMasterSeed = importMasterSeed;
  window.submitImportSeed = submitImportSeed;
  window.triggerLanBeacon = triggerLanBeacon;
  window.triggerLanDiscovery = triggerLanDiscovery;
  window.triggerStunProbe = triggerStunProbe;
  window.triggerPullOffline = triggerPullOffline;
  window.handleFileUpload = handleFileUpload;
  window.downloadBlobAttachment = downloadBlobAttachment;
  window.sendReaction = sendReaction;
  window.copyMessageBySig = copyMessageBySig;
  window.promptEditMessage = promptEditMessage;
  window.deleteMessage = deleteMessage;
  window.toggleVoiceConnect = toggleVoiceConnect;
  window.toggleMobileDrawer = toggleMobileDrawer;
  window.renderKeyMatrixCanvas = renderKeyMatrixCanvas;
  window.openQrScannerModal = openQrScannerModal;
  window.closeQrScannerModal = closeQrScannerModal;
  window.switchScannerCamera = switchScannerCamera;
  window.handleQrImageFile = handleQrImageFile;
  window.buildIsoQrV4Matrix = buildIsoQrV4Matrix;
  window.decodeIsoQrV4Matrix = decodeIsoQrV4Matrix;
  window.decodeWexQrFromImageData = decodeWexQrFromImageData;

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initWexApp);
  } else {
    initWexApp();
  }
})();
