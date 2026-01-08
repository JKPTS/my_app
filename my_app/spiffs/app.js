// ===== FILE: spiffs/app.js =====
function $(id) { return document.getElementById(id); }
function must(id) {
  const el = $(id);
  if (!el) throw new Error(`missing element id="${id}"`);
  return el;
}

let META = { maxBanks: 100, buttons: 8, bankCount: 1, maxActions: 20, longMs: 400 };
let LAYOUT = { bankCount: 1, banks: [] };
let BANKDATA = { switchNames: [] };

let cur = { bank: 0, btn: 0 };
let MAP = null;

let LOADING = false;
let lastUserNavAt = 0;

// limits
const MAX_BANK_NAME = 10;
const MAX_SWITCH_NAME = 5;

// led brightness save timer
let tSaveLed = null;
let dirtyLed = false;

let dirtyBtn = false;
let dirtyLayout = false;
let dirtyBank = false;

// ✅ non-blocking save controller (prevents UI lag)
let btnVer = 0, btnSaving = false, btnSavePromise = Promise.resolve();
let layoutVer = 0, layoutSaving = false, layoutSavePromise = Promise.resolve();
let bankVer = 0, bankSaving = false, bankSavePromise = Promise.resolve();
let ledVer = 0, ledSaving = false, ledSavePromise = Promise.resolve();

function nowMs() { return Date.now(); }

function wrap(n, max) {
  max = Math.max(1, Number(max || 1));
  let r = n % max;
  if (r < 0) r += max;
  return r;
}

function clampInt(v, lo, hi) {
  v = Number(v);
  if (!Number.isFinite(v)) v = lo;
  v = Math.trunc(v);
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

function clipText(s, maxLen) {
  return String(s || "").slice(0, maxLen);
}

function setMsg(text, ok = true) {
  const el = must("msg");
  el.textContent = text || "";
  el.className = "msg " + (ok ? "ok" : "bad");
}

async function apiGet(url) {
  const r = await fetch(url, { cache: "no-store" });
  if (!r.ok) throw new Error(await r.text());
  return r.json();
}

async function apiPost(url, obj) {
  const r = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(obj),
  });
  if (!r.ok) throw new Error(await r.text());
  return r.json();
}

function curBankObj() { return (LAYOUT.banks || [])[cur.bank]; }

// ---------- "finish typing then save" helpers ----------
function isEditableField(el) {
  if (!el) return false;
  const tag = (el.tagName || "").toLowerCase();
  return tag === "input" || tag === "textarea" || tag === "select";
}

function forceCommitActiveField() {
  const el = document.activeElement;
  if (isEditableField(el)) el.blur();
}

function hookFinishedTypingInput(inp, onDirty, onFinish) {
  if (!inp) return;

  inp.addEventListener("input", () => onDirty?.());

  inp.addEventListener("keydown", (e) => {
    if (e.key === "Enter") {
      e.preventDefault();
      inp.blur();
    }
  });

  inp.addEventListener("change", () => onFinish?.());
  inp.addEventListener("blur", () => onFinish?.());
}

async function flushPendingSaves() {
  forceCommitActiveField();

  await Promise.all([layoutSavePromise, bankSavePromise, btnSavePromise, ledSavePromise]);

  if (dirtyLayout) await saveLayoutImmediate();
  if (dirtyBank) await saveBankImmediate();
  if (dirtyBtn) await saveButtonImmediate();

  if (tSaveLed) { clearTimeout(tSaveLed); tSaveLed = null; }
  if (dirtyLed) await saveLedImmediate();
}

// ---------- render ----------
function renderHeader() {
  const b = curBankObj();

  must("curBank").textContent = cur.bank;
  must("curBankName").textContent = (b && b.name) ? b.name : "bank";

  must("bankName").value = (b && b.name) ? b.name : "";

  const sn = (BANKDATA.switchNames && BANKDATA.switchNames[cur.btn]) ? BANKDATA.switchNames[cur.btn] : "";
  must("switchName").value = sn;

  // dropdown
  const sel = must("bankSelect");
  sel.value = String(cur.bank);
}

