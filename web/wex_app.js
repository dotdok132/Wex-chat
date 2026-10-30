/**
 * WEX // PROTOCOL — Frontend Controller & E2EE DM State Machine
 * File: web/wex_app.js
 * Zero external dependencies. Designed for Desktop, Tablet Split-View, and Mobile Master-Detail.
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
  let currentChannel = '';
  let currentDmPeerPk = null;

  /* Local Node Identity & Telemetry */
  let myNick = 'User';
  let myStatus = 'Online';
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

  function shortKey(pk) {
    if (!pk || pk.length < 16) return pk || 'unknown';
    return pk.substring(0, 8) + '…' + pk.substring(pk.length - 6);
  }

  function formatTimestamp(tsMs) {
    if (!tsMs) {
      const now = new Date();
      return now.toTimeString().substring(0, 5);
    }
    const d = new Date(Number(tsMs));
    if (isNaN(d.getTime())) return 'сейчас';
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

    const existing = contacts[key] || {
      public_key: key,
      nickname: 'Peer-' + key.substring(0, 6),
      ip: '',
      port: 0,
      online: false,
      use_relay: false,
      last_msg: '',
      last_ts: 0,
      unread: 0
    };

    const updated = Object.assign({}, existing, patch || {}, { public_key: key });
    if (!updated.nickname || updated.nickname === 'Anon' || updated.nickname.startsWith('Peer-')) {
      if (patch && patch.nickname && patch.nickname !== 'Anon') {
        updated.nickname = patch.nickname;
      } else if (!existing.nickname) {
        updated.nickname = 'Peer-' + key.substring(0, 6);
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
          nickname: p.nickname || ('Peer-' + p.public_key.substring(0, 6)),
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
            nickname: m.author || ('Peer-' + senderPk.substring(0, 6))
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
        nickname: 'Peer-' + currentDmPeerPk.substring(0, 6),
        public_key: currentDmPeerPk,
        online: false,
        use_relay: false
      };

      if (prefixEl) prefixEl.textContent = 'E2EE // ЛС';
      if (titleEl) titleEl.textContent = c.nickname || shortKey(currentDmPeerPk);

      let routeLabel = `PK: ${shortKey(currentDmPeerPk)} // ОФЛАЙН-ОЧЕРЕДЬ QUAS (X25519)`;
      if (c.online) {
        routeLabel = c.use_relay
          ? `ОНЛАЙН // DHT OVERLAY RELAY (${shortKey(currentDmPeerPk)})`
          : `ОНЛАЙН // ПРЯМОЙ UDP P2P (${c.ip ? c.ip + ':' + c.port : shortKey(currentDmPeerPk)})`;
      }
      if (topicEl) topicEl.textContent = routeLabel;
      if (inputEl) inputEl.placeholder = `Зашифрованное ЛС для ${c.nickname || shortKey(currentDmPeerPk)} (ChaCha20-Poly1305)...`;
    } else {
      const sp = spacesCache.find((s) => s.id === currentSpaceId);
      const spName = sp ? sp.name : 'Нет выбранной группы';
      if (prefixEl) prefixEl.textContent = 'SWARM #';
      if (titleEl) titleEl.textContent = currentChannel;
      if (topicEl) topicEl.textContent = `${spName} // ED25519 SIGNED MESH`;
      if (inputEl) inputEl.placeholder = `Сообщение в #${currentChannel}...`;
    }
  }

  /* ======================== PURE-CANVAS 21x21 KEY MATRIX / QR RENDERER ======================== */
  function drawFinderPattern(ctx, ox, oy, cellSize, fgColor, bgColor) {
    for (let r = 0; r < 7; r++) {
      for (let c = 0; c < 7; c++) {
        const isBorder = r === 0 || r === 6 || c === 0 || c === 6;
        const isInner = r >= 2 && r <= 4 && c >= 2 && c <= 4;
        ctx.fillStyle = (isBorder || isInner) ? fgColor : bgColor;
        ctx.fillRect((ox + c) * cellSize, (oy + r) * cellSize, cellSize, cellSize);
      }
    }
  }

  function renderKeyMatrixCanvas(canvasOrId, hexKey) {
    const canvas = typeof canvasOrId === 'string' ? document.getElementById(canvasOrId) : canvasOrId;
    if (!canvas || !canvas.getContext) return;

    const cleanHex = (hexKey || '').replace(/[^0-9a-fA-F]/g, '').padEnd(64, '0');
    const bytes = new Uint8Array(32);
    for (let i = 0; i < 32; i++) {
      bytes[i] = parseInt(cleanHex.substr(i * 2, 2), 16) || 0;
    }

    const gridSize = 21;
    const size = canvas.width || 168;
    const padding = 8;
    const innerSize = size - padding * 2;
    const cellSize = Math.floor(innerSize / gridSize);
    const offset = Math.floor((size - cellSize * gridSize) / 2);

    const ctx = canvas.getContext('2d');
    const bgColor = '#0a0b0d';
    const fgPrimary = '#eceef2';
    const fgAccent = '#10b981';

    ctx.fillStyle = bgColor;
    ctx.fillRect(0, 0, canvas.width, canvas.height);

    ctx.save();
    ctx.translate(offset, offset);

    drawFinderPattern(ctx, 0, 0, cellSize, fgAccent, bgColor);
    drawFinderPattern(ctx, gridSize - 7, 0, cellSize, fgAccent, bgColor);
    drawFinderPattern(ctx, 0, gridSize - 7, cellSize, fgAccent, bgColor);

    for (let i = 8; i < gridSize - 8; i++) {
      ctx.fillStyle = (i % 2 === 0) ? fgPrimary : bgColor;
      ctx.fillRect(i * cellSize, 6 * cellSize, cellSize, cellSize);
      ctx.fillRect(6 * cellSize, i * cellSize, cellSize, cellSize);
    }

    let bitIdx = 0;
    for (let r = 0; r < gridSize; r++) {
      for (let c = 0; c < gridSize; c++) {
        const inTL = r < 8 && c < 8;
        const inTR = r < 8 && c >= gridSize - 8;
        const inBL = r >= gridSize - 8 && c < 8;
        const isTiming = r === 6 || c === 6;
        if (inTL || inTR || inBL || isTiming) continue;

        const byteA = bytes[bitIdx % 32];
        const byteB = bytes[(bitIdx * 7 + r * 3 + c * 5) % 32];
        const shift = (bitIdx + r + c) % 8;
        const bit = ((byteA ^ byteB) >> shift) & 1;

        if (bit === 1) {
          ctx.fillStyle = ((r + c) % 5 === 0) ? fgAccent : fgPrimary;
          ctx.fillRect(c * cellSize, r * cellSize, cellSize - 0.5, cellSize - 0.5);
        }
        bitIdx++;
      }
    }

    ctx.restore();
  }

  function renderAllKeyCanvases(hexKey) {
    const ids = ['profile-qr-canvas', 'identity-qr-canvas', 'qr-canvas', 'key-matrix-canvas'];
    ids.forEach((id) => {
      const c = document.getElementById(id);
      if (c) renderKeyMatrixCanvas(c, hexKey);
    });
  }

  /* ======================== HARDWARE-STYLE WEB AUDIO SYNTHESIZER ======================== */
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
      osc.frequency.setValueAtTime(1046.5, now);
      osc.frequency.exponentialRampToValueAtTime(1567.98, now + 0.045);

      gain.gain.setValueAtTime(0.001, now);
      gain.gain.exponentialRampToValueAtTime(0.08, now + 0.008);
      gain.gain.exponentialRampToValueAtTime(0.0001, now + 0.085);

      osc.connect(gain);
      gain.connect(audioCtx.destination);

      osc.start(now);
      osc.stop(now + 0.09);
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
      showToast(toastMsg || 'СКОПИРОВАНО В БУФЕР');
      return;
    }
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(String(text)).then(() => {
        showToast(toastMsg || 'СКОПИРОВАНО В БУФЕР');
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
      showToast(toastMsg || 'СКОПИРОВАНО В БУФЕР');
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
        showToast('ВСТАВЛЕНО ИЗ БУФЕРА');
        return;
      }
    }
    if (navigator.clipboard && navigator.clipboard.readText) {
      try {
        const clip = await navigator.clipboard.readText();
        if (clip) {
          el.value = clip.trim();
          showToast('ВСТАВЛЕНО ИЗ БУФЕРА');
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
      navigator.share({ title: 'WEX // Ключ и инвайт', text: link }).catch(() => {});
    } else {
      copyTextWithToast(link, 'URI СКОПИРОВАН В БУФЕР');
    }
  }

  /* ======================== RENDER FUNCTIONS ======================== */
  function renderContactList() {
    const container = getEl('dm-container', 'dm-list', 'contacts-list');
    if (!container) return;

    const list = Object.values(contacts);

    // Update badge count
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
          Нет активных диалогов. Нажмите «+ ДОБАВИТЬ КОНТАКТ / КЛЮЧ» выше и вставьте 64-значный Ed25519 ключ собеседника или инвайт wex:// для прямого E2EE ЛС.
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

      const dotClass = c.online ? (c.use_relay ? 'amber' : 'online') : 'offline';
      const transportTag = c.online ? (c.use_relay ? 'RELAY' : 'DIRECT') : 'QUAS';
      const preview = c.last_msg
        ? (c.last_msg.length > 34 ? c.last_msg.substring(0, 34) + '…' : c.last_msg)
        : shortKey(c.public_key);

      item.innerHTML = `
        <div class="peer-left">
          <span class="peer-dot ${dotClass}" style="background:${c.online ? (c.use_relay ? '#f59e0b' : '#10b981') : '#4a5160'};"></span>
          <div class="peer-meta">
            <span class="peer-name">${escapeHtml(c.nickname)}</span>
            <span class="peer-sub">${escapeHtml(preview)}</span>
          </div>
        </div>
        <div class="item-right">
          ${c.unread > 0
            ? `<span class="crypto-tag" style="background:#10b981; color:#0a0b0d; font-weight:700;">+${c.unread}</span>`
            : `<span class="crypto-tag">${transportTag}</span>`
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
      container.innerHTML = '<div class="empty-list-hint">Радиомаяк LAN (UDP :9009) и таблица Kademlia DHT ожидают отклика узлов...</div>';
      return;
    }

    peersCache.forEach((p) => {
      const item = document.createElement('div');
      item.className = 'peer-item';
      item.innerHTML = `
        <div class="peer-left">
          <span class="peer-dot online" style="background:#10b981;"></span>
          <div class="peer-meta">
            <span class="peer-name">${escapeHtml(p.nickname || 'Node')}</span>
            <span class="peer-sub">${escapeHtml(p.ip || '')}:${p.port || ''} // ${shortKey(p.public_key)}</span>
          </div>
        </div>
        <span class="crypto-tag">E2EE ЛС →</span>
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
      btn.textContent = sp.name || 'Swarm';
      btn.title = `${sp.name} (${shortKey(sp.id)})`;
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
    setText(['server-title'], activeSp ? activeSp.name : 'Нет выбранной группы');
    setText(['active-swarm-id-label'], activeSp ? `SWARM ID: ${shortKey(activeSp.id)}` : 'P2P MESH SWARM');

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
    setText(['net-daemon-state'], isOnline ? 'ONLINE // C99 P2P' : 'RECONNECTING');
    setText(['net-udp-port'], `UDP :${myUdpPort}`);
    setText(['net-wan-endpoint'], myWanEndpoint || 'Auto NAT Hole-Punch / DHT Relay');
    setText(['net-dht-count'], String(myDhtCount));
    setText(['net-quas-count'], `${myOfflineShards} SHARDS`);

    const pulseDot = getEl('node-pulse-indicator');
    if (pulseDot) {
      pulseDot.style.background = isOnline ? '#10b981' : '#f59e0b';
    }

    const diagLines = [
      `DAEMON    : ${isOnline ? 'ACTIVE (C99 Native Engine)' : 'RECONNECTING'}`,
      `LOCAL UDP : 0.0.0.0:${myUdpPort}`,
      `WAN STUN  : ${myWanEndpoint || 'NAT Traversal / Overlay Relay'}`,
      `KADEMLIA  : ${myDhtCount} DHT nodes | ${peersCache.length} direct peers | ${myOfflineShards} Quas shards`,
      `ED25519   : ${myPublicKey}`,
      `X25519    : ${myX25519Key || 'Derived from Ed25519'}`
    ];

    if (dhtNodesCache && dhtNodesCache.length > 0) {
      diagLines.push('--- DHT ROUTING TABLE ---');
      dhtNodesCache.slice(0, 8).forEach((n) => {
        diagLines.push(`[B${n.bucket}] ${shortKey(n.id)} -> ${n.ip}:${n.port}`);
      });
    }

    setText(['profile-diag-box'], diagLines.join('\n'));
  }

  function renderIdentityPanel() {
    window.myPublicKey = myPublicKey;
    setText(['my-nickname'], myNick);
    setText(['my-avatar'], (myNick || 'W').charAt(0).toUpperCase());
    setText(['my-node-status'], myWanEndpoint
      ? `WAN ${myWanEndpoint} // DHT:${myDhtCount}`
      : `UDP :${myUdpPort} // DHT:${myDhtCount}`);

    const inviteUri = myInviteUri || (myPublicKey ? `wex://join/${currentSpaceId}?peer=127.0.0.1:${myUdpPort}&pk=${myPublicKey}` : 'wex://join/...');
    setText(['key-pubkey-display', 'invite-pk-box', 'identity-pk-box'], myPublicKey || 'Генерация...');
    setText(['invite-link-box', 'identity-invite-box'], inviteUri);

    const nickInput = getEl('profile-nick-input');
    if (nickInput && document.activeElement !== nickInput) {
      nickInput.value = myNick;
    }

    if (myPublicKey) {
      renderAllKeyCanvases(myPublicKey);
    }
  }

  /* ======================== MESSAGE FEED & E2EE RENDERING ======================== */
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

  function renderHistory(historyList) {
    const feed = getEl('messages', 'chat-messages');
    if (!feed) return;
    feed.innerHTML = '';

    const filtered = (historyList || []).filter(messageMatchesCurrentConversation);
    if (filtered.length === 0) {
      if (activeChatMode === 'dm' && currentDmPeerPk) {
        const c = contacts[currentDmPeerPk];
        const name = c ? c.nickname : shortKey(currentDmPeerPk);
        feed.innerHTML = `
          <div class="empty-state">
            <span class="empty-state-tag">X25519 + CHACHA20-POLY1305 // E2EE ЛС</span>
            <h3>Личный диалог: ${escapeHtml(name)}</h3>
            <p style="font-family:var(--font-mono, monospace); font-size:11px; word-break:break-all;">PK: ${escapeHtml(currentDmPeerPk)}</p>
            <p>Сообщения шифруются сквозным ключом X25519. Если собеседник сейчас офлайн, зашифрованный пакет сохраняется в Quas Store-and-Forward и доставляется при его появлении в сети.</p>
          </div>`;
      } else {
        feed.innerHTML = `
          <div class="empty-state">
            <span class="empty-state-tag">ED25519 SIGNED SWARM</span>
            <h3>Канал #${escapeHtml(currentChannel)}</h3>
            <p>Сообщения подписываются ключом Ed25519 и синхронизируются напрямую между участниками сварм-канала без центральных серверов.</p>
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
    if (isMine) group.classList.add('mine');

    const initial = (msg.author || 'U').charAt(0).toUpperCase();
    const isDmMsg = (msg.channel && msg.channel.startsWith('dm:')) || msg.encrypted;

    const verifiedBadge = (msg.verified !== false)
      ? `<span class="verified-badge">ED25519 ✓</span>`
      : '';
    const e2eeBadge = isDmMsg
      ? `<span class="e2ee-badge">CHACHA20 E2EE</span>`
      : '';

    let contentHtml = escapeHtml(msg.content || '');
    if (msg.edited) {
      contentHtml += ' <span style="font-size:11px; color:#7d8594;">(ред.)</span>';
    }

    const rawContent = msg.content || '';
    if (rawContent.startsWith('[FILE:') || rawContent.startsWith('[attachment:')) {
      const closeIdx = rawContent.indexOf(']');
      const prefixLen = rawContent.startsWith('[FILE:') ? 6 : 12;
      if (closeIdx > prefixLen) {
        const meta = rawContent.substring(prefixLen, closeIdx).split(':');
        const hash = meta[0];
        const fname = meta[1] || 'file.bin';
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
            <span>[BLOB] <strong>${escapeHtml(fname)}</strong> (${Math.max(1, Math.round(fsize / 1024))} KB) — Скачать</span>
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
          <span class="msg-author">${escapeHtml(msg.author || 'Anon')}</span>
          ${verifiedBadge}
          ${e2eeBadge}
          <span class="msg-time">${escapeHtml(timeStr)}</span>
        </div>
        <div class="msg-content" id="content-${escapeHtml(sig)}">${contentHtml}</div>
        <div class="reaction-bar" id="reacts-${escapeHtml(sig)}">${escapeHtml(msg.reactions || '')}</div>
        <div class="msg-actions">
          <button class="action-btn" onclick="sendReaction('${escapeHtml(sig)}', '+1')">+1</button>
          <button class="action-btn" onclick="sendReaction('${escapeHtml(sig)}', 'ACK')">ACK</button>
          <button class="action-btn" onclick="sendReaction('${escapeHtml(sig)}', '🔥')">🔥</button>
          ${isMine ? `<button class="action-btn" onclick="promptEditMessage('${escapeHtml(sig)}')">EDIT</button>` : ''}
          ${isMine ? `<button class="action-btn" onclick="deleteMessage('${escapeHtml(sig)}')">DEL</button>` : ''}
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
      showToast('ВВЕДИТЕ PUBLIC KEY (64 HEX), WEX:// ИЛИ IP:PORT');
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
        : (contacts[parsed.pk] ? contacts[parsed.pk].nickname : ('Peer-' + parsed.pk.substring(0, 6)));

      upsertContact(parsed.pk, {
        nickname: nick,
        online: contacts[parsed.pk] ? contacts[parsed.pk].online : false
      });
      saveContacts();
      selectTab('dms');
      openDmConversation(parsed.pk);
      closeModals();
      showToast('КОНТАКТ ДОБАВЛЕН // E2EE ЛС ОТКРЫТО');
    } else {
      closeModals();
      showToast('ЗАПРОС НА ПОДКЛЮЧЕНИЕ ОТПРАВЛЕН');
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
      showToast('ОЖИДАНИЕ ЛОКАЛЬНОГО ДЕМОНА...');
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
        nickname: peerOrPk.nickname || ('Peer-' + peerOrPk.public_key.substring(0, 6)),
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
      showToast('ПСЕВДОНИМ СОХРАНЕН');
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
      copyTextWithToast(myExportedSeed, 'MASTER SEED СКОПИРОВАН');
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
      showToast('ВВЕДИТЕ КОРРЕКТНЫЙ 64-HEX MASTER SEED');
      return;
    }
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ action: 'import_secret', seed_hex: seedHex }));
      if (input) input.value = '';
      showToast('MASTER SEED ИМПОРТИРОВАН');
    }
  }

  function submitImportSeed() {
    importMasterSeed();
  }

  function triggerLanBeacon() {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ action: 'lan_beacon' }));
      showToast('LAN BEACON ОТПРАВЛЕН (UDP :9009)');
    }
  }

  function triggerLanDiscovery() {
    triggerLanBeacon();
  }

  function triggerStunProbe() {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ action: 'stun_probe' }));
      showToast('STUN PROBE ЗАПУЩЕН...');
    }
  }

  function triggerPullOffline() {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ action: 'pull_offline' }));
      showToast('СИНХРОНИЗАЦИЯ QUAS...');
    }
  }

  /* ======================== FILE BLOBS, REACTIONS & VOICE ======================== */
  function handleFileUpload(e) {
    const file = e && e.target && e.target.files ? e.target.files[0] : null;
    if (!file) return;
    if (file.size > 2 * 1024 * 1024) {
      showToast('МАКС. РАЗМЕР ФАЙЛА: 2 МБ');
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
        showToast('ОТПРАВКА P2P BLOB...');
      }
    };
    reader.readAsDataURL(file);
    e.target.value = '';
  }

  function downloadBlobAttachment(hash, filename, mime) {
    if (blobCache[hash]) {
      const a = document.createElement('a');
      a.href = `data:${mime || 'application/octet-stream'};base64,${blobCache[hash]}`;
      a.download = filename || 'attachment.bin';
      a.click();
    } else if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ action: 'get_blob', hash: hash }));
      showToast('ЗАГРУЗКА БЛОБА ИЗ P2P...');
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

  function promptEditMessage(targetSig) {
    if (!targetSig) return;
    const el = document.getElementById('content-' + targetSig);
    const oldText = el ? el.textContent.replace(' (ред.)', '') : '';
    openInputModal('edit_message', targetSig, oldText);
  }

  function deleteMessage(targetSig) {
    if (!targetSig || !ws || ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify({
      action: 'delete_message',
      target_sig: targetSig
    }));
    showToast('ПАКЕТ УДАЛЕН');
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
    document.querySelectorAll('.modal-overlay').forEach((m) => m.classList.remove('open'));
    pendingModalAction = null;
    pendingTargetSig = null;
  }

  function openInviteModal() {
    renderIdentityPanel();
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
      if (titleEl) titleEl.textContent = 'НОВЫЙ СВАРМ // СООБЩЕСТВО';
      if (labelEl) labelEl.textContent = 'НАЗВАНИЕ СВАРМА';
      if (fieldEl) {
        fieldEl.placeholder = 'Например: Core Dev Swarm';
        fieldEl.value = '';
      }
    } else if (actionType === 'create_channel') {
      if (titleEl) titleEl.textContent = 'НОВЫЙ КАНАЛ В СВАРМЕ';
      if (labelEl) labelEl.textContent = 'ИМЯ КАНАЛА';
      if (fieldEl) {
        fieldEl.placeholder = 'Например: architecture';
        fieldEl.value = '';
      }
    } else if (actionType === 'edit_message') {
      if (titleEl) titleEl.textContent = 'РЕДАКТИРОВАТЬ ПАКЕТ';
      if (labelEl) labelEl.textContent = 'НОВЫЙ ТЕКСТ (ПОДПИСЬ ED25519)';
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
      showToast('СВАРМ СОЗДАН');
    } else if (pendingModalAction === 'create_channel') {
      ws.send(JSON.stringify({ action: 'create_channel', name: val }));
      showToast('КАНАЛ СОЗДАН');
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
        myNick = data.identity.nickname || 'User';
        myStatus = data.identity.status || 'Online';
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
        historyCache = data.history;
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
        if (vBadge) vBadge.textContent = voiceConnected ? 'АКТИВЕН' : 'ВОЙТИ';
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
      if (!isDup) {
        historyCache.push(data);
      }

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
            last_ts: data.timestamp_ms || Date.now(),
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
        appendMessageToDom(data);
      }
      return;
    }

    if (data.type === 'secret_exported') {
      myExportedSeed = data.seed_hex || '';
      setText(['seed-hex-box', 'exported-seed-box', 'master-seed-box'], myExportedSeed);
      copyTextWithToast(myExportedSeed, 'MASTER SEED (64 HEX) СКОПИРОВАН');
      return;
    }

    if (data.type === 'reaction_updated') {
      const el = document.getElementById('reacts-' + data.target_sig);
      if (el) el.textContent = data.summary || '';
      return;
    }

    if (data.type === 'message_edited') {
      const el = document.getElementById('content-' + data.target_sig);
      if (el) el.textContent = (data.content || '') + ' (ред.)';
      return;
    }

    if (data.type === 'message_deleted') {
      const el = document.getElementById('content-' + data.target_sig);
      if (el) {
        el.textContent = '[Пакет удален автором]';
        el.style.color = '#7d8594';
      }
      return;
    }

    if (data.type === 'voice_state') {
      voiceConnected = Boolean(data.connected);
      const vBar = getEl('voice-bar');
      const vBtn = getEl('voice-btn');
      const vBadge = getEl('voice-badge');
      if (vBar) vBar.classList.toggle('active', voiceConnected);
      if (vBtn) vBtn.classList.toggle('connected', voiceConnected);
      if (vBadge) vBadge.textContent = voiceConnected ? 'АКТИВЕН' : 'ВОЙТИ';
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
      setText(['my-node-status'], 'RECONNECTING...');
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
  window.promptEditMessage = promptEditMessage;
  window.deleteMessage = deleteMessage;
  window.toggleVoiceConnect = toggleVoiceConnect;
  window.toggleMobileDrawer = toggleMobileDrawer;
  window.renderKeyMatrixCanvas = renderKeyMatrixCanvas;

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initWexApp);
  } else {
    initWexApp();
  }
})();