function highlightGrid() {
  const pads = Array.from(must("btnGrid").querySelectorAll(".pad"));
  pads.forEach((p) => {
    const i = Number(p.dataset.idx);
    p.classList.toggle("active", i === cur.btn);
  });
}

function escapeHtml(s) {
  s = String(s ?? "");
  return s.replace(/[&<>"']/g, (c) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
  }[c]));
}

function renderGridLabels() {
  const pads = Array.from(must("btnGrid").querySelectorAll(".pad"));
  pads.forEach((p) => {
    const i = Number(p.dataset.idx);
    const name = (BANKDATA.switchNames && BANKDATA.switchNames[i]) ? BANKDATA.switchNames[i] : `SW ${i + 1}`;
    p.innerHTML = `
      <div class="padNum">${i + 1}</div>
      <div class="padName">${escapeHtml(name)}</div>
    `;
  });
}

function makeGrid() {
  const g = must("btnGrid");
  g.innerHTML = "";

  const count = Number(META.buttons || 8);
  for (let i = 0; i < count; i++) {
    const b = document.createElement("button");
    b.className = "pad";
    b.type = "button";
    b.dataset.idx = String(i);
    b.onclick = async () => {
      try {
        await flushPendingSaves();

        lastUserNavAt = nowMs();
        cur.btn = i;
        highlightGrid();
        renderHeader();
        await loadButton();
      } catch (e) {
        setMsg("load switch failed: " + e.message, false);
      }
    };
    g.appendChild(b);
  }

  renderGridLabels();
  highlightGrid();
}

// ---------- action rows ----------
function setInputVisible(inp, visible) {
  inp.style.display = visible ? "" : "none";
}

// ✅ small label wrappers (inline styles to avoid touching style.css)
function mkField(labelText, controlEl) {
  const wrap = document.createElement("div");
  wrap.className = "fieldWrap";
  wrap.style.display = "flex";
  wrap.style.flexDirection = "column";
  wrap.style.gap = "6px";

  const lbl = document.createElement("div");
  lbl.className = "fieldLbl";
  lbl.textContent = labelText || "";
  lbl.style.fontSize = "12px";
  lbl.style.opacity = "0.8";
  lbl.style.userSelect = "none";
  lbl.style.lineHeight = "1";

  wrap.appendChild(lbl);
  wrap.appendChild(controlEl);

  wrap._lbl = lbl;
  wrap._ctl = controlEl;

  return wrap;
}

function mkActionRow(action, onRemove, onDirtyBtn, onFinishBtn, onImmediateSaveBtn) {
  const row = document.createElement("div");
  row.className = "action";

  const type = document.createElement("select");
  ["cc", "pc"].forEach((t) => {
    const o = document.createElement("option");
    o.value = t;
    o.textContent = t;
    type.appendChild(o);
  });
  type.value = action.type || "cc";

  const ch = document.createElement("input");
  ch.type = "number"; ch.min = 1; ch.max = 16;
  ch.value = (action.ch ?? 1);

  const a = document.createElement("input");
  a.type = "number"; a.min = 0; a.max = 127;
  a.value = (action.a ?? 0);

  const b = document.createElement("input");
  b.type = "number"; b.min = 0; b.max = 127;
  b.value = (action.b ?? 0);

  const c = document.createElement("input");
  c.type = "number"; c.min = 0; c.max = 0;
  c.value = "0";

  const rm = document.createElement("button");
  rm.className = "x";
  rm.textContent = "×";
  rm.type = "button";
  rm.onclick = async () => {
    try {
      onRemove(row);
      await onImmediateSaveBtn?.();
    } catch (e) {
      setMsg("save failed: " + e.message, false);
    }
  };

  const fType = mkField("action", type);
  const fCh   = mkField("ch", ch);
  const fA    = mkField("cc#", a);
  const fB    = mkField("value", b);

  function refresh() {
    setInputVisible(ch, true);
    setInputVisible(a, true);
    setInputVisible(b, true);
    setInputVisible(c, false);

    if (type.value === "cc") {
      ch.placeholder = "ch";
      a.placeholder = "cc#";
      b.placeholder = "value";
      a.min = 0; a.max = 127;
      b.min = 0; b.max = 127;

      fA._lbl.textContent = "cc#";
      fB._lbl.textContent = "value";
      fB.style.display = "";
    } else {
      ch.placeholder = "ch";
      a.placeholder = "program";
      b.placeholder = "";
      a.min = 0; a.max = 127;

      setInputVisible(b, false);
      fA._lbl.textContent = "program";
      fB.style.display = "none";
    }
  }

  function getClamped() {
    const t = type.value;
    let _ch = clampInt(ch.value || 1, 1, 16);
    let _a = clampInt(a.value || 0, 0, 127);
    let _b = clampInt(b.value || 0, 0, 127);

    if (t !== "cc") _b = 0;

    ch.value = String(_ch);
    a.value = String(_a);
    b.value = String(_b);
    c.value = "0";

    return { type: t, ch: _ch, a: _a, b: _b, c: 0 };
  }

  row._get = () => getClamped();

  type.onchange = async () => {
    try {
      refresh();
      await onImmediateSaveBtn?.();
    } catch (e) {
      setMsg("save failed: " + e.message, false);
    }
  };

  [ch, a, b].forEach((inp) => hookFinishedTypingInput(inp, onDirtyBtn, onFinishBtn));

  refresh();
  row.append(fType, fCh, fA, fB, c, rm);
  return row;
}

function renderActions(listEl, actions, onDirtyBtn, onFinishBtn, onImmediateSaveBtn) {
  listEl.innerHTML = "";
  (actions || []).forEach((act) => {
    const row = mkActionRow(act, (r) => r.remove(), onDirtyBtn, onFinishBtn, onImmediateSaveBtn);
    listEl.appendChild(row);
  });
}

function collectActions(listEl) {
  const rows = Array.from(listEl.querySelectorAll(".action"));
  return rows.map((r) => r._get());
}

function listCount(listEl) {
  return listEl.querySelectorAll(".action").length;
}

function maxActions() {
  return Number(META.maxActions || 20);
}

// ---- a+b led ui helpers ----
function setAbLedUI(sel01) {
  const a = must("abLedA");
  const b = must("abLedB");
  const v = clampInt(sel01 ?? 1, 0, 1);
  a.checked = (v === 0);
  b.checked = (v === 1);
}

function getAbLedUI() {
  return must("abLedB").checked ? 1 : 0;
}

function ensureAbLedDefaultIfNeeded() {
  const a = must("abLedA");
  const b = must("abLedB");
  if (!a.checked && !b.checked) b.checked = true;
}

function updateModeUI() {
  const pm = Number(must("pressMode").value || "0");
  const paneRight = must("paneRight");
  const leftTitle = must("leftTitle");
  const rightTitle = must("rightTitle");
  const addRight = must("addRight");
  const addLeft = must("addLeft");

  const abWrap = must("abLedWrap");

  if (pm === 0) {
    paneRight.style.display = "none";
    addRight.style.display = "none";
    leftTitle.textContent = "commands";
    addLeft.textContent = `+ add (max ${maxActions()})`;
    abWrap.style.display = "none";
  } else if (pm === 1) {
    paneRight.style.display = "";
    addRight.style.display = "";
    leftTitle.textContent = "short";
    rightTitle.textContent = `long (${META.longMs || 400}ms)`;
    addLeft.textContent = `+ add short (max ${maxActions()})`;
    addRight.textContent = `+ add long (max ${maxActions()})`;
    abWrap.style.display = "none";
  } else if (pm === 2) {
    paneRight.style.display = "";
    addRight.style.display = "";
    leftTitle.textContent = "a";
    rightTitle.textContent = "b";
    addLeft.textContent = `+ add a (max ${maxActions()})`;
    addRight.textContent = `+ add b (max ${maxActions()})`;

    abWrap.style.display = "block";
    ensureAbLedDefaultIfNeeded();
  } else {
    paneRight.style.display = "none";
    addRight.style.display = "none";
    leftTitle.textContent = "group (short)";
    addLeft.textContent = `+ add (max ${maxActions()})`;
    abWrap.style.display = "none";
  }
}

function applyUIFromMap(m) {
  must("pressMode").value = String(m.pressMode ?? 0);

  renderActions(
    must("shortList"),
    m.short || [],
    markButtonDirty,
    requestSaveButtonAfterFinish,
    saveButtonImmediate
  );
  renderActions(
    must("longList"),
    m.long || [],
    markButtonDirty,
    requestSaveButtonAfterFinish,
    saveButtonImmediate
  );

  setAbLedUI(m.abLed ?? 1);
  updateModeUI();
}

function readUIToMap() {
  const pm = Number(must("pressMode").value || "0");
  let shortArr = collectActions(must("shortList"));
  let longArr = (pm === 0 || pm === 3) ? [] : collectActions(must("longList"));

  return {
    pressMode: pm,
    ccBehavior: 0,
    abLed: (pm === 2) ? getAbLedUI() : 1,
    short: shortArr,
    long: longArr,
  };
}

// ---------- non-blocking autosave ----------
function markButtonDirty() {
  if (LOADING) return;
  dirtyBtn = true;
  btnVer++;
  setMsg("editing… ✍️");
}

function markLayoutDirty() {
  if (LOADING) return;
  dirtyLayout = true;
  layoutVer++;
  setMsg("editing… ✍️");
}

function markBankDirty() {
  if (LOADING) return;
  dirtyBank = true;
  bankVer++;
  setMsg("editing… ✍️");
}

function requestSaveButtonAfterFinish() {
  if (LOADING) return;
  if (!dirtyBtn) return;

  if (btnSaving) return;
  btnSaving = true;

  btnSavePromise = (async () => {
    while (true) {
      const v = btnVer;
      try {
        await saveButton();
        if (btnVer === v) {
          dirtyBtn = false;
          setMsg("saved ✅");
          break;
        }
      } catch (e) {
        setMsg("save failed: " + e.message, false);
        break;
      }
      await new Promise((r) => setTimeout(r, 0));
    }
    btnSaving = false;
  })();
}

function requestSaveLayoutAfterFinish() {
  if (LOADING) return;
  if (!dirtyLayout) return;

  if (layoutSaving) return;
  layoutSaving = true;

  layoutSavePromise = (async () => {
    while (true) {
      const v = layoutVer;
      try {
        await saveLayout();
        if (layoutVer === v) {
          dirtyLayout = false;
          setMsg("saved ✅");
          break;
        }
      } catch (e) {
        setMsg("save failed: " + e.message, false);
        break;
      }
      await new Promise((r) => setTimeout(r, 0));
    }
    layoutSaving = false;
  })();
}

function requestSaveBankAfterFinish() {
  if (LOADING) return;
  if (!dirtyBank) return;

  if (bankSaving) return;
  bankSaving = true;

  bankSavePromise = (async () => {
    while (true) {
      const v = bankVer;
      try {
        await saveBank();
        if (bankVer === v) {
          dirtyBank = false;
          setMsg("saved ✅");
          break;
        }
      } catch (e) {
        setMsg("save failed: " + e.message, false);
        break;
      }
      await new Promise((r) => setTimeout(r, 0));
    }
    bankSaving = false;
  })();
}

async function saveButtonImmediate() {
  if (LOADING) return;
  dirtyBtn = true;
  btnVer++;
  requestSaveButtonAfterFinish();
  return btnSavePromise;
}

async function saveLayoutImmediate() {
  if (LOADING) return;
  dirtyLayout = true;
  layoutVer++;
  requestSaveLayoutAfterFinish();
  await layoutSavePromise;
  renderHeader();
  refreshLayoutButtons();
  refreshBankDropdown();
}

async function saveBankImmediate() {
  if (LOADING) return;
  dirtyBank = true;
  bankVer++;
  requestSaveBankAfterFinish();
  await bankSavePromise;
  renderGridLabels();
  renderHeader();
}

// ---------- led brightness ----------
async function loadLedBrightness() {
  const r = await apiGet("/api/led");
  let v = clampInt(r?.brightness ?? 100, 0, 100);
  must("ledBrightness").value = String(v);
  must("ledBrightnessVal").textContent = String(v);
}

async function saveLedBrightness() {
  const v = clampInt(must("ledBrightness").value ?? 100, 0, 100);
  await apiPost("/api/led", { brightness: v });
}

function markLedDirty() {
  if (LOADING) return;
  dirtyLed = true;
  ledVer++;
}

function requestSaveLedAfterFinish() {
  if (LOADING) return;
  if (!dirtyLed) return;

  if (tSaveLed) { clearTimeout(tSaveLed); tSaveLed = null; }
  tSaveLed = setTimeout(() => {
    if (ledSaving) return;
    ledSaving = true;

    ledSavePromise = (async () => {
      while (true) {
        const v = ledVer;
        try {
          await saveLedBrightness();
          if (ledVer === v) {
            dirtyLed = false;
            setMsg("saved ✅");
            break;
          }
        } catch (e) {
          setMsg("save led failed: " + e.message, false);
          break;
        }
        await new Promise((r) => setTimeout(r, 0));
      }
      ledSaving = false;
    })();
  }, 250);
}

async function saveLedImmediate() {
  if (LOADING) return;
  dirtyLed = true;
  ledVer++;
  requestSaveLedAfterFinish();
  return ledSavePromise;
}

// ---------- load/save ----------
async function loadMeta() {
  META = await apiGet("/api/meta");
}

function refreshLayoutButtons() {
  const bc = Number(LAYOUT.bankCount || 1);

  must("btnAddBank").disabled = bc >= Number(META.maxBanks || 100);
  must("btnDelBank").disabled = bc <= 1;
}

function refreshBankDropdown() {
  const sel = must("bankSelect");
  sel.innerHTML = "";

  const bc = Number(LAYOUT.bankCount || 1);
  for (let i = 0; i < bc; i++) {
    const b = (LAYOUT.banks || [])[i] || { name: `Bank ${i + 1}` };
    const opt = document.createElement("option");
    opt.value = String(i);
    opt.textContent = `#${i} · ${b.name || "bank"}`;
    sel.appendChild(opt);
  }

  sel.value = String(cur.bank);
}

async function loadLayout() {
  LAYOUT = await apiGet("/api/layout");

  const bc = Number(LAYOUT.bankCount || 1);
  LAYOUT.bankCount = bc;

  const rawBanks = (LAYOUT.banks || []).slice(0, bc);
  LAYOUT.banks = rawBanks.map((b, idx) => {
    return {
      index: idx,
      name: clipText((b && b.name) ? b.name : ("Bank " + (idx + 1)), MAX_BANK_NAME),
    };
  });

  while (LAYOUT.banks.length < bc) {
    const idx = LAYOUT.banks.length;
    LAYOUT.banks.push({
      index: idx,
      name: clipText("Bank " + (idx + 1), MAX_BANK_NAME),
    });
  }

  cur.bank = wrap(cur.bank, LAYOUT.bankCount);
  refreshLayoutButtons();
  refreshBankDropdown();
}

async function saveLayout() {
  const payload = {
    bankCount: LAYOUT.bankCount,
    banks: LAYOUT.banks.map((b, idx) => ({
      index: idx,
      name: clipText(b.name, MAX_BANK_NAME),
    })),
  };
  await apiPost("/api/layout", payload);
}

async function loadBank() {
  const url = `/api/bank?bank=${cur.bank}`;
  BANKDATA = await apiGet(url);

  if (!BANKDATA || typeof BANKDATA !== "object") BANKDATA = { switchNames: [] };
  if (!Array.isArray(BANKDATA.switchNames)) BANKDATA.switchNames = [];

  BANKDATA.switchNames = BANKDATA.switchNames
    .slice(0, Number(META.buttons || 8))
    .map((s, i) => {
      const v = clipText(s, MAX_SWITCH_NAME);
      return v || `SW${i + 1}`;
    });

  while (BANKDATA.switchNames.length < Number(META.buttons || 8)) {
    BANKDATA.switchNames.push(`SW${BANKDATA.switchNames.length + 1}`);
  }

  renderGridLabels();
}

async function saveBank() {
  const payload = {
    switchNames: (BANKDATA.switchNames || [])
      .slice(0, Number(META.buttons || 8))
      .map((s) => clipText(s, MAX_SWITCH_NAME)),
  };
  await apiPost(`/api/bank?bank=${cur.bank}`, payload);
}

async function loadButton() {
  LOADING = true;
  try {
    renderHeader();
    refreshLayoutButtons();

    await loadBank();

    const url = `/api/button?bank=${cur.bank}&btn=${cur.btn}`;
    MAP = await apiGet(url);

    if (!MAP || typeof MAP !== "object") MAP = { pressMode: 0, short: [], long: [], abLed: 1 };
    if (!Array.isArray(MAP.short)) MAP.short = [];
    if (!Array.isArray(MAP.long)) MAP.long = [];

    applyUIFromMap(MAP);
    highlightGrid();
    renderHeader();

    dirtyBtn = false; dirtyLayout = false; dirtyBank = false; dirtyLed = false;
  } finally {
    LOADING = false;
  }
}

async function saveButton() {
  const url = `/api/button?bank=${cur.bank}&btn=${cur.btn}`;
  const payload = readUIToMap();
  await apiPost(url, payload);
}

// ---------- sync bank web <-> hw ----------
async function setHardwareState() {
  await apiPost("/api/state", { bank: cur.bank });
}

async function gotoBank(bank) {
  await flushPendingSaves();

  lastUserNavAt = nowMs();
  cur.bank = wrap(bank, LAYOUT.bankCount);
  cur.btn = wrap(cur.btn, Number(META.buttons || 8));

  await setHardwareState();
  await loadButton();
}

// ---------- add/remove helpers (insert after current) ----------
function reindexBanks() {
  (LAYOUT.banks || []).forEach((b, idx) => { b.index = idx; });
}

function insertBankAfterCurrent() {
  const pos = Math.min(LAYOUT.bankCount, cur.bank + 1);
  const newBank = {
    index: pos,
    name: clipText(`Bank ${pos + 1}`, MAX_BANK_NAME),
  };
  LAYOUT.banks.splice(pos, 0, newBank);
  LAYOUT.bankCount += 1;
  reindexBanks();
  return pos;
}

function deleteCurrentBank() {
  if (LAYOUT.bankCount <= 1) throw new Error("need at least 1 bank");
  LAYOUT.banks.splice(cur.bank, 1);
  LAYOUT.bankCount -= 1;
  reindexBanks();
  cur.bank = Math.min(cur.bank, LAYOUT.bankCount - 1);
  cur.btn = wrap(cur.btn, Number(META.buttons || 8));
}

function tryAddRow(listEl) {
  if (listCount(listEl) >= maxActions()) {
    setMsg(`max actions reached (${maxActions()})`, false);
    return;
  }

  const row = mkActionRow(
    { type: "cc", ch: 1, a: 0, b: 127, c: 0 },
    (r) => r.remove(),
    markButtonDirty,
    requestSaveButtonAfterFinish,
    saveButtonImmediate
  );
  listEl.appendChild(row);

  saveButtonImmediate().catch((e) => setMsg("save failed: " + e.message, false));
}

// ---------- live poll (hardware -> web sync) ----------
async function pollLive() {
  try {
    const st = await apiGet("/api/state");
    must("liveBank").textContent = st.bank;

    const b = wrap(st.bank, LAYOUT.bankCount);

    if ((nowMs() - lastUserNavAt) > 800) {
      if (b !== cur.bank) {
        await flushPendingSaves();
        cur.bank = b;
        cur.btn = wrap(cur.btn, Number(META.buttons || 8));
        await loadButton();
        setMsg("synced ✅");
      }
    }
  } catch (_) {}
  setTimeout(pollLive, 450);
}

// ---------- UI wiring ----------
function setupUI() {
  must("bankMinus").onclick = async () => { await gotoBank(cur.bank - 1); };
  must("bankPlus").onclick  = async () => { await gotoBank(cur.bank + 1); };

  // dropdown
  must("bankSelect").onchange = async (e) => {
    const v = clampInt(e.target.value, 0, Math.max(0, (LAYOUT.bankCount || 1) - 1));
    await gotoBank(v);
  };

  must("btnAddBank").onclick = async () => {
    try {
      await flushPendingSaves();
      if (LAYOUT.bankCount >= (META.maxBanks || 100)) throw new Error("max banks reached");

      const newIdx = insertBankAfterCurrent();
      await saveLayoutImmediate();
      await gotoBank(newIdx);
      setMsg("added bank ✅");
    } catch (e) {
      setMsg("add bank failed: " + e.message, false);
    }
  };

  must("btnDelBank").onclick = async () => {
    try {
      await flushPendingSaves();
      const ok = confirm(`delete current bank? (#${cur.bank})`);
      if (!ok) return;

      deleteCurrentBank();
      await saveLayoutImmediate();
      await gotoBank(cur.bank);
      setMsg("deleted bank ✅");
    } catch (e) {
      setMsg("delete bank failed: " + e.message, false);
    }
  };

  const bankName = must("bankName");
  const switchName = must("switchName");

  bankName.oninput = () => {
    const b = curBankObj();
    if (!b) return;
    b.name = clipText(bankName.value, MAX_BANK_NAME);
    bankName.value = b.name;
    must("curBankName").textContent = b.name || "bank";
    markLayoutDirty();
    refreshBankDropdown();
  };
  hookFinishedTypingInput(bankName, () => {}, requestSaveLayoutAfterFinish);

  switchName.oninput = () => {
    const v = clipText(switchName.value, MAX_SWITCH_NAME);
    switchName.value = v;
    if (!Array.isArray(BANKDATA.switchNames)) BANKDATA.switchNames = [];
    BANKDATA.switchNames[cur.btn] = v || `SW${cur.btn + 1}`;
    renderGridLabels();
    markBankDirty();
  };
  hookFinishedTypingInput(switchName, () => {}, requestSaveBankAfterFinish);

  // led brightness
  const led = must("ledBrightness");
  led.addEventListener("input", () => {
    const v = clampInt(led.value, 0, 100);
    led.value = String(v);
    must("ledBrightnessVal").textContent = String(v);
    markLedDirty();
    requestSaveLedAfterFinish();
  });
  led.addEventListener("change", () => {
    markLedDirty();
    requestSaveLedAfterFinish();
  });

  // a+b led radio (exclusive)
  const abA = must("abLedA");
  const abB = must("abLedB");
  function onAbChange() {
    if (LOADING) return;
    if (Number(must("pressMode").value || "0") !== 2) return;
    saveButtonImmediate().catch((e) => setMsg("save failed: " + e.message, false));
  }
  abA.addEventListener("change", onAbChange);
  abB.addEventListener("change", onAbChange);

  must("addLeft").onclick = () => tryAddRow(must("shortList"));
  must("addRight").onclick = () => tryAddRow(must("longList"));

  must("btnReload").onclick = async () => {
    try {
      await flushPendingSaves();
      await loadButton();
      setMsg("reloaded ✅");
    } catch (e) {
      setMsg("reload failed: " + e.message, false);
    }
  };

  must("pressMode").onchange = async () => {
    try {
      updateModeUI();
      await saveButtonImmediate();
    } catch (e) {
      setMsg("save failed: " + e.message, false);
    }
  };
}

window.addEventListener("load", async () => {
  try {
    setMsg("init… ⚙️");
    await loadMeta();
    await loadLayout();
    await loadLedBrightness();

    setupUI();
    makeGrid();

    try {
      const st = await apiGet("/api/state");
      cur.bank = wrap(st.bank, LAYOUT.bankCount);
    } catch (_) {}

    await gotoBank(cur.bank);

    pollLive();
    setMsg("ready ✅");
  } catch (e) {
    try { setMsg("init failed: " + e.message, false); }
    catch (_) { alert("init failed: " + e.message); }
  }
});
