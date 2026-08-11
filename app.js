'use strict';

// ── 兼容性 polyfill：老浏览器（Safari <10 等）不支持 NodeList.forEach ──
if (window.NodeList && !NodeList.prototype.forEach) {
    NodeList.prototype.forEach = function (cb, thisArg) {
        for (let i = 0; i < this.length; i++) cb.call(thisArg, this[i], i, this);
    };
}
// ── 兼容性 polyfill：Element.closest（Safari <9 / IE）──
if (window.Element && !Element.prototype.closest) {
    Element.prototype.closest = function (sel) {
        let el = this;
        while (el && el.nodeType === 1) {
            if (el.matches(sel)) return el;
            el = el.parentElement || el.parentNode;
        }
        return null;
    };
}
// ── 兼容性 polyfill：Element.matches ──
if (window.Element && !Element.prototype.matches) {
    Element.prototype.matches = Element.prototype.msMatchesSelector ||
        Element.prototype.webkitMatchesSelector ||
        function (sel) { return [].indexOf.call(document.querySelectorAll(sel), this) !== -1; };
}

// ── 兼容性：安全 localStorage 访问 ──
// ← 修复：Safari 隐私模式 / 部分系统禁用存储时，直接访问 localStorage 会抛
//   SecurityError 导致整个脚本崩溃。以下包装统一 try/catch 静默降级。
function safeGet(key) {
    try { return window.localStorage.getItem(key); } catch (e) { return null; }
}
function safeSet(key, value) {
    try { window.localStorage.setItem(key, value); } catch (e) { /* 隐私模式等静默失败 */ }
}
function safeRemove(key) {
    try { window.localStorage.removeItem(key); } catch (e) { /* 静默失败 */ }
}

// ── 全局状态 ──
const state = {
    connected: false,
    messages: [],
    members: [],
    users: {},           // { user_id: nickname }
    groups: {},
    groupNameCache: {},  // { group_code: group_name } 群名缓存
    groupOwnerUserId: '',
    currentGroup: '',
    membersGroup: '',    // 当前 state.members 所属的群号（用于切换群后不误显示旧群成员）
    botId: '',           // ← 修复：从 status API 读取
    forwardMode: false,
    msgLogEnabled: true,
    recallMonitorEnabled: false,
    theme: safeGet('theme') || 'light',
    selectedSticker: null,
    statusPollingStarted: false,  // ← 修复：防止重复轮询
    backgroundMode: safeGet('bgMode') || 'colorful',  // colorful | glass | custom
    customBg: safeGet('customBg') || '',            // 自定义背景 Data URL
    bgDim: safeGet('bgDim') === '1',
    theme_qq: safeGet('themeQQ') || 'default',
    customTheme: JSON.parse(safeGet('customTheme') || 'null'),
    memberBadges: JSON.parse(safeGet('memberBadges') || '{}'),  // {user_id: {text, type, color, auth, avatar}}
    memberAuth: JSON.parse(safeGet('memberAuth') || '{}'),  // {user_id: true} 认证蓝标
    memberAvatars: JSON.parse(safeGet('memberAvatars') || '{}'),  // {user_id: dataURL} 自定义头像
};

// ── 工具函数 ──
function $(id) { return document.getElementById(id); }
function showToast(msg, type = '', duration = 3000) {
    const container = $('toastContainer') || createToastContainer();
    const toast = document.createElement('div');
    toast.className = `toast ${type}`;
    toast.textContent = msg;
    container.appendChild(toast);
    setTimeout(() => { toast.style.opacity = '0'; setTimeout(() => toast.remove(), 300); }, duration);
}
function createToastContainer() {
    const c = document.createElement('div');
    c.className = 'toast-container';
    c.id = 'toastContainer';
    document.body.appendChild(c);
    return c;
}

// ── API 封装 ──
// 统一：用 r.ok 判断成功，避免字段名错误
async function api(path, opts = {}) {
    try {
        // ← 修复：POST 请求统一注入当前查看的群号（发送到当前派而非默认群），
        //   显式传了 group_code 的请求（如 groups/switch、groups/listen）不受影响
        const body = opts.body ? { ...opts.body } : undefined;
        if (body && typeof body === 'object' && !('group_code' in body) && state.currentGroup) {
            body.group_code = state.currentGroup;
        }
        // ← 修复：请求超时保护（15s）。后端异常/无响应时 fetch 不会自行结束，
        //   发送按钮/消息列表会一直"转圈"，表现为界面卡死。超时后返回失败并提示。
        const ctrl = new AbortController();
        const timer = setTimeout(() => ctrl.abort(), (opts.timeout || 15) * 1000);
        try {
            const res = await fetch(path, {
                headers: { 'Content-Type': 'application/json' },
                ...opts,
                signal: ctrl.signal,
                body: body ? JSON.stringify(body) : undefined,
            });
            clearTimeout(timer);
            const j = await res.json();
            return j;
        } catch (e) {
            clearTimeout(timer);
            console.error(`[API] ${path} 请求失败:`, e);
            return { ok: false, message: e.name === 'AbortError' ? '请求超时，请检查服务器连接' : e.message };
        }
    } catch (e) {
        console.error(`[API] ${path} 调用异常:`, e);
        return { ok: false, message: e.message };
    }
}

// ── 主题 ──
function toggleTheme() {
    // V4.2 修复：使用 setDarkMode 保持 UI 同步
    setDarkMode(state.theme !== 'dark');
}
document.documentElement.setAttribute('data-theme', state.theme);
if (state.theme === 'dark') $('themeToggle').textContent = '🌙';

// ── 黑夜模式（V4.2 修复：与厂商效果完全兼容） ──
function setDarkMode(enabled) {
    state.theme = enabled ? 'dark' : 'light';
    document.documentElement.setAttribute('data-theme', state.theme);
    safeSet('theme', state.theme);
    const cb = $('settingDarkMode');
    if (cb) cb.checked = enabled;
    const tg = $('themeToggle');
    if (tg) tg.textContent = enabled ? '🌙' : '☀️';
    flashThemeSwitch();
    if (state.theme_qq === 'qq-custom-gradient') applyCustomGradientVars();
}

// ── 自定义背景（v4.0，纯前端 localStorage 方案）──
function applyBackground() {
    const html = document.documentElement;
    html.classList.remove('bg-mode-colorful', 'bg-mode-glass', 'bg-mode-custom');
    html.classList.add('bg-mode-' + state.backgroundMode);
    if (state.backgroundMode === 'custom' && state.customBg) {
        html.style.setProperty('--custom-bg', `url("${state.customBg}")`);
    } else {
        html.style.removeProperty('--custom-bg');
    }
    if (state.bgDim) html.classList.add('bg-dim'); else html.classList.remove('bg-dim');
    // 同步 UI 选中态
    document.querySelectorAll('#bgModeChips .chip').forEach(c => {
        c.classList.toggle('active', c.dataset.bg === state.backgroundMode);
    });
    const wrap = $('bgCustomWrap');
    if (wrap) wrap.style.display = state.backgroundMode === 'custom' ? 'block' : 'none';
    const dimToggle = $('bgDimToggle');
    if (dimToggle) dimToggle.checked = state.bgDim;
    const prev = $('bgPreviewImg');
    if (prev) prev.src = state.customBg || '';
}

function setBackgroundMode(mode) {
    state.backgroundMode = mode;
    safeSet('bgMode', mode);
    if (mode !== 'custom') {
        // 切换走自定义时保留已存图片，方便切回
        state.customBg = safeGet('customBg') || '';
    }
    applyBackground();
}

function toggleBgDim(enabled) {
    state.bgDim = enabled;
    safeSet('bgDim', enabled ? '1' : '0');
    applyBackground();
}

function clearCustomBg() {
    state.customBg = '';
    safeRemove('customBg');
    applyBackground();
    showToast('已清除自定义背景', 'success');
}

// 将图片等比缩放并转码为浏览器兼容格式，控制体积以适配 localStorage
function resizeImageToDataURL(file, cb) {
    const reader = new FileReader();
    reader.onerror = () => cb(null);
    reader.onload = () => {
        const img = new Image();
        img.onerror = () => cb(null);
        img.onload = () => {
            let { width, height } = img;
            const max = 1600;
            if (width > max || height > max) {
                const r = Math.min(max / width, max / height);
                width = Math.round(width * r);
                height = Math.round(height * r);
            }
            const canvas = document.createElement('canvas');
            canvas.width = width; canvas.height = height;
            const ctx = canvas.getContext('2d');
            ctx.drawImage(img, 0, 0, width, height);
            let url = canvas.toDataURL('image/jpeg', 0.85);
            // 体积仍过大则进一步压缩
            if (url.length > 1500000) url = canvas.toDataURL('image/jpeg', 0.7);
            if (url.length > 2200000) {
                cb(null);
                showToast('图片过大，请选择更小的图片', 'error');
                return;
            }
            cb(url);
        };
        img.src = reader.result;
    };
    reader.readAsDataURL(file);
}

function handleCustomBgSelect(e) {
    const file = e.target.files[0];
    if (!file) return;
    resizeImageToDataURL(file, (dataUrl) => {
        if (!dataUrl) return;
        state.customBg = dataUrl;
        safeSet('customBg', dataUrl);
        state.backgroundMode = 'custom';
        safeSet('bgMode', 'custom');
        applyBackground();
        showToast('自定义背景已应用', 'success');
    });
}

// ── QQ 桌面端调色盘主题（v5.0） + V4.2 至尊主题 ──
const QQ_THEMES = [
    { id: 'default', name: '默认蓝', swatch: 'linear-gradient(135deg,#1E90FF,#3FA3FF)' },
    { id: 'qq-blue', name: '经典蓝', swatch: 'linear-gradient(135deg,#1E90FF,#3FA3FF)' },
    { id: 'qq-green', name: '清新绿', swatch: 'linear-gradient(135deg,#20C9A6,#35DBB8)' },
    { id: 'qq-red', name: '热情红', swatch: 'linear-gradient(135deg,#E64547,#F55F60)' },
    { id: 'qq-purple', name: '神秘紫', swatch: 'linear-gradient(135deg,#6C5CE7,#8377F0)' },
    { id: 'qq-pink', name: '樱花粉', swatch: 'linear-gradient(135deg,#F368E0,#FF83EA)' },
    { id: 'qq-orange', name: '活力橙', swatch: 'linear-gradient(135deg,#FF9F43,#FFB264)' },
    { id: 'qq-cyan', name: '薄荷青', swatch: 'linear-gradient(135deg,#0E9E79,#14BC90)' },
    { id: 'qq-yellow', name: '柠檬黄', swatch: 'linear-gradient(135deg,#FEC94F,#FFD96B)' },
    { id: 'qq-sakura', name: '浪漫樱', swatch: 'linear-gradient(135deg,#FF8FAB,#FFA5BC)' },
    { id: 'qq-ocean', name: '深邃海', swatch: 'linear-gradient(135deg,#12557F,#1E6E9E)' },
    { id: 'qq-dark', name: '暗夜黑', swatch: 'linear-gradient(135deg,#0F1419,#5AC8FA)' },
    { id: 'qq-supreme-gold', name: '👑 至尊黄金', swatch: 'linear-gradient(135deg,#FFD700,#D4AF37,#B8860B)' },
    { id: 'qq-supreme-black-gold', name: '👑 至尊黑金', swatch: 'linear-gradient(135deg,#0A0805,#D4AF37)' },
    { id: 'qq-harmony-spatial', name: '🌌 鸿蒙空间光感', swatch: 'linear-gradient(135deg,#2B9DFA,#7DD8FF,#FFFFFF)' },
    { id: 'qq-enterprise', name: '🏢 企业简洁', swatch: 'linear-gradient(135deg,#2563EB,#0F172A)' },
    { id: 'qq-custom-gradient', name: '🎨 自定义渐变', swatch: 'conic-gradient(from 0deg,#FF0080,#7928CA,#0070F3,#00DFD8,#FF0080)' },
];
const CUSTOM_THEME_DEFAULTS = { from: '#0066CC', to: '#00E5FF', bgFrom: '#F0F8FF', bgTo: '#FFFFFF', text: '#1d1d1f' };

function renderThemeGrid() {
    const grid = $('themeGrid');
    if (!grid) return;
    const chips = QQ_THEMES.map(t => `
        <button class="theme-chip ${state.theme_qq === t.id ? 'active' : ''}" data-theme-id="${t.id}" onclick="setQQTheme('${t.id}')">
            <span class="theme-swatch" style="background:${t.swatch}"></span>
            <span>${t.name}</span>
        </button>
    `).join('');
    const customPanel = `
        <div id="customGradientPanel" style="display:${state.theme_qq === 'qq-custom-gradient' ? 'block' : 'none'};margin-top:10px;padding:12px;background:var(--input-bg);border-radius:12px;border:1px solid var(--border-soft)">
            <div style="font-size:12px;color:var(--text-secondary);margin-bottom:8px">🎨 自定义渐变主题（V4.2）— 选任意两色做为主色，支持任意颜色组合</div>
            <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:8px">
                <label style="display:flex;align-items:center;gap:6px;font-size:12px"><span>主色 A</span><input type="color" id="customFrom" value="${(state.customTheme || {}).from || CUSTOM_THEME_DEFAULTS.from}" onchange="onCustomThemeChange()" style="width:36px;height:28px;border:1px solid var(--border-soft);border-radius:4px;background:none"></label>
                <label style="display:flex;align-items:center;gap:6px;font-size:12px"><span>主色 B</span><input type="color" id="customTo" value="${(state.customTheme || {}).to || CUSTOM_THEME_DEFAULTS.to}" onchange="onCustomThemeChange()" style="width:36px;height:28px;border:1px solid var(--border-soft);border-radius:4px;background:none"></label>
                <label style="display:flex;align-items:center;gap:6px;font-size:12px"><span>背景 A</span><input type="color" id="customBgFrom" value="${(state.customTheme || {}).bgFrom || CUSTOM_THEME_DEFAULTS.bgFrom}" onchange="onCustomThemeChange()" style="width:36px;height:28px;border:1px solid var(--border-soft);border-radius:4px;background:none"></label>
                <label style="display:flex;align-items:center;gap:6px;font-size:12px"><span>背景 B</span><input type="color" id="customBgTo" value="${(state.customTheme || {}).bgTo || CUSTOM_THEME_DEFAULTS.bgTo}" onchange="onCustomThemeChange()" style="width:36px;height:28px;border:1px solid var(--border-soft);border-radius:4px;background:none"></label>
            </div>
            <div style="display:flex;gap:6px;flex-wrap:wrap" id="customPresets">
                <button class="theme-chip" onclick="applyCustomPreset('#FF0080','#7928CA','#FFF0F8','#FFFFFF')" style="font-size:11px">🌸 粉紫</button>
                <button class="theme-chip" onclick="applyCustomPreset('#00DFD8','#0070F3','#E0FAFF','#FFFFFF')" style="font-size:11px">🌊 青蓝</button>
                <button class="theme-chip" onclick="applyCustomPreset('#FF6B6B','#FFA500','#FFF5F0','#FFFFFF')" style="font-size:11px">🍊 落日</button>
                <button class="theme-chip" onclick="applyCustomPreset('#10B981','#059669','#ECFDF5','#FFFFFF')" style="font-size:11px">🌿 森林</button>
                <button class="theme-chip" onclick="applyCustomPreset('#8B5CF6','#EC4899','#FAF5FF','#FFFFFF')" style="font-size:11px">🔮 极光</button>
                <button class="theme-chip" onclick="applyCustomPreset('#0F172A','#475569','#F8FAFC','#E2E8F0')" style="font-size:11px">🌑 商务</button>
            </div>
            <div id="customGradientPreview" style="margin-top:10px;height:30px;border-radius:6px;border:1px solid var(--border-soft)"></div>
        </div>
    `;
    grid.innerHTML = chips + customPanel;
    updateCustomGradientPreview();
}
function applyCustomPreset(a, b, bgA, bgB) {
    state.customTheme = { from: a, to: b, bgFrom: bgA, bgTo: bgB };
    safeSet('customTheme', JSON.stringify(state.customTheme));
    const set = (id, v) => { const el = $(id); if (el) el.value = v; };
    set('customFrom', a); set('customTo', b); set('customBgFrom', bgA); set('customBgTo', bgB);
    if (state.theme_qq !== 'qq-custom-gradient') setQQTheme('qq-custom-gradient');
    else { applyCustomGradientVars(); updateCustomGradientPreview(); showToast('已应用预设渐变', 'success'); }
}
function onCustomThemeChange() {
    state.customTheme = {
        from: $('customFrom').value,
        to: $('customTo').value,
        bgFrom: $('customBgFrom').value,
        bgTo: $('customBgTo').value,
    };
    safeSet('customTheme', JSON.stringify(state.customTheme));
    applyCustomGradientVars();
    updateCustomGradientPreview();
}
function applyCustomGradientVars() {
    const c = state.customTheme || CUSTOM_THEME_DEFAULTS;
    const root = document.documentElement.style;
    const isDark = document.documentElement.getAttribute('data-theme') === 'dark';
    root.setProperty('--custom-from', c.from);
    root.setProperty('--custom-to', c.to);
    root.setProperty('--custom-bg-from', c.bgFrom);
    root.setProperty('--custom-bg-to', c.bgTo);
    // 自动计算文字色（跟随背景明暗）
    const lum = hexLuminance(c.bgTo);
    root.setProperty('--custom-text', lum < 0.5 ? '#FFFFFF' : '#1d1d1f');
    root.setProperty('--custom-text-secondary', lum < 0.5 ? '#CCCCCC' : '#5a5a5a');
    root.setProperty('--custom-text-muted', lum < 0.5 ? '#888888' : '#aaaaaa');
    root.setProperty('--custom-msg-self', lum < 0.5 ? '#1F2A3A' : '#E8F0FE');
    // 主色语义变量：按下档 / 主色之上的文字色 / 描边 / 轨道 / 输入底 / 暗色底
    const pLum = hexLuminance(c.from);
    root.setProperty('--custom-deep', shade(c.from, -0.30));
    root.setProperty('--custom-on-primary', pLum < 0.5 ? '#FFFFFF' : '#1d1d1f');
    root.setProperty('--custom-border', shade(c.from, 0.62));
    root.setProperty('--custom-border-soft', shade(c.from, 0.80));
    root.setProperty('--custom-track', shade(c.bgTo, isDark ? 0.14 : -0.05));
    root.setProperty('--custom-input-bg', shade(c.bgTo, isDark ? 0.04 : -0.02));
    root.setProperty('--custom-dark-bg', shade(c.from, -0.82));
}
/* 将十六进制颜色向黑(amt<0)/白(amt>0)混合 amt∈[-1,1] */
function shade(hex, amt) {
    const m = hex.replace('#', '').match(/.{1,2}/g);
    if (!m || m.length < 3) return hex;
    const [r, g, b] = m.slice(0, 3).map(x => parseInt(x, 16));
    const t = amt < 0 ? 0 : 255, p = Math.abs(amt);
    const f = v => Math.round((t - v) * p + v);
    return '#' + [f(r), f(g), f(b)].map(v => v.toString(16).padStart(2, '0')).join('');
}
function flashThemeSwitch() {
    // 主题切换闪烁特效已移除（保留空函数以兼容调用点）
}
function hexLuminance(hex) {
    const m = hex.replace('#','').match(/.{1,2}/g);
    if (!m || m.length < 3) return 1;
    const [r, g, b] = m.slice(0,3).map(x => parseInt(x, 16) / 255);
    const f = c => c <= 0.03928 ? c/12.92 : Math.pow((c+0.055)/1.055, 2.4);
    return 0.2126*f(r) + 0.7152*f(g) + 0.0722*f(b);
}
function updateCustomGradientPreview() {
    const el = $('customGradientPreview');
    if (!el) return;
    const c = state.customTheme || CUSTOM_THEME_DEFAULTS;
    el.style.background = `linear-gradient(135deg, ${c.from}, ${c.to})`;
}
function setQQTheme(themeId) {
    if (!QQ_THEMES.find(t => t.id === themeId)) return;
    state.theme_qq = themeId;
    safeSet('themeQQ', themeId);
    applyQQTheme();
    flashThemeSwitch();
    if (themeId === 'qq-custom-gradient') applyCustomGradientVars();
    const panel = $('customGradientPanel');
    if (panel) panel.style.display = themeId === 'qq-custom-gradient' ? 'block' : 'none';
}
function applyQQTheme() {
    const html = document.documentElement;
    // 移除所有主题类
    html.classList.remove('theme-default', 'theme-qq-blue', 'theme-qq-green', 'theme-qq-red',
        'theme-qq-purple', 'theme-qq-pink', 'theme-qq-orange', 'theme-qq-cyan',
        'theme-qq-yellow', 'theme-qq-sakura', 'theme-qq-ocean', 'theme-qq-dark',
        'theme-qq-supreme-gold', 'theme-qq-supreme-black-gold', 'theme-qq-harmony-spatial',
        'theme-qq-custom-gradient');
    // 始终挂载基础调色盘（默认蓝），保证 --primary / --on-primary 等语义变量始终有定义
    html.classList.add('theme-default');
    if (state.theme_qq && state.theme_qq !== 'default') {
        html.classList.add('theme-' + state.theme_qq);
    }
    if (state.theme_qq === 'qq-custom-gradient') applyCustomGradientVars();
    // 同步 UI
    document.querySelectorAll('#themeGrid .theme-chip').forEach(c => {
        c.classList.toggle('active', c.dataset.themeId === state.theme_qq);
    });
}

// ── Tab 切换 ──
// 右上角"⋯"在 设置 ↔ 消息 之间切换（侧边栏已删除，作为唯一入口）
function toggleSettings() {
    const m = $('settingsModal');
    if (!m) return;
    if (m.classList.contains('active')) closeSettingsModal();
    else openSettingsModal();
}
function openSettingsModal() {
    const m = $('settingsModal');
    if (!m) return;
    m.classList.add('active');
    // 进入设置时加载插件列表
    if (typeof loadPlugins === 'function') loadPlugins();
    refreshBadgeStats();
}
function closeSettingsModal() {
    const m = $('settingsModal');
    if (m) m.classList.remove('active');
}
function switchTab(tabId) {
    document.querySelectorAll('.tab-page').forEach(p => p.classList.remove('active'));
    document.querySelectorAll('.tab-item').forEach(t => t.classList.remove('active'));
    const target = $(tabId);
    if (target) target.classList.add('active');
    const btn = document.querySelector(`.tab-item[data-tab="${tabId}"]`);
    if (btn) btn.classList.add('active');
    if (tabId === 'tab-messages') {
        // 消息页三栏：默认加载群聊与成员
        loadGroups();
        switchPanel('group-members');
        refreshBadgeStats();
    }
    updateTabIndicator(tabId);
}

// ── 左右栏折叠/恢复 ──
// side: 'groups' | 'members'
function toggleColumn(side) {
    const layout = document.querySelector('.messages-layout');
    if (!layout) return;
    // 手机端（单栏模式）：切换「仅显示该栏」视图（竖条常显，由 CSS 控制）
    if (window.innerWidth <= 1180) {
        if (side === 'messages') {
            // 手机端：折叠/展开消息栏（收起后仅剩三竖条）
            layout.classList.remove('show-groups', 'show-members');
            layout.classList.toggle('collapsed-messages');
            return;
        }
        const active = layout.classList.contains('show-' + side);
        layout.classList.remove('show-groups', 'show-members');
        if (!active) layout.classList.add('show-' + side);
        return;
    }
    // 桌面端：折叠/展开对应栏
    layout.classList.toggle('collapsed-' + side);
}
function restoreColumn(side) {
    const layout = document.querySelector('.messages-layout');
    if (!layout) return;
    // 手机端：恢复后只显示该栏（竖条常显，由 CSS 控制）
    if (window.innerWidth <= 1180) {
        // 消息栏：回到消息视图（同时撤销折叠）
        if (side === 'messages') {
            layout.classList.remove('show-groups', 'show-members', 'collapsed-messages');
            return;
        }
        layout.classList.remove('show-groups', 'show-members', 'collapsed-messages');
        layout.classList.add('show-' + side);
        return;
    }
    // 桌面端：展开对应栏
    layout.classList.remove('collapsed-' + side);
}
// 手机端：从左右栏全屏视图返回消息视图（← 返回按钮）
function closeSidePanels() {
    const layout = document.querySelector('.messages-layout');
    if (!layout) return;
    layout.classList.remove('show-groups', 'show-members');
    // 手机端返回时同时撤销消息栏折叠，保证消息栏可见
    if (window.innerWidth <= 1180) layout.classList.remove('collapsed-messages');
}
// 窗口尺寸变化时：切回消息栏视图（手机端恢复时只显示单栏，转桌面后重置）
let _prevDesktopView = window.innerWidth > 1180;
window.addEventListener('resize', () => {
    const layout = document.querySelector('.messages-layout');
    if (!layout) return;
    const isDesktop = window.innerWidth > 1180;
    if (isDesktop) {
        layout.classList.remove('show-groups', 'show-members');
    } else if (_prevDesktopView) {
        // 桌面 → 手机：默认显示「元宝派」（群聊列表），并撤销消息栏折叠恢复主体
        layout.classList.remove('show-members');
        layout.classList.remove('collapsed-messages');
        layout.classList.add('show-groups');
    }
    _prevDesktopView = isDesktop;
});

// ── 子面板切换（tab-members / tab-settings 内的面板切换） ──
function switchSubPanel(tabId, subpanelId) {
    const tab = $(tabId);
    if (!tab) return;
    // 切换子面板按钮 active
    tab.querySelectorAll('.panel-tabs .panel-tab').forEach(btn => {
        btn.classList.toggle('active', btn.dataset.subpanel === subpanelId);
    });
    // 切换子面板内容显示
    tab.querySelectorAll('[data-subpanel-content]').forEach(c => {
        c.classList.toggle('active', c.dataset.subpanelContent === subpanelId);
    });
}

// ── Tab 指示器位置更新（跑道形滑块，支持横屏垂直模式） ──
function updateTabIndicator(tabId) {
    const indicator = document.getElementById('tabIndicator');
    const activeTab = document.querySelector(`.tab-item[data-tab="${tabId}"]`);
    if (!indicator || !activeTab) return;
    const tabBar = activeTab.parentElement;
    const items = Array.from(tabBar.children).filter(c => c.classList.contains('tab-item'));
    const idx = items.indexOf(activeTab);
    if (idx < 0) return;
    const isLandscape = window.innerWidth >= 900;
    if (isLandscape) {
        // 垂直侧边栏：基于每个 item 的实际 offset
        const pr = tabBar.getBoundingClientRect();
        const ar = activeTab.getBoundingClientRect();
        const gap = Math.max(4, ar.height * 0.06);
        const h = ar.height - gap * 2;
        const y = ar.top - pr.top + gap;
        indicator.style.width = 'calc(100% - 10px)';
        indicator.style.height = h + 'px';
        indicator.style.transform = `translateY(${y}px)`;
    } else {
        // 水平底栏：均分宽度
        const totalWidth = tabBar.offsetWidth;
        const itemWidth = totalWidth / items.length;
        const gap = Math.max(4, itemWidth * 0.04);
        const w = itemWidth - gap * 2;
        const x = idx * itemWidth + gap;
        indicator.style.width = w + 'px';
        indicator.style.transform = `translateX(${x}px)`;
        indicator.style.height = 'calc(100% - 10px)';
    }
}

// ── 页面加载 & 窗口resize 时更新指示器位置 ──
document.addEventListener('DOMContentLoaded', () => {
    updateTabIndicator('tab-messages');
    // 手机版默认显示「元宝派」（群聊列表）而非消息栏
    if (window.innerWidth <= 1180) {
        const layout = document.querySelector('.messages-layout');
        if (layout) { layout.classList.remove('show-members', 'collapsed-messages'); layout.classList.add('show-groups'); }
    }
});
window.addEventListener('resize', () => {
    const active = document.querySelector('.tab-item.active');
    if (active) updateTabIndicator(active.dataset.tab);
});

// ── 合并面板子 Tab 切换 ──
function switchPanel(panelId) {
    document.querySelectorAll('.panel-tab').forEach(t => t.classList.remove('active'));
    document.querySelectorAll('.panel-content').forEach(p => p.classList.remove('active'));
    const tab = document.querySelector(`.panel-tab[data-panel="${panelId}"]`);
    if (tab) tab.classList.add('active');
    $(`panel-${panelId}`).classList.add('active');
    if (panelId === 'group-members') {
        loadMembers();
    }
}

// ── 连接管理 ──
async function connect() {
    const r = await api('/api/connect', { method: 'POST' });
    if (r.ok) {
        state.connected = true;
        // ← 修复：连接后立即获取 bot_id，避免等 3 秒轮询才有值，
        //   否则刚发送的消息（isSelf 判断依赖 botId）不会标记为自己的
        try {
            const st = await api('/api/status');
            if (st && st.bot_id) state.botId = st.bot_id;
        } catch (e) {}
        updateConnectionUI();
        showToast('已连接', 'success');
        startStatusPolling();
    } else {
        showToast((r.message || '连接失败'), 'error');
    }
}
async function disconnect() {
    const r = await api('/api/disconnect', { method: 'POST' });
    state.connected = false;
    updateConnectionUI();
    showToast('已断开', 'warning');
}
function updateConnectionUI() {
    $('statusText').textContent = state.connected ? '已连接' : '未连接';
    $('statusDot').className = `status-dot ${state.connected ? 'connected' : ''}`;
    $('btnConnect').disabled = state.connected;
    $('btnDisconnect').disabled = !state.connected;
    const connText = $('connectionStatusText');
    if (connText) { connText.textContent = state.connected ? '已连接' : '未连接'; connText.style.color = state.connected ? 'var(--success)' : 'var(--danger)'; }
}

// ── 状态轮询（仅启动一次）──
function startStatusPolling() {
    if (state.statusPollingStarted) return;
    state.statusPollingStarted = true;
    setInterval(async () => {
        const r = await api('/api/status');
        if (!r) return;
        state.connected = r.connected;
        // ← 修复：正确读取 bot_id
        if (r.bot_id) state.botId = r.bot_id;
        updateConnectionUI();
        // ← 修复：forward_mode_enabled（不是 forward_mode）
        if (r.forward_mode_enabled !== undefined) {
            state.forwardMode = r.forward_mode_enabled;
            const fms = $('forwardModeStatusTextSmall');
            if (fms) { fms.textContent = r.forward_mode_enabled ? '已启用' : '未启用'; fms.style.color = r.forward_mode_enabled ? 'var(--success)' : 'var(--warning)'; }
        }
        if (r.forward_at_yuanbao !== undefined) {
            const ays = $('atYuanbaoStatusText');
            if (ays) { ays.textContent = r.forward_at_yuanbao ? '已启用' : '未启用'; ays.style.color = r.forward_at_yuanbao ? 'var(--success)' : 'var(--warning)'; }
        }
        if (r.msg_log_enabled !== undefined) {
            state.msgLogEnabled = r.msg_log_enabled;
            syncMsgLogToggles();
        }
        if (r.recall_monitor_enabled !== undefined) {
            state.recallMonitorEnabled = r.recall_monitor_enabled;
            syncRecallMonitorToggles();
        }
        if (r.logger_written !== undefined) {
            const ls = $('loggerWrittenStatus');
            if (ls) ls.textContent = r.logger_written || 0;
        }
    }, 3000);
}

// ── 消息显示 ──
async function loadRecentMessages() {
    const limit = 500;
    // ← 修复：按当前查看的群过滤，避免不同群（派）的消息混合显示；
    //   未选择群时后端兜底返回所有监听群消息
    const g = state.currentGroup;
    const r = await api(`/api/messages?limit=${limit}${g ? '&group_code=' + encodeURIComponent(g) : ''}`);
    if (r && r.messages) {
        // ── 生成签名比对，无变化时不重新渲染（避免闪烁） ──
        const sig = r.messages.map(m => m.sender_id + '|' + m.content + '|' + m.time + '|' + (m.media_info ? m.media_info.type : '')).join('\n');
        if (sig === state._msgSig) return;
        state._msgSig = sig;
        state.messages = r.messages;
        renderMessages();
    }
}
// ← 修复：图片 URL 转后端代理。resourceUrl（hunyuan.tencent.com/api/resource/download?resourceId=...）
//   需 X-Token 鉴权，浏览器 <img> 无法带该头 → 404。通过 /api/image-proxy 后端带 token 拉取。
function proxyImageUrl(u) {
    if (!u) return u;
    if (u.indexOf('hunyuan.tencent.com/api/resource/') >= 0) {
        const m = u.match(/resourceId=([^&]+)/);
        if (m) return '/api/image-proxy?resourceId=' + encodeURIComponent(m[1]);
    }
    return u;  // COS 直链或签名 URL 直接用
}
function renderMessages() {
    const container = $('messageLog');
    const msgs = state.messages;
    if (!msgs.length) { container.innerHTML = '<div class="empty-state">暂无消息</div>'; return; }
    // 建立现有 DOM 节点的 key → element 映射（key = sender + time + content + media_type）
    const existing = new Map();
    container.querySelectorAll('.message-entry').forEach(el => { if (el.dataset.msgKey) existing.set(el.dataset.msgKey, el); });
    const frag = document.createDocumentFragment();
    const newKeys = [];
    msgs.forEach((m, i) => {
        const key = m.sender_id + '|||' + m.time + '|||' + (m.content || '') + '|||' + (m.media_info ? m.media_info.type : 'text');
        newKeys.push(key);
        const isSelf = m.sender_id === state.botId;
        let contentHtml = '';
        const mi = m.media_info;
        if (mi && mi.type === 'image' && mi.image_urls && mi.image_urls.length) {
            // 图片消息：渲染缩略图（可点击查看原图）
            const extra = (m.content && m.content !== '[图片]') ? `<div>${escHtml(m.content)}</div>` : '';
            // ← 修复：resourceUrl（hunyuan.tencent.com/api/resource/download）需 X-Token 鉴权，
            //   浏览器直接访问 404。改走后端代理 /api/image-proxy?resourceId=... 显示
            const imgs = mi.image_urls.map(u => {
                const proxySrc = proxyImageUrl(u);
                return `<img src="${proxySrc}" class="msg-image" alt="[图片]" referrerpolicy="no-referrer" onclick="event.stopPropagation();window.open('${escHtml(u)}','_blank')">`;
            }).join('');
            contentHtml = extra + `<div class="msg-images">${imgs}</div>`;
        } else if (mi && mi.type === 'sticker') {
            // 贴纸消息：优先用本地 ico 图标（按名称对应）
            const sname = mi.sticker_name || '';
            const extra = (m.content && m.content !== '[贴纸]') ? `<div>${escHtml(m.content)}</div>` : '';
            // ← 修改：仅使用本地 ico 图标，未匹配显示"缺图"错误占位，不再回退旧 CDN
            const imgSrc = stickerIconMap[sname] || '';
            contentHtml = extra +
                (imgSrc
                    ? `<img src="${imgSrc}" class="msg-sticker" alt="${escHtml(sname || '贴纸')}" title="${escHtml(sname || '贴纸')}" referrerpolicy="no-referrer" loading="lazy" onerror="this.outerHTML='<span class=&quot;msg-sticker-fallback&quot;>❌</span>'">`
                    : `<span class="msg-sticker-fallback" title="缺少本地图标">❌</span>`) +
                (sname ? `<span class="msg-media-name">${escHtml(sname)}</span>` : '');
        } else if (mi && mi.type === 'file') {
            // 文件消息：显示文件名 + 下载链接
            const fname = mi.file_name || '文件';
            const fsize = mi.file_size ? fmtSize(mi.file_size) : '';
            const fhref = mi.file_url || '#';
            contentHtml = `<div class="msg-file"><a href="${escHtml(fhref)}" target="_blank" rel="noopener noreferrer" onclick="event.stopPropagation()">` +
                `<span class="msg-file-icon">📎</span><span class="msg-file-name">${escHtml(fname)}${fsize ? ` <small>(${fsize})</small>` : ''}</span></a></div>`;
        } else if (mi && mi.type === 'video') {
            // 视频消息：显示视频名称（可点开 URL）
            const vname = mi.name || '视频';
            const vhref = mi.url || '#';
            contentHtml = `<div class="msg-file"><a href="${escHtml(vhref)}" target="_blank" rel="noopener noreferrer" onclick="event.stopPropagation()">` +
                `<span class="msg-file-icon">🎬</span><span class="msg-file-name">${escHtml(vname)}</span></a></div>`;
        } else {
            contentHtml = escHtml(m.content || '');
        }
        const quoteHtml = m.quote ? `<div class="msg-quote"><span class="quote-sender">${escHtml(m.quote.sender_nickname || m.quote.sender_id || '?')}</span><span class="quote-text">${escHtml(m.quote.desc || '')}</span></div>` : '';
        if (existing.has(key)) {
            // 复用已有 DOM 节点（避免闪烁）
            const el = existing.get(key);
            existing.delete(key);
            // 更新序号和 onclick index（可能因增删消息而偏移）
            const idxEl = el.querySelector('.msg-index');
            if (idxEl) idxEl.textContent = '#' + (i + 1);
            el.setAttribute('onclick', 'openReplyModal(' + i + ')');
            // 更新 sender 和 time（极少变化但保持准确）
            const senderEl = el.querySelector('.msg-sender');
            if (senderEl) senderEl.textContent = m.sender_name || m.sender_id || '?';
            const timeEl = el.querySelector('.msg-time');
            if (timeEl) timeEl.textContent = m.time || '';
            frag.appendChild(el);
        } else {
            // 创建新 DOM 节点
            const el = document.createElement('div');
            el.dataset.msgKey = key;
            el.className = 'message-entry ' + (isSelf ? 'self' : 'other');
            el.setAttribute('onclick', 'openReplyModal(' + i + ')');
            el.innerHTML = `<div class="msg-meta"><span class="msg-index">#${i+1}</span><span class="msg-sender">${escHtml(m.sender_name || m.sender_id || '?')}</span><span class="msg-time">${escHtml(m.time || '')}</span></div>${quoteHtml}<div>${contentHtml}</div>`;
            frag.appendChild(el);
        }
    });
    // 移除不再存在的旧节点
    for (const [, el] of existing) el.remove();
    // ← 修复：replaceChildren 为 Chrome 86+ / Safari 14+，老浏览器需降级
    if (container.replaceChildren) {
        container.replaceChildren(frag);
    } else {
        container.innerHTML = '';
        container.appendChild(frag);
    }
}
function escHtml(s) {
    const d = document.createElement('div');
    d.textContent = s;
    return d.innerHTML;
}
// 文件大小格式化（对齐前端展示习惯：B/KB/MB/GB）
function fmtSize(bytes) {
    if (!bytes || bytes <= 0) return '';
    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
    let v = bytes, i = 0;
    while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
    return v.toFixed(v >= 100 ? 0 : 1) + ' ' + units[i];
}
async function clearMessages() {
    // ← 修复：同时清空后端缓存并重新加载，避免清空后自动刷新又把旧消息拉回来
    await api('/api/messages/clear');
    state.messages = [];
    state._msgSig = '';
    $('messageLog').innerHTML = '<div class="empty-state">已清空</div>';
    loadRecentMessages();
}

// ── 消息历史记录弹窗 ──
let _historyMessages = [];
function toggleHistoryModal() {
    const m = $('historyModal');
    if (!m) return;
    if (m.classList.contains('active')) closeHistoryModal();
    else openHistoryModal();
}
// 历史弹窗自动刷新定时器（打开时每 5 秒刷新，关闭时停止）
let historyRefreshTimer = null;
function openHistoryModal() {
    const m = $('historyModal');
    if (!m) return;
    m.classList.add('active');
    const search = $('historySearch');
    if (search) search.value = '';
    loadHistory();
    // ← 自动刷新：历史弹窗打开期间每 5 秒刷新一次
    if (historyRefreshTimer) clearInterval(historyRefreshTimer);
    historyRefreshTimer = setInterval(() => {
        // 仅在弹窗打开且用户未在搜索时刷新
        if (!m.classList.contains('active')) return;
        const q = $('historySearch');
        if (q && q.value.trim()) return;
        loadHistory();
    }, 5000);
}
function closeHistoryModal() {
    const m = $('historyModal');
    if (m) m.classList.remove('active');
    if (historyRefreshTimer) { clearInterval(historyRefreshTimer); historyRefreshTimer = null; }
}
// ── 当前派信息弹窗（右上角 ⋯ 按钮） ──
function toggleGroupInfoModal() {
    const m = $('groupInfoModal');
    if (!m) return;
    if (m.classList.contains('active')) closeGroupInfoModal();
    else openGroupInfoModal();
}
function openGroupInfoModal() {
    const m = $('groupInfoModal');
    if (!m) return;
    // 填充当前派信息
    const code = state.currentGroup || '';
    const name = state.groupNameCache[code] || '';
    // Bot 昵称：优先从成员列表找，其次后端记录的"元宝"
    let botName = '元宝';
    if (state.botId) {
        const bot = (state.members || []).find(x => x.user_id === state.botId);
        if (bot && bot.nick_name) botName = bot.nick_name;
    }
    const memberCount = (state.members || []).length;
    const setName = $('ginfoName'), setCode = $('ginfoCode'),
          setBot = $('ginfoBotName'), setCount = $('ginfoMemberCount');
    if (setName) setName.textContent = name || '(未知)';
    if (setCode) setCode.textContent = code || '(未选择)';
    if (setBot) setBot.textContent = botName;
    if (setCount) setCount.textContent = memberCount ? String(memberCount) : '-';
    m.classList.add('active');
}
function closeGroupInfoModal() {
    const m = $('groupInfoModal');
    if (m) m.classList.remove('active');
}

async function loadHistory() {
    const list = $('historyList');
    if (!list) return;
    list.innerHTML = '<div class="empty-state">加载中...</div>';
    // ← 修复：按当前查看的群过滤历史消息，避免跨群混合
    const g = state.currentGroup;
    const r = await api(`/api/messages?limit=500${g ? '&group_code=' + encodeURIComponent(g) : ''}`);
    if (r && r.messages) {
        _historyMessages = r.messages;
        renderHistory();
    } else {
        list.innerHTML = '<div class="empty-state">历史消息加载失败</div>';
    }
}
function renderHistory() {
    const list = $('historyList');
    if (!list) return;
    const q = ($('historySearch') ? $('historySearch').value.trim() : '').toLowerCase();
    let msgs = _historyMessages;
    if (q) {
        msgs = msgs.filter(m => {
            const hay = ((m.sender_name || '') + ' ' + (m.sender_id || '') + ' ' + (m.content || '') + ' ' + (m.time || '')).toLowerCase();
            return hay.includes(q);
        });
    }
    if (!msgs.length) {
        list.innerHTML = '<div class="empty-state">' + (q ? '未找到匹配消息' : '暂无历史消息') + '</div>';
        return;
    }
    // 按时间正序展示（旧 → 新），最新在底部
    const frag = document.createDocumentFragment();
    msgs.forEach(m => {
        const el = document.createElement('div');
        el.className = 'message-entry ' + (m.sender_id === state.botId ? 'self' : 'other');
        let contentHtml = '';
        if (m.media_info && m.media_info.type === 'image' && m.media_info.image_urls && m.media_info.image_urls.length) {
            contentHtml = `<span class="history-media-tag">[图片]</span>`;
        } else {
            contentHtml = escHtml(m.content || '');
        }
        el.innerHTML = `<div class="msg-meta"><span class="msg-sender">${escHtml(m.sender_name || m.sender_id || '?')}</span><span class="msg-time">${escHtml(m.time || '')}</span></div><div>${contentHtml}</div>`;
        frag.appendChild(el);
    });
    list.replaceChildren ? list.replaceChildren(frag) : (list.innerHTML = '', list.appendChild(frag));
}
function filterHistoryDebounced() {
    // 防抖搜索
    clearTimeout(_historySearchTimer);
    _historySearchTimer = setTimeout(renderHistory, 200);
}
let _historySearchTimer = null;

// ── 自动刷新（默认开启）：消息 2 秒 / 元宝派+成员 5 秒 ──
// 开关已从前端移除，自动刷新始终开启
let autoRefreshTimer = null;
let panelRefreshTimer = null;
function startAutoRefresh() {
    // 先清理旧定时器再创建，避免重复初始化导致定时器累积
    if (autoRefreshTimer) { clearInterval(autoRefreshTimer); autoRefreshTimer = null; }
    if (panelRefreshTimer) { clearInterval(panelRefreshTimer); panelRefreshTimer = null; }
    autoRefreshTimer = setInterval(() => loadRecentMessages(), 2000);
    // 元宝派（群列表）与成员：5 秒刷新一次，避免群名查询/成员列表请求过于频繁
    panelRefreshTimer = setInterval(() => {
        loadGroups();
        loadMembers();
    }, 5000);
}
// 页面加载后立即启动自动刷新（默认开启）
document.addEventListener('DOMContentLoaded', () => {
    setTimeout(startAutoRefresh, 500);
});

// ── V4.2 展开式聊天编辑器 ──
let _composerMode = 'normal';
function closeChatComposer() {
    $('chatComposerOverlay').classList.remove('active');
}
function setComposerMode(mode) {
    _composerMode = mode;
    document.querySelectorAll('#composerModeChips .chip').forEach(c => {
        c.classList.toggle('active', c.dataset.mode === mode);
    });
    const needsTarget = ['latex'].includes(mode);
    $('composerTargetRow').style.display = needsTarget ? 'block' : 'none';
    $('composerCountRow').style.display = 'none';
    $('composerLatexControls').style.display = ['latex'].includes(mode) ? 'block' : 'none';
}
async function sendFromComposer() {
    const text = $('composerText').value.trim();
    if (!text) { showToast('请输入消息内容', 'warning'); return; }
    const body = { text, mode: _composerMode };
    if (['latex'].includes(_composerMode)) {
        body.target_id = $('composerTargetId').value.trim();
        body.target_nick = $('composerTargetNick').value.trim();
    }
    if (['latex'].includes(_composerMode)) {
        body.scale = parseFloat($('composerScaleSlider').value) || 3.0;
        body.rotate = parseInt($('composerRotateSlider').value) || 15;
    }
    const endpoint = '/api/send';
    const r = await api(endpoint, { method: 'POST', body });
    if (r && r.ok) {
        $('composerText').value = '';
        showToast('已发送', 'success');
        loadRecentMessages();
        closeChatComposer();
    } else {
        showToast((r && r.message) || '发送失败', 'error');
    }
}

// ── 输入框 @ 候选选择器（普通模式）──
let _atSuggestIdx = -1;
let _atSuggestPaused = false;  // 选中昵称后暂停候选框，直到用户重新编辑（输入/删除字符）才恢复
let _atPausedValue = '';       // 暂停时的输入框文本快照（文本变化则自动恢复候选框）

// 获取光标前最近一个「@开头」的 token：返回 { atPos, query } 或 null
// @ 前可以是开头、空白或任意字符（中文/标点均可）
function atTokenAt(value, caret) {
    const before = value.slice(0, caret);
    // 从末尾往前找最后一个 @，且 @ 后紧跟非空白的待选词
    let pos = before.lastIndexOf('@');
    while (pos >= 0) {
        const head = before[pos + 1] || '';
        if (head !== ' ' && head !== '　' && head !== '\n' && head !== '\t') {
            // ← 修复：从 @ 位置开始匹配，query 取 @ 后到空白/冒号前的已输入字符，
            //   不能截掉 @ 后的字符（旧代码 slice(0,pos+1) 导致 query 永远为空，候选框从不按输入过滤）
            const m = before.slice(pos).match(/^@([^\s@:：]*)$/);
            if (m) return { atPos: pos, query: m[1] };
        }
        pos = before.lastIndexOf('@', pos - 1);
    }
    return null;
}

function updateAtSuggest() {
    const box = $('atSuggestBox');
    const input = $('inputMessage');
    if (!box || !input || currentMode !== 'normal') { if (box) box.style.display = 'none'; return; }
    // ← 修复：选中昵称后暂停候选框，直到用户重新编辑。
    //   否则含空格昵称（@德 道哥）选中后候选框又会弹出、拦截 Enter，导致无法输入/换行。
    //   兜底：文本发生变化（IME 组词等可能不触发 keydown）则自动恢复
    if (_atSuggestPaused) {
        if (input.value !== _atPausedValue) _atSuggestPaused = false;
        else { box.style.display = 'none'; return; }
    }
    const tok = atTokenAt(input.value, input.selectionStart);
    if (!tok) { box.style.display = 'none'; return; }
    const q = (tok.query || '').toLowerCase();
    // ← 修复：@ 后已是完整成员昵称（如刚选中的 @德道哥 或用户手输完整名）时
    //   不再弹出候选框 —— 否则候选框持续开着，按 Enter 被拦截为"选中候选"，
    //   导致 @ 之后无法换行。只有部分输入（还在筛选）或刚输入 @（待选）才弹出。
    if (q && (state.members || []).some(m =>
        (m.nick_name || '').toLowerCase() === q || (m.user_id || '').toLowerCase() === q)) {
        box.style.display = 'none';
        return;
    }
    let users = (state.members || []).filter(m =>
        !q || (m.nick_name || '').toLowerCase().includes(q) || (m.user_id || '').toLowerCase().includes(q));
    if (!users.length) { box.style.display = 'none'; return; }
    // ← 修复：不再截断，显示所有匹配的派成员（列表自身可滚动）
    box.innerHTML = users.map((u, i) =>
        `<div class="at-suggest-item${i === 0 ? ' active' : ''}" onmousedown="pickAtSuggest(event, ${i})">`
        + `<span class="at-suggest-nick" title="@${escapeHtml(u.nick_name || '')}">@${escapeHtml(u.nick_name || '(无名)')}</span>`
        + `<span class="at-suggest-id">${escapeHtml(u.user_id)}</span></div>`).join('');
    _atSuggestIdx = 0;
    box.style.display = 'block';
    positionAtSuggest();
}

// 候选框用 fixed 定位（对齐输入框，DOM 已挂到 body 下，无祖先裁剪）。
// 按视口剩余空间动态调整高度与朝向，保证列表完整显示、永不出界。
function positionAtSuggest() {
    const box = $('atSuggestBox');
    const input = $('inputMessage');
    if (!box || !input || box.style.display === 'none') return;
    const r = input.getBoundingClientRect();
    const vh = window.innerHeight;
    const MAX_H = Math.min(box.scrollHeight, 210);
    // 上方空间：输入框顶到视口顶部；下方空间：输入框底到视口底部
    const upSpace = r.top - 8;
    const downSpace = vh - r.bottom - 8;
    box.style.position = 'fixed';
    box.style.left = Math.max(8, r.left) + 'px';
    // 宽度取输入框宽度，但不超过视口宽度（避免贴左边界后右侧出屏）
    box.style.width = Math.min(r.width, window.innerWidth - Math.max(8, r.left) - 8) + 'px';
    let top, h;
    if (upSpace >= downSpace && upSpace > 60) {
        // 上方空间更多：向上弹，高度不超过上方空间
        h = Math.min(MAX_H, upSpace);
        top = r.top - h - 6;
    } else if (downSpace > 60) {
        // 下方空间更多：向下弹，高度不超过下方空间
        h = Math.min(MAX_H, downSpace);
        top = r.bottom + 6;
    } else {
        // 上下都紧张：取最大可用空间，从输入框处居中弹出
        h = Math.min(MAX_H, Math.max(upSpace, downSpace, 60));
        top = Math.max(8, r.top - h - 6);
    }
    box.style.top = top + 'px';
    box.style.maxHeight = h + 'px';
    box.style.height = 'auto';
}

function setAtSuggestActive(idx) {
    const box = $('atSuggestBox');
    if (!box) return;
    const items = box.querySelectorAll('.at-suggest-item');
    if (!items.length) return;
    idx = ((idx % items.length) + items.length) % items.length;
    items.forEach((it, i) => it.classList.toggle('active', i === idx));
    _atSuggestIdx = idx;
    const act = items[idx];
    if (!act) return;
    // ← 修复：scrollIntoView 会触发 window scroll 事件，进而再次触发 positionAtSuggest，
    //   可能引发「滚动→重定位→再滚动」循环导致页面卡死。
    //   改为仅调整候选框自身滚动容器的 scrollTop，不冒泡到窗口，从根上消除循环。
    const boxRect = box.getBoundingClientRect();
    const itemRect = act.getBoundingClientRect();
    if (itemRect.top < boxRect.top) box.scrollTop += itemRect.top - boxRect.top - 4;
    else if (itemRect.bottom > boxRect.bottom) box.scrollTop += itemRect.bottom - boxRect.bottom + 4;
}

// 选中候选：把 @已输入 替换为 @昵称，光标置于其后（不加空格，可直接输入 :自定义名）
function pickAtSuggest(e, idx) {
    if (e) e.preventDefault();
    const box = $('atSuggestBox');
    if (!box) return;
    const items = box.querySelectorAll('.at-suggest-item');
    const item = items[idx] || items[_atSuggestIdx] || items[0];
    if (!item) return;
    const nick = item.querySelector('.at-suggest-nick').textContent.replace(/^@/, '');
    const input = $('inputMessage');
    const caret = input.selectionStart;
    const before = input.value.slice(0, caret);
    const after = input.value.slice(caret);
    const tok = atTokenAt(input.value, caret);
    const replace = tok ? before.slice(0, tok.atPos) + '@' + nick : before + '@' + nick;
    // 仅当后面紧贴非空白内容时才补一个空格分隔（保持「@昵称 内容」语义）
    const gap = after && !/^\s/.test(after) ? ' ' : '';
    input.value = replace + gap + after;
    const pos = replace.length + gap.length;
    input.focus();
    input.setSelectionRange(pos, pos);
    box.style.display = 'none';
    // ← 修复：选中后暂停候选框（含空格昵称 @德 道哥 也不会再弹出、拦截输入/Enter），
    //   直到用户重新编辑（keydown 恢复 + 文本变化兜底）
    _atSuggestPaused = true;
    _atPausedValue = input.value;
}

// @ 候选框事件绑定
(function initAtSuggest() {
    const input = $('inputMessage');
    if (!input) return;
    input.addEventListener('input', () => {
        autoResizeTextarea(input);  // ← 输入内容变化时自动增高
        if (currentMode !== 'normal') { const b = $('atSuggestBox'); if (b) b.style.display = 'none'; return; }
        // 暂停期间：文本变化（用户编辑）→ 恢复候选框；文本未变 → 保持暂停
        if (_atSuggestPaused) {
            if (input.value !== _atPausedValue) { _atSuggestPaused = false; updateAtSuggest(); return; }
            const b = $('atSuggestBox'); if (b) b.style.display = 'none'; return;
        }
        updateAtSuggest();
    });
    input.addEventListener('click', () => { if (currentMode === 'normal') updateAtSuggest(); });
    // ← 修复：移除 window scroll 捕获监听，避免候选框重定位触发滚动→重定位循环。
    //   仅窗口 resize 时重新定位（输入框位置变化）；输入框位置随发送面板滚动而变时，
    //   候选框在每次 input/click 事件时都会重新定位，无需监听滚动。
    window.addEventListener('resize', positionAtSuggest);
    // ← 修复：候选框移到 body 下，彻底脱离发送面板的 overflow/裁剪上下文，
    //   保证任何情况下列表完整显示
    const box = $('atSuggestBox');
    if (box && box.parentNode !== document.body) document.body.appendChild(box);
    // ← 补：贴纸小窗也挂到 body 下，避免被发送面板 overflow 裁剪
    const sp = $('stickerPopover');
    if (sp && sp.parentNode !== document.body) document.body.appendChild(sp);
    // 点击输入框/其他区域时关闭贴纸小窗
    document.addEventListener('mousedown', (e) => {
        const sp2 = $('stickerPopover');
        const btn = document.querySelector('.quick-action-btn[title="发送贴纸"]');
        if (sp2 && sp2.style.display === 'block' && !sp2.contains(e.target) && (!btn || !btn.contains(e.target))) {
            sp2.style.display = 'none';
        }
    });
    window.addEventListener('resize', positionStickerPopover);
    input.addEventListener('keydown', (e) => {
        const box = $('atSuggestBox');
        const boxOpen = currentMode === 'normal' && box && box.style.display === 'block';
        // ← 修复：用户重新编辑（输入字符/退格/粘贴）时恢复候选框，
        //   但导航键（方向键/Enter/Escape）不恢复，保持选中后的暂停状态
        if (_atSuggestPaused && currentMode === 'normal'
            && !['ArrowDown', 'ArrowUp', 'ArrowLeft', 'ArrowRight', 'Enter', 'Escape', 'Shift', 'Control', 'Alt', 'Meta'].includes(e.key)) {
            _atSuggestPaused = false;
        }
        // Backspace：光标紧贴 @昵称 后时一次删除整个 @昵称（含可选 :显示名）
        if (currentMode === 'normal' && e.key === 'Backspace' && !e.ctrlKey && !e.metaKey) {
            const selStart = input.selectionStart;
            if (input.selectionEnd === selStart && selStart > 0) {
                const before = input.value.slice(0, selStart);
                // ← 修复：只匹配「光标紧邻」的 @昵称 / @昵称:显示名（昵称不跨空格），
                //   且该昵称必须匹配派成员，才整体删除（避免误删 @ 后的正文）
                const m = before.match(/(?:^|[^@\s])@([^\s@:：]+)(?::([^\s@]*))?$/);
                if (m) {
                    const nick = m[1];
                    const known = (state.members || []).some(x => x.nick_name === nick || x.user_id === nick);
                    if (known) {
                        const atStart = before.lastIndexOf('@');
                        e.preventDefault();
                        input.value = before.slice(0, atStart) + input.value.slice(selStart);
                        input.setSelectionRange(atStart, atStart);
                        if (box) box.style.display = 'none';
                        return;
                    }
                }
            }
        }
        if (!boxOpen) return;
        if (e.key === 'ArrowDown') { e.preventDefault(); setAtSuggestActive(_atSuggestIdx + 1); }
        else if (e.key === 'ArrowUp') { e.preventDefault(); setAtSuggestActive(_atSuggestIdx - 1); }
        else if (e.key === 'Enter') {
            // ← 修复：Shift+Enter 始终换行（候选框打开时也可换行），普通 Enter 才选中候选
            if (e.shiftKey) { box.style.display = 'none'; return; }
            // ← 修复：候选框打开但还没输入任何昵称（刚输入 @）时，Enter 视为"放弃选择并换行"，
            //   而不是选中第一个候选 —— 否则用户只想换行却总是被选中候选
            const tok = atTokenAt(input.value, input.selectionStart);
            if (!tok || !tok.query) {
                box.style.display = 'none';
                return;
            }
            e.preventDefault(); pickAtSuggest(null, _atSuggestIdx);
        }
        else if (e.key === 'Escape') { box.style.display = 'none'; }
    });
})();

// 解析输入框中的 @昵称 / @昵称:显示名 → { parts, text, ats }
// parts: 有序片段 [{type:'text',text}|{type:'at',user_id,display}]，保持 @ 原始位置
function parseAtMessage(raw) {
    const members = state.members || [];
    const byNick = {}, byId = {};
    members.forEach(m => {
        if (m.nick_name) byNick[m.nick_name] = m;
        if (m.user_id) byId[m.user_id] = m;
    });
    const parts = [];
    const ats = [];
    let cursor = 0; // 已消费的原文位置
    let idx = 0;
    while ((idx = raw.indexOf('@', idx)) >= 0) {
        const na = raw.indexOf('@', idx + 1);
        const nl = raw.indexOf('\n', idx + 1);
        const stop = Math.min(na < 0 ? raw.length : na, nl < 0 ? raw.length : nl);
        const seg = raw.slice(idx + 1, stop);
        if (!seg) { idx = stop; continue; }
        // 昵称区域：到「冒号(在空白前)」或「第一个空白」或段尾
        const ci = seg.search(/[:：]/);
        const ws = seg.search(/\s/);
        const hasColon = ci >= 0 && (ws < 0 || ci < ws);
        const nameEnd = hasColon ? ci : (ws < 0 ? seg.length : ws);
        const words = seg.slice(0, nameEnd).split(/\s+/);
        // 从长到短逐词扩展匹配（支持含空格昵称）
        let member = null, cand = '';
        for (let k = words.length; k >= 1; k--) {
            const c = words.slice(0, k).join(' ');
            if (byNick[c] || byId[c]) { member = byNick[c] || byId[c]; cand = c; break; }
        }
        if (!member) { idx = stop; continue; }
        // 显示名：冒号后到空白（自定义昵称）
        let display = '';
        if (hasColon) {
            const rest = seg.slice(ci + 1);
            const rws = rest.search(/\s/);
            display = (rws < 0 ? rest : rest.slice(0, rws)).trim();
        }
        // @ 片段结束位置（相对原文）
        const tokenEnd = idx + 1 + cand.length + (hasColon ? (ci - cand.length) + 1 + display.length : 0);
        // 保留 @ 前面的文本片段
        if (idx > cursor) parts.push({ type: 'text', text: raw.slice(cursor, idx) });
        parts.push({ type: 'at', user_id: member.user_id, display: display || member.nick_name || '' });
        ats.push({ user_id: member.user_id, nick: member.nick_name || '', display: display || member.nick_name || '' });
        cursor = tokenEnd;
        idx = stop;
    }
    // 尾部剩余文本
    if (cursor < raw.length) parts.push({ type: 'text', text: raw.slice(cursor) });
    if (!parts.length) parts.push({ type: 'text', text: raw });
    // text：拼接所有文本片段（去掉 @），供后端兜底/日志
    const text = parts.filter(p => p.type === 'text').map(p => p.text).join('').replace(/\s+/g, ' ').trim();
    return { parts, text, ats };
}

// 高级选项折叠（普通模式）
function toggleAdvOptions() {
    const body = $('advOptionsBody');
    const caret = $('advCaret');
    if (!body) return;
    const open = body.style.display !== 'none';
    body.style.display = open ? 'none' : 'block';
    if (caret) caret.textContent = open ? '▸' : '▾';
}

// ← 补：私聊窗口高级选项折叠（与普通模式一致）
function toggleDmAdvOptions() {
    const body = $('dmAdvOptionsBody');
    const caret = $('dmAdvCaret');
    if (!body) return;
    const open = body.style.display !== 'none';
    body.style.display = open ? 'none' : 'block';
    if (caret) caret.textContent = open ? '▸' : '▾';
}

// ── 待发送媒体队列（图片/文件/贴纸先入输入框，点发送才真正发送） ──
let pendingMedia = [];  // {type:'image'|'file'|'sticker', file?, name, size?}

// 贴纸小窗点击 → 入队 + 输入框占位
function pickStickerToQueue(name) {
    queuePendingMedia({ type: 'sticker', name });
    const box = $('stickerPopover');
    if (box) box.style.display = 'none';
}

// 加入待发送队列并在输入框显示占位文本
function queuePendingMedia(item) {
    pendingMedia.push(item);
    const input = $('inputMessage');
    if (input) {
        const placeholder = item.type === 'sticker'
            ? `[贴纸:${item.name}]`
            : `[${item.type === 'image' ? '图片' : '文件'}:${item.name}]`;
        // ← 修复：占位符单独一行，且末尾追加换行，保证最后一个占位符后也可换行
        const sep = input.value && !input.value.endsWith('\n') ? '\n' : '';
        input.value = (input.value || '') + sep + placeholder + '\n';
        autoResizeTextarea(input);
        input.focus();
        input.setSelectionRange(input.value.length, input.value.length);
    }
    showToast('已加入待发送，点击发送按钮发送', 'info');
}

// ← 补：textarea 自动增高（内容变化时自适应，无需手动拉伸）
function autoResizeTextarea(el) {
    if (!el) return;
    el.style.height = 'auto';
    el.style.height = (el.scrollHeight > 160 ? 160 : Math.max(80, el.scrollHeight)) + 'px';
}

// 按顺序发送指定媒体列表（支持刷屏：每轮都发送一遍全部媒体）
async function sendMediaList(items) {
    if (!items || !items.length) return true;
    const curGroup = (typeof state !== 'undefined' && state.currentGroup) || '';
    let allOk = true;
    // ← 修复：媒体发送中给出进度反馈，避免上传大图时页面无响应误以为"发送有问题"
    showToast(items.length > 1 ? `正在发送 ${items.length} 个媒体...` : `正在发送${items[0].type === 'image' ? '图片' : (items[0].type === 'file' ? '文件' : '贴纸')}...`, 'info', 8000);
    for (const it of items) {
        try {
            if (it.type === 'sticker') {
                // 后端 /api/send-sticker 读取 body["name"]
                const r = await api('/api/send-sticker', { method: 'POST', body: { name: it.name, group: curGroup } });
                if (!r || !r.ok) throw new Error((r && r.message) || '贴纸发送失败');
            } else {
                const fd = new FormData();
                fd.append('file', it.file);
                if (curGroup) fd.append('group', curGroup);
                const res = await fetch(it.type === 'image' ? '/api/send-image' : '/api/send-file', { method: 'POST', body: fd });
                const j = await res.json();
                if (!j.ok) throw new Error(j.message || (it.type === 'image' ? '图片发送失败' : '文件发送失败'));
            }
        } catch (e) {
            allOk = false;
            showToast((e && e.message) || '媒体发送失败', 'error');
        }
    }
    return allOk;
}

// ← 修复：带媒体刷屏。清空待发送队列并返回媒体项（供每轮刷屏使用）
function takePendingMedia() {
    const items = pendingMedia.slice();
    pendingMedia = [];
    return items;
}

// ── 发送消息 ──
let currentMode = 'normal';

// ← 修改：输入框左下角快捷按钮 → 直接弹出文件选择器（不展开媒体面板、不切换模式、不打断输入）
function openQuickMedia(type) {
    const el = type === 'image' ? $('imageFile') : $('documentFile');
    if (el) { el.value = ''; el.click(); }  // 清空 value 确保重复选择同一文件也触发 change
}
function setMode(mode) {
    currentMode = mode;
    document.querySelectorAll('.chip').forEach(c => c.classList.remove('active'));
    const chip = document.querySelector(`.chip[data-mode="${mode}"]`);
    if (chip) chip.classList.add('active');
    const isMedia = mode === 'media';
    const isSticker = mode === 'sticker';
    // ← 修改：LaTeX 已移入普通模式高级选项，不再有独立 latex 模式
    // media 模式：显示图片/文件功能区；sticker 模式：显示贴纸区；均隐藏文本发送区
    const mediaCtl = $('mediaControls');
    if (mediaCtl) mediaCtl.style.display = isMedia ? 'block' : 'none';
    const stickerCtl = $('stickerControls');
    if (stickerCtl) stickerCtl.style.display = isSticker ? 'block' : 'none';
    const sendText = $('sendTextArea');
    if (sendText) sendText.style.display = (isMedia || isSticker) ? 'none' : '';
    $('targetRow').style.display = 'none';
    $('countRow').style.display = 'none';
    $('targetLabel').textContent = '@ 目标用户ID';
    $('targetHint').textContent = '';
    // 普通模式显示「高级选项」（含刷屏 + LaTeX 编辑器）
    const advOptions = $('advOptions');
    if (advOptions) advOptions.style.display = (mode === 'normal') ? '' : 'none';
    // 非普通模式关闭 @ 候选框
    if (mode !== 'normal') { const sb = $('atSuggestBox'); if (sb) sb.style.display = 'none'; }
}
async function sendMessage() {
    // ← 修复：发送中禁用发送按钮，防止重复点击造成连发/卡顿
    if (_sending) return;
    const text = $('inputMessage').value.trim();

    // 普通模式：解析 @ 候选、支持高级选项（刷屏次数 + LaTeX 编辑器 + 媒体）
    if (currentMode === 'normal') {
        _sending = true;
        const sendBtn = document.querySelector('.quick-send-btn');
        if (sendBtn) { sendBtn.disabled = true; sendBtn.style.opacity = '.5'; }
        try {
            return await doSendNormal(text);
        } finally {
            _sending = false;
            if (sendBtn) { sendBtn.disabled = false; sendBtn.style.opacity = '1'; }
        }
    }
}
let _sending = false;

// ← 抽出的普通模式发送主逻辑（便于发送中状态管理）
async function doSendNormal(text) {
        // 高级选项刷屏次数（展开才生效）
        const advBody = $('advOptionsBody');
        const spamOpen = advBody && advBody.style.display !== 'none';
        let spamCount = 1, spamInterval = 0.5;
        if (spamOpen) {
            spamCount = parseInt($('advCount').value) || 1;
            spamInterval = parseFloat($('advInterval').value) || 0.5;
        }

        // 媒体 + LaTeX 同时存在时，优先 LaTeX（与之前行为一致）
        if (latexItems.length > 0) {
            const finalMessage = latexItems.map(it => it.code).join('\n');
            const body = { text: finalMessage, mode: 'normal' };
            if (spamCount > 1) { body.count = spamCount; body.interval = spamInterval; }
            const r = await api('/api/send', { method: 'POST', body });
            if (r && r.ok) {
                showToast('LaTeX 消息已发送', 'success');
                clearLatexList();
                loadRecentMessages();
            } else {
                showToast((r && r.message) || '发送失败', 'error');
            }
            return;
        }

        // 取出待发送媒体（清空队列）
        const mediaItems = takePendingMedia();
        // 过滤占位符后得到真实文本
        // ← 修复：只使用过滤后的真实文本，不能回退到含占位符的原始 text，
        //   否则只有媒体时会把 [贴纸:x] 等占位符当正文发送
        const realText = text.split('\n').filter(line =>
            !/^\[(?:图片|文件|贴纸):.*\]$/.test(line.trim())).join('\n').trim();
        const parsed = parseAtMessage(realText);
        const hasText = parsed.parts.some(p => p.type === 'text' && p.text.trim());

        // 无媒体且无文本 → 提示
        if (mediaItems.length === 0 && !hasText && parsed.ats.length === 0) {
            showToast('请输入消息内容', 'warning');
            return;
        }

        // ← 修复：媒体 + 文本一起刷屏。每轮 = 发一遍全部媒体 + 发一次文本
        const rounds = Math.max(1, spamCount);
        const sendTextBody = (() => {
            const b = { text: parsed.text, mode: 'normal' };
            if (parsed.ats.length > 0) b.parts = parsed.parts;
            return b;
        })();
        let okAll = true;
        for (let i = 0; i < rounds; i++) {
            // 每轮先发媒体（图片/文件/贴纸）
            if (mediaItems.length) {
                if (!await sendMediaList(mediaItems)) okAll = false;
            }
            // 再发文本（无文本则跳过）
            if (hasText || parsed.ats.length > 0) {
                const r = await api('/api/send', { method: 'POST', body: sendTextBody });
                if (!(r && r.ok)) okAll = false;
            }
            // 轮次间隔
            if (i < rounds - 1 && spamCount > 1) {
                await new Promise(res => setTimeout(res, spamInterval * 1000));
            }
        }
        if (okAll) showToast(rounds > 1 ? `已发送 ${rounds} 轮` : '发送成功', 'success');
        else showToast('部分发送失败', 'error');
        $('inputMessage').value = '';
        autoResizeTextarea($('inputMessage'));  // ← 发送后重置输入框高度
        loadRecentMessages();
        return;
}

// ── LaTeX 消息编辑器（v6.0：从旧版移植）──

// ── LaTeX 消息编辑器（v6.0：从旧版移植）──
let latexItems = [];
let latexSettings = { text: '测试', scale: 3.0, rotate: 15, fontStyle: 'normal', hScale: 1.0, vScale: 1.0, border: false };

function getFontStyleName(style) {
    const map = { 'normal': '常规', 'bold': '粗体', 'italic': '斜体', 'bold-italic': '粗斜体' };
    return map[style] || style;
}

function updateLatexSettings() {
    latexSettings.text = $('latexText').value;
    latexSettings.scale = parseFloat($('scaleSlider').value);
    latexSettings.rotate = parseInt($('rotateSlider').value);
    latexSettings.fontStyle = $('fontStyleSelect').value;
    latexSettings.hScale = parseFloat($('hScaleSlider').value);
    latexSettings.vScale = parseFloat($('vScaleSlider').value);
    latexSettings.border = $('borderCheckbox').checked;
    $('scaleValue').textContent = latexSettings.scale.toFixed(1);
    $('rotateValue').textContent = latexSettings.rotate + '°';
    $('fontStyleValue').textContent = getFontStyleName(latexSettings.fontStyle);
    $('hScaleValue').textContent = latexSettings.hScale.toFixed(1);
    $('vScaleValue').textContent = latexSettings.vScale.toFixed(1);
}

function generateLatexCode() {
    const { text, scale, rotate, fontStyle, hScale, vScale, border } = latexSettings;
    const escaped = (text || '').replace(/[\\&%$#_{}~^]/g, '\\$&');
    let fontCmd = '';
    if (fontStyle === 'bold') fontCmd = '\\mathbf{';
    else if (fontStyle === 'italic') fontCmd = '\\mathit{';
    else if (fontStyle === 'bold-italic') fontCmd = '\\boldsymbol{';
    let content = fontCmd ? (fontCmd + escaped + '}') : escaped;
    let scaled = content;
    if (scale !== 1.0 || hScale !== 1.0 || vScale !== 1.0) {
        if (hScale !== 1.0 || vScale !== 1.0) {
            scaled = `\\scalebox{${hScale.toFixed(1)}}[${vScale.toFixed(1)}]{${content}}`;
        } else {
            scaled = `\\scalebox{${scale.toFixed(1)}}{${content}}`;
        }
    }
    let rotated = scaled;
    if (rotate !== 0) rotated = `\\rotatebox{${rotate}}{${scaled}}`;
    let final = rotated;
    if (border) final = `\\fbox{${rotated}}`;
    return `$${final}$`;
}

function updateLatexPreview() {
    updateLatexSettings();
    const code = generateLatexCode();
    $('latexPreview').textContent = code;
    $('latexCode').value = code;
}

function initLatexControls() {
    ['latexText','scaleSlider','rotateSlider','fontStyleSelect','hScaleSlider','vScaleSlider','borderCheckbox'].forEach(id => {
        const el = $(id);
        if (!el) return;
        const ev = id === 'fontStyleSelect' || id === 'borderCheckbox' ? 'change' : 'input';
        el.addEventListener(ev, updateLatexPreview);
    });
    updateLatexPreview();
}

function addToLatexList() {
    const text = latexSettings.text;
    if (!text || !text.trim()) { showToast('请输入文本内容', 'warning'); return; }
    const code = generateLatexCode();
    latexItems.push({ text, code, settings: { ...latexSettings } });
    $('latexListContainer').style.display = 'block';
    renderLatexList();
    // ← 修复：把生成的 LaTeX 代码追加到普通消息内容框，用户可直接看到内容并发送
    const input = $('inputMessage');
    if (input) {
        input.value = input.value ? input.value + '\n' + code : code;
        input.focus();
        input.setSelectionRange(input.value.length, input.value.length);
    }
    showToast('✅ 已添加到消息列表', 'success');
}

function renderLatexList() {
    const list = $('latexList');
    if (!list) return;
    if (latexItems.length === 0) {
        list.innerHTML = '<div style="text-align:center;color:var(--text-muted);padding:20px;">消息列表为空</div>';
        $('latexListContainer').style.display = 'none';
        return;
    }
    list.innerHTML = latexItems.map((item, idx) => `
        <div class="latex-list-item">
            <div class="latex-list-content">${escHtml(item.code)}</div>
            <div class="latex-list-actions">
                <button class="btn btn-outline btn-small" onclick="moveLatexItemUp(${idx})" ${idx===0?'disabled':''}>⬆</button>
                <button class="btn btn-outline btn-small" onclick="moveLatexItemDown(${idx})" ${idx===latexItems.length-1?'disabled':''}>⬇</button>
                <button class="btn btn-danger btn-small" onclick="removeLatexItem(${idx})">删除</button>
            </div>
        </div>
    `).join('');
}

function moveLatexItemUp(i) {
    if (i > 0) { [latexItems[i], latexItems[i-1]] = [latexItems[i-1], latexItems[i]]; renderLatexList(); }
}
function moveLatexItemDown(i) {
    if (i < latexItems.length - 1) { [latexItems[i], latexItems[i+1]] = [latexItems[i+1], latexItems[i]]; renderLatexList(); }
}
function removeLatexItem(i) { latexItems.splice(i, 1); renderLatexList(); }
function clearLatexList() { latexItems = []; renderLatexList(); }
function clearLatexSettings() {
    latexSettings = { text: '测试', scale: 3.0, rotate: 15, fontStyle: 'normal', hScale: 1.0, vScale: 1.0, border: false };
    $('latexText').value = '测试';
    $('scaleSlider').value = '3.0';
    $('rotateSlider').value = '15';
    $('fontStyleSelect').value = 'normal';
    $('hScaleSlider').value = '1.0';
    $('vScaleSlider').value = '1.0';
    $('borderCheckbox').checked = false;
    updateLatexPreview();
}

// ── 贴纸 ──
let stickerList = [];
let stickerIconMap = {};  // ← 修改：贴纸名称 → 本地 ico URL（/api/sticker-icon?name=...）
async function loadStickers() {
    const r = await api('/api/stickers');
    if (r && r.stickers) {
        stickerList = r.stickers;
        // ← 修改：收集本地图标映射（名称对应），未匹配的贴纸明确报错（不再回退旧 CDN）
        stickerIconMap = {};
        const missing = [];
        r.stickers.forEach(s => { if (s.icon) stickerIconMap[s.name] = s.icon; else missing.push(s.name); });
        renderStickers();
        if (missing.length) {
            const list = missing.slice(0, 8).join('、') + (missing.length > 8 ? ` 等共 ${missing.length} 个` : '');
            showToast(`贴纸缺少本地图标：${list}`, 'error', 6000);
        }
    }
}
// ── 贴纸图片 URL ─────────────────────────────
// ← 修改：仅使用本地 ico 图标（按名称对应），未匹配返回空串由调用方显示"缺图"错误占位，
//   不再回退旧 qzonestyle CDN
function stickerImageUrl(s) {
    if (s && s.icon) return s.icon;
    return '';
}
function renderStickers() {
    const grid = $('stickerGrid');
    const q = ($('stickerSearch').value || '').toLowerCase();
    const items = stickerList.filter(s => !q || s.name.includes(q));
    grid.innerHTML = items.map(s => {
        const img = stickerImageUrl(s);
        const sel = state.selectedSticker === s.name ? 'selected' : '';
        return `<div class="sticker-item ${sel}" onclick="selectSticker('${s.name}')" ondblclick="sendStickerByName('${s.name}')" title="${s.name}${img ? '' : '（缺少本地图标）'}">` +
            (img ? `<img class="sticker-emoji" src="${img}" alt="${s.name}" loading="lazy" referrerpolicy="no-referrer" onerror="this.style.display='none';this.insertAdjacentHTML('afterend','<span class=&quot;sticker-emoji-fallback&quot;>❌</span>')">` : `<span class="sticker-emoji-fallback">❌</span>`) +
            `<span class="sticker-name">${s.name}</span></div>`;
    }).join('');
}
// ← 修改：输入框左下角贴纸快捷按钮 → 弹出贴纸选择小窗（仿 @ 候选框），
//   点击贴纸直接发送并收起，不切换模式、不打断输入
function openQuickSticker() {
    const box = $('stickerPopover');
    if (!box) return;
    if (box.style.display === 'block') { box.style.display = 'none'; return; }
    if (!stickerList.length) loadStickers();
    // 渲染贴纸网格（点击 → 加入待发送队列，显示在输入框，不立即发送）
    const items = (stickerList.length ? stickerList : [{ name: '（加载中...）' }]).slice(0, 40);
    box.innerHTML = `<div class="sticker-pop-title">选择贴纸</div><div class="sticker-pop-grid">` + items.map(s => {
        const img = stickerImageUrl(s);
        return `<div class="sticker-pop-item" onclick="pickStickerToQueue('${s.name.replace(/'/g, "\\'")}')" title="${s.name}${img ? '' : '（缺少本地图标）'}">` +
            (img ? `<img src="${img}" alt="${s.name}" loading="lazy" referrerpolicy="no-referrer" onerror="this.style.display='none';this.insertAdjacentHTML('afterend','<span class=&quot;sticker-emoji-fallback&quot;>❌</span>')">` : `<span class="sticker-emoji-fallback">❌</span>`) +
            `</div>`;
    }).join('') + `</div>`;
    box.style.display = 'block';
    positionStickerPopover();
}
function positionStickerPopover() {
    const box = $('stickerPopover');
    const input = $('inputMessage');
    if (!box || !input || box.style.display === 'none') return;
    const r = input.getBoundingClientRect();
    const vh = window.innerHeight;
    box.style.position = 'fixed';
    box.style.left = Math.max(8, r.left) + 'px';
    box.style.width = '320px';
    // 优先向上弹，空间不足向下
    const idealH = Math.min(box.scrollHeight, 300);
    const upSpace = r.top - 8;
    const downSpace = vh - r.bottom - 8;
    let top;
    if (upSpace >= downSpace && upSpace > 80) top = r.top - idealH - 6;
    else top = r.bottom + 6;
    box.style.top = top + 'px';
    box.style.maxHeight = Math.min(idealH, Math.max(upSpace, downSpace, 120)) + 'px';
    box.style.overflowY = 'auto';
}
function hideStickerPopover() { const b = $('stickerPopover'); if (b) b.style.display = 'none'; }
let _stickerFromQuick = false;

function selectSticker(name) { state.selectedSticker = name; renderStickers(); }
function sendStickerByName(name) {
    state.selectedSticker = name;
    renderStickers();
    // ← 补：从贴纸小窗（😀快捷按钮）发送时，直接收起小窗（不经过贴纸面板）
    const sp = $('stickerPopover');
    if (sp && sp.style.display === 'block') {
        sp.style.display = 'none';
        sendSticker();
        return;
    }
    sendSticker();
}
function filterStickers() { renderStickers(); }
async function sendSticker() {
    if (!state.selectedSticker) { showToast('请先选择贴纸', 'warning'); return; }
    const body = { name: state.selectedSticker };
    const text = $('stickerText').value.trim(); if (text) body.text = text;
    const at = $('stickerAtUser').value.trim();
    if (at) {
        body.at_user = at;
        body.at_nickname = $('stickerAtNick').value.trim();
    }
    body.count = parseInt($('stickerCount').value) || 1;
    body.interval = parseFloat($('stickerInterval').value) || 0.1;
    // ← 修复：路径 /api/send-sticker
    const r = await api('/api/send-sticker', { method: 'POST', body });
    if (r && r.ok) {
        showToast('贴纸已发送', 'success');
        loadRecentMessages();   // ← 立即刷新，显示自己刚发送的贴纸
        // ← 补：从输入框快捷按钮打开时，发送后自动收起贴纸面板
        if (_stickerFromQuick) {
            _stickerFromQuick = false;
            const ctl = $('stickerControls');
            if (ctl) ctl.style.display = 'none';
        }
    }
    else { showToast((r && r.message) || '发送失败', 'error'); }
}

// ── 群成员 ──
async function loadMembers() {
    // ← 修复：请求时带上当前查看的群，切换派后成员列表随群刷新
    const g = state.currentGroup;
    const r = await api(`/api/members${g ? '?group_code=' + encodeURIComponent(g) : ''}`);
    if (r && r.ok && r.members) {
        if (r.members.length) {
            state.members = r.members;
            state.membersGroup = g || state.currentGroup || '';
            state.groupOwnerUserId = r.group_owner_user_id || '';
            // ← 显示成员人数（括号形式，如 (43)）
            const badge = $('memberCountBadge');
            if (badge) badge.textContent = `(${r.members.length})`;
            renderMembers();
        } else if (state.membersGroup !== (g || state.currentGroup)) {
            // ← 修复：当前群无成员且与旧成员所属群不同（跨群/切换后），
            //   显示空态而非错误保留上一个群的成员
            state.members = [];
            $('memberList').innerHTML = '<div class="empty-state">该群暂无成员数据</div>';
            const badge = $('memberCountBadge');
            if (badge) badge.textContent = '';
        }
        // 当前群已有旧成员但本次返回空（临时故障）：保留旧成员，候选框仍可用
    } else {
        $('memberList').innerHTML = `<div class="empty-state">${r.message || '获取失败或无权限'}</div>`;
    }
}

// ── 自定义铭牌/钻标（V4.6 恢复）──
const BADGE_PRESETS = {
    owner: { text: '群主' }, admin: { text: '管理员' }, mod: { text: '版主' },
    bot: { text: '机器人' }, ai: { text: '元宝AI' }, vip: { text: 'VIP' },
    svip: { text: 'SVIP' }, svip_year: { text: '年SVIP' }, big_vip: { text: '大会员' },
    partner: { text: '合作方' }, member: { text: '成员' },
    yellow_dia: { text: '黄钻' }, green_dia: { text: '绿钻' }, blue_dia: { text: '蓝钻' },
    purple_dia: { text: '紫钻' }, pink_dia: { text: '粉钻' }, black_dia: { text: '黑钻' },
    red_dia: { text: '红钻' }, gold_dia: { text: '金钻' },
};
const _badgesDisabled = true;
let _editingBadgeUid = null;
let _editingBadgeNick = '';
let _currentBadgeType = 'admin';
let _currentBadgeText = '管理员';
let _currentAuth = false;
let _currentAvatar = '';

function openBadgeEditor(uid, nick) {
    _editingBadgeUid = uid;
    const _mem = (state.members || []).find(x => x.user_id === uid);
    _editingBadgeNick = (nick || (_mem && _mem.nick_name) || '(无名)');
    const existing = state.memberBadges[uid] || {};
    _currentBadgeType = existing.type || 'admin';
    _currentBadgeText = existing.text || (BADGE_PRESETS[_currentBadgeType] || {}).text || '成员';
    _currentAuth = state.memberAuth[uid] === true;
    _currentAvatar = state.memberAvatars[uid] || '';
    $('badgeTargetNick').textContent = _editingBadgeNick;
    $('badgeTargetId').textContent = '(' + uid + ')';
    $('badgeNick').value = existing.nick || '';
    $('badgeCustomText').value = existing.text || '';
    $('badgeAuthToggle').checked = _currentAuth;
    // 头像预览
    const ap = $('badgeAvatarPreview');
    if (ap) {
        if (_currentAvatar) ap.innerHTML = `<img src="${_currentAvatar}" alt="avatar">`;
        else ap.textContent = (_editingBadgeNick || '?').charAt(0).toUpperCase();
    }
    // 高亮当前选中
    document.querySelectorAll('#badgePresetGrid .badge-preset').forEach(el => {
        el.classList.toggle('active', el.dataset.type === _currentBadgeType);
    });
    updateBadgePreview();
    $('badgeModal').classList.add('active');
}
function closeBadgeModal() { $('badgeModal').classList.remove('active'); _editingBadgeUid = null; }
function selectBadgePreset(type, text) {
    _currentBadgeType = type;
    _currentBadgeText = text;
    $('badgeCustomText').value = '';
    document.querySelectorAll('#badgePresetGrid .badge-preset').forEach(el => {
        el.classList.toggle('active', el.dataset.type === type);
    });
    updateBadgePreview();
}
function onBadgeAvatarSelect(e) {
    const file = e.target.files[0];
    if (!file) return;
    if (file.size > 2 * 1024 * 1024) { showToast('图片过大（限制 2MB）', 'error'); return; }
    const reader = new FileReader();
    reader.onload = ev => {
        _currentAvatar = ev.target.result;
        const ap = $('badgeAvatarPreview');
        if (ap) ap.innerHTML = `<img src="${_currentAvatar}" alt="avatar">`;
        showToast('头像已选择，记得点保存', 'success');
    };
    reader.readAsDataURL(file);
}
function removeBadgeAvatar() {
    _currentAvatar = '';
    const ap = $('badgeAvatarPreview');
    if (ap) ap.textContent = (_editingBadgeNick || '?').charAt(0).toUpperCase();
}
function updateBadgePreview() {
    const customText = ($('badgeCustomText').value || '').trim();
    const nick = ($('badgeNick').value || '').trim();
    const text = customText || _currentBadgeText;
    const type = _currentBadgeType;
    const _authEl = $('badgeAuthToggle');
    const auth = _authEl ? _authEl.checked : false;
    const authHtml = auth ? '<span class="auth-badge-vip">V</span>' : '';
    const showName = nick || _editingBadgeNick;
    $('badgePreview').innerHTML =
        `<span id="badgePreviewName" style="font-weight:600">${escapeHtml(showName)}${authHtml}</span>` +
        `<span class="custom-badge ${type}"><span class="badge-dot"></span>${escapeHtml(text)}</span>`;
}
function saveBadge() {
    if (!_editingBadgeUid) return;
    const customText = ($('badgeCustomText').value || '').trim();
    const nick = ($('badgeNick').value || '').trim();
    const text = customText || _currentBadgeText;
    const type = _currentBadgeType;
    const _authEl = $('badgeAuthToggle');
    const auth = _authEl ? _authEl.checked : false;
    state.memberBadges[_editingBadgeUid] = { text, type, nick, updated: Date.now() };
    safeSet('memberBadges', JSON.stringify(state.memberBadges));
    state.memberAuth[_editingBadgeUid] = auth;
    safeSet('memberAuth', JSON.stringify(state.memberAuth));
    if (_currentAvatar) {
        state.memberAvatars[_editingBadgeUid] = _currentAvatar;
        safeSet('memberAvatars', JSON.stringify(state.memberAvatars));
    } else {
        delete state.memberAvatars[_editingBadgeUid];
        safeSet('memberAvatars', JSON.stringify(state.memberAvatars));
    }
    const parts = [];
    if (nick) parts.push('昵称：' + nick);
    if (text) parts.push('铭牌：' + text);
    if (auth) parts.push('认证');
    if (_currentAvatar) parts.push('头像');
    showToast(`已为 ${_editingBadgeNick} 设置（${parts.join(' / ')}）`, 'success');
    closeBadgeModal();
    renderMembers();
    refreshBadgeStats();
}
function removeBadge() {
    if (!_editingBadgeUid) return;
    delete state.memberBadges[_editingBadgeUid];
    delete state.memberAuth[_editingBadgeUid];
    delete state.memberAvatars[_editingBadgeUid];
    safeSet('memberBadges', JSON.stringify(state.memberBadges));
    safeSet('memberAuth', JSON.stringify(state.memberAuth));
    safeSet('memberAvatars', JSON.stringify(state.memberAvatars));
    showToast(`已移除 ${_editingBadgeNick} 的所有自定义资料`, 'success');
    closeBadgeModal();
    renderMembers();
    refreshBadgeStats();
}
function clearAllBadges() {
    if (!confirm('确认清空所有自定义铭牌、认证和头像？此操作不可撤销。')) return;
    state.memberBadges = {};
    state.memberAuth = {};
    state.memberAvatars = {};
    safeSet('memberBadges', '{}');
    safeSet('memberAuth', '{}');
    safeSet('memberAvatars', '{}');
    showToast('已清空所有自定义资料', 'success');
    renderMembers();
    refreshBadgeStats();
}
function exportMemberBadges() {
    const data = {
        version: '4.2',
        exportedAt: new Date().toISOString(),
        badges: state.memberBadges,
        auth: state.memberAuth,
        avatars: state.memberAvatars,
    };
    const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `yuanbao_member_data_${new Date().toISOString().slice(0,10)}.json`;
    a.click();
    URL.revokeObjectURL(url);
    showToast('已导出（包含铭牌/认证/头像）', 'success');
}
function importMemberBadges() {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.json';
    input.onchange = e => {
        const file = e.target.files[0];
        if (!file) return;
        const reader = new FileReader();
        reader.onload = ev => {
            try {
                const data = JSON.parse(ev.target.result);
                if (data.badges) {
                    state.memberBadges = data.badges;
                    safeSet('memberBadges', JSON.stringify(state.memberBadges));
                }
                if (data.auth) {
                    state.memberAuth = data.auth;
                    safeSet('memberAuth', JSON.stringify(state.memberAuth));
                }
                if (data.avatars) {
                    state.memberAvatars = data.avatars;
                    safeSet('memberAvatars', JSON.stringify(state.memberAvatars));
                }
                const total = Object.keys(state.memberBadges).length + Object.keys(state.memberAuth).length + Object.keys(state.memberAvatars).length;
                showToast(`已导入（${total} 项数据）`, 'success');
                renderMembers();
                refreshBadgeStats();
            } catch (err) {
                showToast('导入失败：' + err.message, 'error');
            }
        };
        reader.readAsText(file);
    };
    input.click();
}
function refreshBadgeStats() {
    const el = $('badgeStats');
    if (!el) return;
    const total = Object.keys(state.memberBadges).length;
    const authCount = Object.values(state.memberAuth).filter(v => v === true).length;
    const avatarCount = Object.keys(state.memberAvatars).length;
    if (total === 0 && authCount === 0 && avatarCount === 0) {
        el.innerHTML = '📊 当前未设置任何铭牌。点击成员右侧 ⚙ 按钮开始设置。';
    } else {
        el.innerHTML = `📊 铭牌 <b>${total}</b> · 认证 <b>${authCount}</b> · 自定义头像 <b>${avatarCount}</b>`;
    }
}

function renderMembers() {
    const q = ($('memberSearch').value || '').toLowerCase();
    // ← 修复：过滤掉无 user_id 的成员（异常数据），保证每个成员项可双击私聊
    const list = state.members.filter(m => (m.user_id || '').length > 0)
        .filter(m => !q || (m.nick_name || '').toLowerCase().includes(q) || (m.user_id || '').includes(q));
    if (!list.length) { $('memberList').innerHTML = '<div class="empty-state">无成员</div>'; return; }
    $('memberList').innerHTML = list.map(m => {
        const isOwner = m.user_id === state.groupOwnerUserId;
        const uid = escapeHtml(m.user_id);
        const b = state.memberBadges[m.user_id] || {};
        const avatarUrl = state.memberAvatars[m.user_id];
        const auth = state.memberAuth[m.user_id] === true;
        const displayName = b.nick || m.nick_name || '(无名)';
        let badges = '';
        // ← 自动 Bot 标识：ID 以 "bot" 开头的成员即为机器人
        const isBotByUid = (m.user_id || '').toLowerCase().startsWith('bot');
        // 系统默认铭牌（群主/元宝AI/机器人/成员）
        if (isOwner) badges += '<span class="member-badge admin">群主</span>';
        else if (isBotByUid || m.member_type === 3) badges += '<span class="member-badge bot">🤖 机器人</span>';
        else if (m.member_type === 2) badges += '<span class="member-badge ai">元宝AI</span>';
        else if (m.member_type === 1) badges += '<span class="member-badge">成员</span>';
        // 自定义铭牌 / 钻标（V4.6 恢复）
        if (b.type || b.text) {
            const txt = b.text || (BADGE_PRESETS[b.type] && BADGE_PRESETS[b.type].text) || '成员';
            badges += `<span class="custom-badge ${escapeHtml(b.type || 'member')}"><span class="badge-dot"></span>${escapeHtml(txt)}</span>`;
        }
        if (auth) badges += '<span class="auth-badge-vip">V</span>';
        // 头像（自定义头像优先）
        const initial = (displayName || '?').charAt(0).toUpperCase();
        const avatarHtml = avatarUrl
            ? `<img src="${avatarUrl}" alt="" style="width:100%;height:100%;object-fit:cover;border-radius:50%">`
            : escapeHtml(initial);
        return `<div class="member-item clickable" data-uid="${uid}" data-nick="${escapeHtml(displayName)}"
                 title="左键单击复制ID / 双击私聊 / 右键设置"
                 onclick="memberClick(event,this)" ondblclick="memberDblClick(event,this)" oncontextmenu="memberContextMenu(event,this)">
            <div class="member-avatar-wrap">${avatarHtml}</div>
            <div class="member-info">
                <div class="member-name">${escapeHtml(displayName)}${badges}</div>
                <div class="member-id">${uid}</div>
            </div>
        </div>`;
    }).join('');
    refreshBadgeStats();
}
function filterMembers() { renderMembers(); }
// 成员项交互：左键单击复制ID / 双击使用 / 右键设置（双击会先触发两次单击，用定时器防抖）
let _memberClickTimer = null;
function memberClick(e, el) {
    if (e.button !== 0) return;
    const uid = el.dataset.uid;
    if (!uid) return;
    clearTimeout(_memberClickTimer);
    _memberClickTimer = setTimeout(() => copyText(uid), 250);
}
function memberDblClick(e, el) {
    if (e.button !== 0) return;
    clearTimeout(_memberClickTimer);
    const uid = el.dataset.uid;
    // ← 修改：双击成员 → 弹出独立私聊窗口（不再是切到现有私聊模式）
    if (uid) openDmModal(uid, el.dataset.nick || '');
}
function memberContextMenu(e, el) {
    e.preventDefault();
    const uid = el.dataset.uid;
    if (uid) openBadgeEditor(uid);
}
// ← 修改：独立私聊窗口（双击成员弹出，不改变当前发送模式）
function openDmModal(uid, nick) {
    if (!uid) return;
    const nickEl = $('dmTargetNick');
    if (nickEl) nickEl.textContent = nick || '(未知)';
    const msg = $('dmMessage');
    if (msg) { msg.value = ''; setTimeout(() => msg.focus(), 50); }
    // 记录当前私聊目标
    _dmTarget = { user_id: uid, nick: nick || '' };
    const modal = $('dmModal');
    if (modal) modal.classList.add('active');
    // ← 修复：打开时重置签名（避免不同用户切换时误用上一人的缓存），再加载
    const hbox = $('dmHistory');
    if (hbox) { delete hbox.dataset.sig; delete hbox.dataset.loaded; }
    loadDmHistory(uid);
    // ← 修复：私聊窗口实时轮询（每 2 秒刷新历史），对方发来的新私聊自动出现
    startDmPolling(uid);
    // ← 修复：显示私聊高级选项（标题行常显，内容默认收起）
    const dmAdv = $('dmAdvOptions');
    if (dmAdv) dmAdv.style.display = '';
}
function closeDmModal() {
    const modal = $('dmModal');
    if (modal) modal.classList.remove('active');
    _dmTarget = null;
    stopDmPolling();
}

// ← 修复：私聊窗口实时轮询，对方新消息自动刷新（不重新加载整个列表，只更新）
let _dmPollTimer = null;
let _dmPollUid = '';
function startDmPolling(uid) {
    stopDmPolling();
    _dmPollUid = uid;
    _dmPollTimer = setInterval(() => { loadDmHistory(_dmPollUid); }, 2000);
}
function stopDmPolling() {
    if (_dmPollTimer) { clearInterval(_dmPollTimer); _dmPollTimer = null; }
    _dmPollUid = '';
}

// ← 补：加载私聊历史（/api/messages?c2c_user=uid，仿主页消息样式渲染）
// ← 修复：签名比对——无变化不重渲染（避免每 2 秒轮询闪烁），仅首次/有变化时刷新
async function loadDmHistory(uid) {
    const box = $('dmHistory');
    if (!box) return;
    const r = await api(`/api/messages?limit=100&c2c_user=${encodeURIComponent(uid)}`);
    if (!r || !r.messages) { if (!box.dataset.loaded) box.innerHTML = '<div class="empty-state">暂无历史消息</div>'; return; }
    const msgs = r.messages;
    const sig = msgs.map(m => (m.sender_id || '') + '|' + (m.content || '') + '|' + (m.time || '')).join('\n');
    if (sig === box.dataset.sig) { if (box.scrollTop === 0 || box.scrollTop + box.clientHeight >= box.scrollHeight - 40) box.scrollTop = box.scrollHeight; return; }
    box.dataset.sig = sig;
    box.dataset.loaded = '1';
    if (!msgs.length) { box.innerHTML = '<div class="empty-state">暂无与该用户的历史消息</div>'; return; }
    box.innerHTML = msgs.map(m => {
        const isMine = m.sender_id === state.botId;
        const who = isMine ? '我' : (m.sender_name || '对方');
        const t = m.time ? new Date(m.time).toLocaleString('zh-CN', { hour12: false }) : '';
        return `<div class="dm-msg ${isMine ? 'mine' : 'theirs'}">`
             + `<span class="dm-msg-meta">${escHtml(who)} ${t ? '· ' + escHtml(t) : ''}</span>`
             + `<span class="dm-msg-bubble">${escHtml(m.content || '')}</span></div>`;
    }).join('');
    box.scrollTop = box.scrollHeight;  // 滚动到底部（最新消息）
}
async function sendDmMessage() {
    const msg = $('dmMessage');
    const text = msg ? msg.value.trim() : '';
    if (!text) { showToast('请输入私聊内容', 'warning'); return; }
    if (!_dmTarget) { showToast('私聊目标无效', 'error'); return; }
    const body = {
        text,
        mode: 'dm',
        target_id: _dmTarget.user_id,
        target_nick: _dmTarget.nick,
    };
    // ← 补：私聊高级选项（刷屏次数 + 间隔），展开状态才生效
    const dmAdvBody = $('dmAdvOptionsBody');
    if (dmAdvBody && dmAdvBody.style.display !== 'none') {
        const cnt = parseInt($('dmAdvCount').value) || 1;
        if (cnt > 1) {
            body.count = cnt;
            body.interval = parseFloat($('dmAdvInterval').value) || 0.5;
        }
    }
    const r = await api('/api/send', { method: 'POST', body });
    if (r && r.ok) {
        showToast(`已私聊 ${_dmTarget.nick || _dmTarget.user_id}`, 'success');
        if (msg) msg.value = '';
        loadRecentMessages();
        // ← 补：发送成功后刷新私聊历史
        loadDmHistory(_dmTarget.user_id);
    } else {
        showToast((r && r.message) || '发送失败', 'error');
    }
}
let _dmTarget = null;
function copyText(text) {
    // ← 修复：非 HTTPS / localhost 环境下 navigator.clipboard 可能为 undefined，
    //   旧浏览器也不支持 Clipboard API，需降级到 execCommand('copy')。
    const fallback = () => {
        try {
            const ta = document.createElement('textarea');
            ta.value = text;
            ta.style.position = 'fixed';
            ta.style.opacity = '0';
            document.body.appendChild(ta);
            ta.select();
            const ok = document.execCommand('copy');
            document.body.removeChild(ta);
            showToast(ok ? '已复制' : '复制失败', ok ? 'success' : 'error');
        } catch (e) { showToast('复制失败', 'error'); }
    };
    if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(() => showToast('已复制', 'success')).catch(fallback);
    } else {
        fallback();
    }
}

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'})[c]);
}

// ── 引用回复 ──
async function sendReply() {
    const idx = parseInt($('replyIndex').value);
    const text = $('replyText').value.trim();
    if (isNaN(idx) || idx < 0) { showToast('请输入有效序号', 'warning'); return; }
    if (!text) { showToast('请输入回复内容', 'warning'); return; }
    const m = state.messages[idx];
    // ← 修复：优先回传后端 /api/messages 附带的 list_index（完整列表绝对位置），
    //   避免前端数组索引（只含尾部 limit 条）与后端完整列表索引不一致的问题
    const body = { text };
    if (m && typeof m.list_index === 'number') body.list_index = m.list_index;
    else body.index = idx;
    // ← 修复：附带被引用消息的 msg_id，供后端构造引用
    if (m && m.msg_id) body.ref_msg_id = m.msg_id;
    const at = $('replyAtUser').value.trim();
    if (at) { body.at_user = at; body.at_nickname = $('replyAtNick').value.trim(); }
    body.count = parseInt($('replyCount').value) || 1;
    // ← 修复：路径 /api/send-reply
    const r = await api('/api/send-reply', { method: 'POST', body });
    if (r && r.ok) { showToast('回复已发送', 'success'); $('replyText').value = ''; }
    else { showToast((r && r.message) || '回复失败', 'error'); }
}

// ── 回复模态框 ──
let _replyModalIndex = -1;

function openReplyModal(index) {
    _replyModalIndex = index;
    const m = state.messages[index];
    if (!m) return;
    $('modalMsgIndex').textContent = index + 1;
    $('modalMsgSender').textContent = m.sender_name || m.sender_id || '?';
    $('modalMsgContent').textContent = m.content || '(无文本内容)';
    $('modalReplyText').value = '';
    $('modalAtUser').value = '';
    $('modalAtNick').value = '';
    $('modalReplyCount').value = 1;
    $('replyModal').classList.add('active');
    // 聚焦输入框
    setTimeout(() => $('modalReplyText').focus(), 100);
}

function closeReplyModal() {
    $('replyModal').classList.remove('active');
    _replyModalIndex = -1;
}

async function sendModalReply() {
    const idx = _replyModalIndex;
    if (idx < 0 || idx >= state.messages.length) { showToast('消息序号无效', 'warning'); return; }
    const text = $('modalReplyText').value.trim();
    if (!text) { showToast('请输入回复内容', 'warning'); return; }
    // 同步到设置面板的字段，保持兼容
    $('replyIndex').value = idx;
    $('replyText').value = text;
    $('replyAtUser').value = $('modalAtUser').value.trim();
    $('replyAtNick').value = $('modalAtNick').value.trim();
    $('replyCount').value = parseInt($('modalReplyCount').value) || 1;
    closeReplyModal();
    // 调用现有的发送函数
    await sendReply();
}

// ── 图片上传发送 ──
// ← 修改：选中的图片先进入"待发送队列"并显示在消息内容框，点发送才真正发送
// ← 修复：严格区分图片/文件。仅当 MIME 为 image/* 或扩展名为常见图片格式时才按图片上传，
//   否则按文件处理（走 /api/send-file），避免"本来是文件的被当作图片上传"
function isImageFile(file) {
    if (file && file.type && file.type.startsWith('image/')) return true;
    const m = (file && file.name || '').match(/\.([^.]+)$/);
    if (!m) return false;
    return ['jpg', 'jpeg', 'png', 'gif', 'bmp', 'webp'].indexOf(m[1].toLowerCase()) >= 0;
}
function handleImageFileSelect(e) {
    const file = e.target.files[0];
    if (!file) return;
    // ← 修复：非图片文件改按文件入队（占位符显示 [文件:...]，发送走 /api/send-file）
    if (!isImageFile(file)) {
        queuePendingMedia({ type: 'file', file, name: file.name, size: file.size });
        $('imageFile').value = '';
        showToast('检测到非图片文件，已按文件方式待发送', 'info');
        return;
    }
    // 快捷按钮场景：入队 + 输入框占位（不立即发送）
    queuePendingMedia({ type: 'image', file, name: file.name, size: file.size });
    $('imageFile').value = '';
    return;
}
async function sendImage() {
    const file = $('imageFile').files[0];
    if (!file) { showToast('请先选择图片', 'warning'); return; }
    // ← 修复：非图片文件改走 /api/send-file，不当作图片上传
    if (!isImageFile(file)) {
        showToast('检测到非图片文件，已按文件方式发送', 'info');
        const fd0 = new FormData();
        fd0.append('file', file);
        const cg0 = (typeof state !== 'undefined' && state.currentGroup) || '';
        if (cg0) fd0.append('group', cg0);
        const at0 = $('imageAtUser').value.trim();
        if (at0) fd0.append('at_user', at0);
        const nick0 = $('imageAtNick').value.trim();
        if (nick0) fd0.append('at_nickname', nick0);
        try {
            const r0 = await fetch('/api/send-file', { method: 'POST', body: fd0 });
            const j0 = await r0.json();
            if (j0.ok) showToast('文件已发送', 'success');
            else showToast(j0.message || '发送失败', 'error');
        } catch (e0) { showToast('发送异常: ' + e0.message, 'error'); }
        return;
    }
    const fd = new FormData();
    fd.append('file', file);
    const curGroup = (typeof state !== 'undefined' && state.currentGroup) || '';
    if (curGroup) fd.append('group', curGroup);
    const at = $('imageAtUser').value.trim();
    if (at) fd.append('at_user', at);
    const nick = $('imageAtNick').value.trim();
    if (nick) fd.append('at_nickname', nick);
    try {
        // ← 修复：路径 /api/send-image
        const res = await fetch('/api/send-image', { method: 'POST', body: fd });
        const j = await res.json();
        if (j.ok) {
            showToast('图片已发送', 'success');
            // ← 补：发送成功后隐藏媒体面板（若由快捷按钮打开）
            const mc = $('mediaControls');
            if (mc && mc.style.display !== 'none') mc.style.display = 'none';
        }
        else showToast(j.message || '发送失败', 'error');
    } catch(e) { showToast('发送异常: ' + e.message, 'error'); }
}

// ── 文件上传发送 ──
// ← 修改：选中的文件先进入"待发送队列"并显示在消息内容框，点发送才真正发送
function handleFileSelect(e) {
    const file = e.target.files[0];
    if (!file) return;
    // 快捷按钮场景：入队 + 输入框占位（不立即发送）
    queuePendingMedia({ type: 'file', file, name: file.name, size: file.size });
    $('documentFile').value = '';
    return;
}
async function sendFile() {
    const file = $('documentFile').files[0];
    if (!file) { showToast('请先选择文件', 'warning'); return; }
    const fd = new FormData();
    fd.append('file', file);
    const curGroup = (typeof state !== 'undefined' && state.currentGroup) || '';
    if (curGroup) fd.append('group', curGroup);
    const at = $('fileAtUser').value.trim();
    if (at) fd.append('at_user', at);
    const nick = $('fileAtNick').value.trim();
    if (nick) fd.append('at_nickname', nick);
    try {
        // ← 修复：路径 /api/send-file
        const res = await fetch('/api/send-file', { method: 'POST', body: fd });
        const j = await res.json();
        if (j.ok) {
            showToast('文件已发送', 'success');
            // ← 补：发送成功后隐藏媒体面板（若由快捷按钮打开）
            const mc = $('mediaControls');
            if (mc && mc.style.display !== 'none') mc.style.display = 'none';
        }
        else showToast(j.message || '发送失败', 'error');
    } catch(e) { showToast('发送异常: ' + e.message, 'error'); }
}

// ── 视频发送（复用 /api/send-file 上传，视频以文件消息形式发送） ──
function handleVideoSelect(event) {
    const file = event.target.files[0];
    if (!file) return;
    $('videoFileLabel').classList.add('has-file');
    $('videoFileLabel').querySelector('.file-name').textContent = file.name;
    $('videoInfo').style.display = 'block';
    $('videoSize').textContent = (file.size / 1024).toFixed(1) + ' KB';
    $('videoType').textContent = file.type || '视频';
}
async function sendVideo() {
    const file = $('videoFile').files[0];
    if (!file) { showToast('请先选择视频', 'warning'); return; }
    if (file.size > 20 * 1024 * 1024) { showToast('视频超过 20MB 上限', 'error'); return; }
    const fd = new FormData();
    fd.append('file', file);
    const curGroup = (typeof state !== 'undefined' && state.currentGroup) || '';
    if (curGroup) fd.append('group', curGroup);
    const at = $('videoAtUser').value.trim();
    if (at) fd.append('at_user', at);
    const nick = $('videoAtNick').value.trim();
    if (nick) fd.append('at_nickname', nick);
    try {
        const res = await fetch('/api/send-file', { method: 'POST', body: fd });
        const j = await res.json();
        if (j.ok) showToast('视频已发送', 'success');
        else showToast(j.message || '发送失败', 'error');
    } catch(e) { showToast('发送异常: ' + e.message, 'error'); }
}

// ── 批量发送图片（V4.6）──
let _batchFiles = [];
let _batchCancelled = false;
const _BATCH_MAX = 50;

function handleBatchImageSelect(e) {
    const all = Array.from(e.target.files || []);
    // ← 修复：只接收图片文件，非图片文件直接过滤（避免"文件被当作图片上传"）
    const files = all.filter(isImageFile);
    if (files.length !== all.length) showToast(`已过滤 ${all.length - files.length} 个非图片文件`, 'warning');
    if (!files.length) { e.target.value = ''; return; }
    const room = _BATCH_MAX - _batchFiles.length;
    if (room <= 0) { showToast(`最多选择 ${_BATCH_MAX} 张`, 'warning'); }
    _batchFiles = _batchFiles.concat(files.slice(0, Math.max(0, room)));
    renderBatchPreview();
    e.target.value = '';
}

function renderBatchPreview() {
    const grid = $('batchPreview');
    grid.innerHTML = _batchFiles.map((f, i) => `
        <div class="batch-thumb">
            <img src="${URL.createObjectURL(f)}" alt="">
            <button type="button" class="batch-remove" onclick="removeBatchImage(${i})" title="移除">×</button>
            <span class="batch-name">${f.name}</span>
        </div>`).join('');
    $('batchMeta').style.display = _batchFiles.length ? 'block' : 'none';
    $('batchCount').textContent = `已选 ${_batchFiles.length} 张`;
}

function removeBatchImage(i) {
    _batchFiles.splice(i, 1);
    renderBatchPreview();
}

function populateBatchGroups() {
    const sel = $('batchGroup');
    if (!sel) return;
    const cur = (typeof state !== 'undefined' && state.currentGroup) || '';
    let html = '<option value="">当前群</option>';
    if (typeof state !== 'undefined' && Array.isArray(state.groups)) {
        html += state.groups.map(g => {
            const name = g.group_name || g.group_code;
            return `<option value="${g.group_code}">${name}</option>`;
        }).join('');
    }
    sel.innerHTML = html;
    sel.value = cur || '';
}

function delay(ms) { return new Promise(r => setTimeout(r, ms)); }

async function sendImageBatch() {
    if (!_batchFiles.length) { showToast('请先选择图片', 'warning'); return; }
    const interval = Math.max(200, parseInt($('batchInterval').value, 10) || 1000);
    const group = $('batchGroup').value.trim();
    const total = _batchFiles.length;
    _batchCancelled = false;

    const btn = $('btnBatchSend'), cancelBtn = $('btnBatchCancel');
    btn.disabled = true; btn.style.opacity = '.6';
    cancelBtn.style.display = 'inline-block';
    $('batchProgressWrap').style.display = 'block';
    $('batchBar').style.width = '0%';

    let done = 0, ok = 0, fail = 0;
    for (let i = 0; i < total; i++) {
        if (_batchCancelled) { showToast('已取消批量发送', 'warning'); break; }
        const file = _batchFiles[i];
        $('batchStatus').textContent = `发送中 ${i + 1}/${total}：${file.name}`;
        const fd = new FormData();
        fd.append('file', file);
        if (group) fd.append('group', group);
        try {
            const res = await fetch('/api/send-image', { method: 'POST', body: fd });
            const j = await res.json();
            if (j.ok) ok++; else { fail++; console.warn('批量发图失败:', j.message); }
        } catch (err) {
            fail++; console.warn('批量发图异常:', err);
        }
        done++;
        $('batchBar').style.width = Math.round(done / total * 100) + '%';
        $('batchStatus').textContent = `已发送 ${done}/${total}（成功 ${ok} / 失败 ${fail}）`;
        if (i < total - 1 && !_batchCancelled) await delay(interval);
    }
    btn.disabled = false; btn.style.opacity = '1';
    cancelBtn.style.display = 'none';
    showToast(`批量发送完成：成功 ${ok} / 失败 ${fail} / 共 ${total}`,
              (fail === 0 && ok === total) ? 'success' : 'warning');
    if (fail === 0 && ok === total) { _batchFiles = []; renderBatchPreview(); }
}

function cancelBatchSend() { _batchCancelled = true; }

// ── 搜索/过滤防抖（v4.0）──
function debounce(fn, wait) {
    let t;
    return function(...args) { clearTimeout(t); t = setTimeout(() => fn.apply(this, args), wait); };
}
const filterStickersDebounced = debounce(filterStickers, 200);
const filterMembersDebounced = debounce(filterMembers, 200);

// ── AI 生成图片 ──
async function sendAiImage() {
    const prompt = $('aiImagePrompt').value.trim();
    if (!prompt) { showToast('请输入提示词', 'warning'); return; }

    const btn = $('btnSendAiImage');
    const status = $('aiImageStatus');
    const statusText = $('aiImageStatusText');
    btn.disabled = true;
    btn.textContent = '⏳ 生成中...';
    status.style.display = 'flex';
    statusText.textContent = '正在发送图片生成请求...';

    try {
        const r = await api('/api/send/ai-image', { method: 'POST', body: { prompt } });
        if (r && r.ok) {
            showToast('图片已生成并发送到群聊', 'success');
            $('aiImagePrompt').value = '';
            status.style.display = 'none';
        } else {
            statusText.textContent = (r && r.message) || '生成失败';
            showToast((r && r.message) || '生成失败', 'error');
        }
    } catch (e) {
        statusText.textContent = '请求异常: ' + e.message;
        showToast('请求异常: ' + e.message, 'error');
    } finally {
        btn.disabled = false;
        btn.textContent = '✨ 生成并发送图片';
        setTimeout(() => { status.style.display = 'none'; }, 5000);
        }
    }
    
    // ── 设置面板 ──
async function saveSettings() {
    // ← 修复：字段名全部对齐后端 api_update_settings 的读取 key
    // 主群概念已移除：不再提交 default_group，默认目标群为监听列表第一项
    const body = {
        port: parseInt($('settingPort').value) || 8000,
        heartbeat_interval: parseFloat($('settingHeartbeatInterval').value) || 1.0,
        forward_mode_enabled: $('settingForwardMode').checked,
        forward_at_only: $('settingForwardAtOnly').checked,
        forward_at_yuanbao: $('settingForwardAtYuanbao').checked,
        msg_log_enabled: $('settingMsgLogEnabled').checked,
    };
    const r = await api('/api/settings', { method: 'POST', body });
    if (!r || !r.ok) { showToast((r && r.message) || '保存失败', 'error'); return; }
    // 大模型配置（走 /api/llm/config；api_key 留空则不修改）
    const llm = {
        api_url: $('settingLlmApiUrl').value.trim(),
        model: $('settingLlmModel').value.trim(),
        system_prompt: $('settingLlmSystemPrompt').value.trim(),
        max_tokens: parseInt($('settingLlmMaxTokens').value) || 200,
        temperature: parseFloat($('settingLlmTemperature').value),
        timeout_sec: parseInt($('settingLlmTimeout').value) || 15,
    };
    const lk = $('settingLlmApiKey').value.trim();
    if (lk) llm.api_key = lk;
    const lr = await api('/api/llm/config', { method: 'POST', body: llm });
    if (lr && lr.ok) {
        showToast('设置已保存', 'success');
        updateLlmStatus(!!lr.llm_enabled);
    } else {
        showToast((lr && lr.message) || '大模型配置保存失败', 'error');
    }
}
function updateLlmStatus(enabled) {
    const el = $('settingLlmEnabledStatus');
    if (!el) return;
    el.textContent = enabled ? '🟢 已启用' : '⚪ 未启用';
    el.style.color = enabled ? 'var(--success)' : 'var(--text-muted)';
}
async function testLlmConfig(btn) {
    if (!btn) btn = document.querySelector('.settings-section button[onclick="testLlmConfig(this)"]');
    const old = btn.textContent;
    btn.disabled = true;
    btn.textContent = '测试中...';
    try {
        const r = await api('/api/llm/test', { method: 'POST', body: { message: '你好，请简单介绍一下自己' } });
        if (r && r.ok) showToast('✅ ' + ((r.reply || '').slice(0, 60)), 'success');
        else showToast((r && r.message) || '连接失败', 'error');
    } catch (e) {
        showToast('请求异常: ' + e.message, 'error');
    } finally {
        btn.disabled = false;
        btn.textContent = old;
    }
}
async function loadSettings() {
    // ← 修复：GET /api/settings 返回 settings 字典
    const r = await api('/api/settings');
    if (!r) return;
    const s = r; // 直接是 settings 对象
    // 主群概念已移除：设置面板不再有默认群号输入框
    if (s.port) $('settingPort').value = s.port;
    if (s.heartbeat_interval) $('settingHeartbeatInterval').value = s.heartbeat_interval;
    if (s.forward_mode_enabled !== undefined) {
        $('settingForwardMode').checked = s.forward_mode_enabled;
        state.forwardMode = s.forward_mode_enabled;
    }
    if (s.forward_at_only !== undefined) $('settingForwardAtOnly').checked = s.forward_at_only;
    if (s.forward_at_yuanbao !== undefined) $('settingForwardAtYuanbao').checked = s.forward_at_yuanbao;
    if (s.msg_log_enabled !== undefined) {
        state.msgLogEnabled = s.msg_log_enabled;
        syncMsgLogToggles();
    }
    updateWorkflowDesc();
    // 大模型配置回填
    const llm = await api('/api/llm/config');
    if (llm) {
        $('settingLlmApiUrl').value = llm.api_url || '';
        $('settingLlmModel').value = llm.model || '';
        $('settingLlmSystemPrompt').value = llm.system_prompt || '';
        $('settingLlmMaxTokens').value = llm.max_tokens || 200;
        $('settingLlmTemperature').value = (llm.temperature !== undefined && llm.temperature !== null) ? llm.temperature : 0.8;
        $('settingLlmTimeout').value = llm.timeout_sec || 15;
        $('settingLlmApiKey').value = '';
        $('settingLlmApiKey').placeholder = llm.api_key ? ('已配置 ' + llm.api_key + '，留空则不修改') : '未配置';
        updateLlmStatus(!!llm.enabled);
    }
}

// ── 消息记录开关同步 ──
function syncMsgLogToggles() {
    $('settingMsgLogEnabled').checked = state.msgLogEnabled;
    const statusEl = $('msgLogStatusText');
    if (statusEl) {
        statusEl.textContent = state.msgLogEnabled ? '● 记录中' : '○ 已暂停';
        statusEl.style.color = state.msgLogEnabled ? 'var(--success)' : 'var(--warning)';
    }
}
async function onMsgLogToggle(enabled) {
    const r = await api('/api/settings', { method: 'POST', body: { msg_log_enabled: enabled } });
    if (r && r.ok) {
        state.msgLogEnabled = enabled;
        syncMsgLogToggles();
        showToast(enabled ? '已开启消息记录' : '已暂停消息记录', enabled ? 'success' : 'warning');
    } else {
        // 回滚
        $('settingMsgLogEnabled').checked = state.msgLogEnabled;
        showToast('切换失败', 'error');
    }
}

// ── 撤回监听切换 ──
function syncRecallMonitorToggles() {
    const cb = $('recallMonitorToggle');
    if (cb) cb.checked = state.recallMonitorEnabled;
    const statusEl = $('recallMonitorStatusText');
    if (statusEl) {
        statusEl.textContent = state.recallMonitorEnabled ? '● 监听中' : '○ 未启用';
        statusEl.style.color = state.recallMonitorEnabled ? 'var(--success)' : 'var(--warning)';
    }
}
async function onRecallMonitorToggle(enabled) {
    const r = await api('/api/settings', { method: 'POST', body: { recall_monitor_enabled: enabled } });
    if (r && r.ok) {
        state.recallMonitorEnabled = enabled;
        syncRecallMonitorToggles();
        showToast(enabled ? '已开启撤回监听' : '已关闭撤回监听', enabled ? 'success' : 'warning');
    } else {
        // 回滚
        const cb = $('recallMonitorToggle');
        if (cb) cb.checked = state.recallMonitorEnabled;
        showToast('切换失败', 'error');
    }
}

// ── 代理模式 ──
function onForwardModeToggle(el) { state.forwardMode = el.checked; updateWorkflowDesc(); }
async function enableForwardMode() {
    const atOnly = $('settingForwardAtOnly').checked;
    const atYb = $('settingForwardAtYuanbao').checked;
    // ← 修复：路径 /api/forward-mode/enable
    const r = await api('/api/forward-mode/enable', { method: 'POST', body: { at_only: atOnly, forward_at_yuanbao: atYb } });
    if (r && r.ok) { state.forwardMode = true; showToast('代理模式已启用', 'success'); loadSettings(); }
    else { showToast((r && r.message) || '启用失败', 'error'); }
}
async function disableForwardMode() {
    const r = await api('/api/forward-mode/disable', { method: 'POST' });
    if (r && r.ok) { state.forwardMode = false; showToast('代理模式已禁用', 'warning'); loadSettings(); }
    else { showToast((r && r.message) || '禁用失败', 'error'); }
}
async function updateForwardAtOnly(val) {
    // ← 修复：路径 /api/forward-mode/enable（toggle 用 enable 重新配置）
    await api('/api/forward-mode/enable', { method: 'POST', body: { at_only: val, forward_at_yuanbao: $('settingForwardAtYuanbao').checked } });
}
async function updateForwardAtYuanbao(val) {
    // ← 修复：路径 /api/forward-mode/toggle-at-yuanbao
    await api('/api/forward-mode/toggle-at-yuanbao', { method: 'POST', body: { enabled: val } });
    updateWorkflowDesc();
}
async function getForwardModeConfig() {
    // ← 修复：路径 /api/forward-mode/config
    const r = await api('/api/forward-mode/config');
    if (r && r.ok) {
        const info = `代理:${r.enabled?'开':'关'} @元宝:${r.forward_at_yuanbao?'开':'关'} 仅@:${r.at_only?'是':'否'} 队列:${r.queue_length||0}`;
        showToast(info, '');
    }
}
async function clearForwardQueue() {
    // ← 修复：路径 /api/forward-mode/clear-queue
    const r = await api('/api/forward-mode/clear-queue', { method: 'POST' });
    if (r && r.ok) { showToast('队列已清空', 'success'); }
    else { showToast((r && r.message) || '清空失败', 'error'); }
}
function updateWorkflowDesc() {
    const atYb = $('settingForwardAtYuanbao').checked;
    const desc = $('workflowDesc');
    if (!desc) return;
    if (atYb) {
        desc.innerHTML = '<div><strong>①</strong> 源群消息 → 实时转发到中转群@元宝</div><div><strong>②</strong> 元宝回复 → 自动转发回原群</div>';
    } else {
        desc.innerHTML = '<div><strong>①</strong> 源群消息 → 直接转发到中转群</div><div><strong>②</strong> 实时完成，不等回复</div>';
    }
}

// ── 心跳 ──
async function setHeartbeatInterval() {
    const v = parseFloat($('settingHeartbeatInterval').value) || 1.0;
    const r = await api('/api/heartbeat/interval', { method: 'POST', body: { interval: v } });
    if (r && r.ok) { showToast(`心跳间隔: ${v}s`, 'success'); }
    else { showToast((r && r.message) || '设置失败', 'error'); }
}
async function checkHeartbeat() {
    // ← 修复：路径 /api/heartbeat (GET)
    const r = await api('/api/heartbeat');
    if (r && r.ok) { showToast(r.connected ? '心跳正常' : '已连接但心跳未确认', 'success'); }
    else { showToast('心跳异常', 'error'); }
}

// ── 群聊管理（多群聊监听）──
async function loadGroups() {
    // ← 修复：后端 /api/groups 返回 { groups, current_group }，没有 ok 字段
    const r = await api('/api/groups');
    if (r && r.groups) {
        state.groups = r.groups;
        // 预填充群名缓存
        r.groups.forEach(g => { if (g.group_name) state.groupNameCache[g.group_code] = g.group_name; });
        // ← 主群概念已移除：仅在尚未选择群时用默认目标群（监听列表第一项）初始化，
        //   已选择（用户点击过切换）时不覆盖
        if (r.current_group && !state.currentGroup) state.currentGroup = r.current_group;
        populateBatchGroups();
        // 群名去重：同名群附加群号，避免"两个一样的群"无法区分
        const nameCount = {};
        r.groups.forEach(g => { const n = g.group_name || g.group_code; nameCount[n] = (nameCount[n] || 0) + 1; });
        const gname = g => {
            const n = g.group_name || g.group_code;
            return nameCount[n] > 1 ? `${n}（${g.group_code}）` : n;
        };
        const gl = $('groupsList');
        if (gl) {
            // 多群监听：每项显示监听开关（👁 正在监听 / 空心未监听），点击行切换查看的群
            gl.innerHTML = r.groups.map(g => {
                const active = g.group_code === state.currentGroup;
                const listening = g.listening === true;
                const meta = g.last_message || (g.message_count ? `${g.message_count} 条消息` : '');
                return `<div class="member-item group-item${active ? ' active' : ''}${listening ? ' listening' : ''}" data-group="${escapeHtml(g.group_code)}" onclick="switchGroupByCode('${escapeHtml(g.group_code)}')">
                    <div class="member-info">
                        <div class="member-name">${active ? '🎧 ' : ''}${escapeHtml(gname(g))}</div>
                        <div class="member-id">${escapeHtml(meta)}</div>
                    </div>
                    <button class="listen-toggle${listening ? ' on' : ''}" title="${listening ? '正在监听（点击停止）' : '未监听（点击开始监听）'}" onclick="event.stopPropagation();toggleGroupListen('${escapeHtml(g.group_code)}')">${listening ? '👁' : '👁‍🗨'}</button>
                </div>`;
            }).join('') || '<div class="empty-state">暂无</div>';
        }
    }
}
// 多群监听：单独开/关某个群的监听（主群概念已移除，任何群都可开关）
async function toggleGroupListen(code) {
    if (!code) return;
    const g = (state.groups || []).find(x => x.group_code === code);
    const nowListening = g ? g.listening === true : false;
    const next = !nowListening;
    const r = await api('/api/groups/listen', { method: 'POST', body: { group_code: code, listen: next } });
    if (r && r.ok) {
        showToast(next ? '已开始监听该群' : '已停止监听该群', 'success');
        loadGroups();
        loadRecentMessages();
    } else {
        showToast((r && r.message) || '操作失败', 'error');
        loadGroups();
    }
}
function switchGroupByCode(code) {
    if (!code) return;
    state.currentGroup = code;
    // ← 修复：切换群时立即清空旧群成员并显示加载中，
    //   避免新群成员未返回前候选框/成员面板显示上一个群的成员
    state.members = [];
    state.membersGroup = '';
    const ml = $('memberList');
    if (ml) ml.innerHTML = '<div class="empty-state">成员加载中...</div>';
    const badge = $('memberCountBadge');
    if (badge) badge.textContent = '';
    api('/api/groups/switch', { method: 'POST', body: { group_code: code } }).then(r => {
        if (r && r.ok) {
            showToast('已切换群', 'success');
            // ← 切换派后刷新所有面板：群列表 / 消息 / 成员
            loadGroups();
            loadRecentMessages();
            loadMembers();
        } else {
            showToast((r && r.message) || '切换失败', 'error');
        }
    });
}

// ── 插件生态（v4.4）──
// 插件只需注册"页面"与"卡片"元数据，这里用系统统一 settings-section 样式渲染，
// 自动继承 CSS 变量 / 深色浅色 / 厂商效果主题，插件无法自定义 CSS。
let pluginCardData = [];  // 当前渲染的卡片数据（用于操作按钮取表单字段）

async function loadPlugins() {
    const box = $('pluginListBox');
    if (!box) return;
    const r = await api('/api/plugins');
    if (!r || !r.ok) { box.innerHTML = '<div class="empty-state">加载插件列表失败</div>'; return; }
    const plugins = r.plugins || [];
    const cnt = $('pluginCount');
    if (cnt) cnt.textContent = `(${plugins.filter(p => p.active).length}/${plugins.length} 运行中)`;
    if (!plugins.length) {
        box.innerHTML = '<div class="empty-state">还没有安装插件，在上方粘贴 Git 仓库地址即可安装</div>';
        renderPluginPages([]);
        return;
    }
    box.innerHTML = plugins.map(p => {
        const running = p.active;
        const disabled = p.error === '已禁用';
        const statusCls = running ? 'status-ok' : (disabled ? 'status-off' : 'status-err');
        const statusTxt = running ? '运行中' : (disabled ? '已禁用' : '未运行');
        const errHtml = p.error && !disabled
            ? `<div class="setting-hint" style="color:#ff6b6b;margin-top:6px">⚠ ${escapeHtml(p.error)}</div>` : '';
        return `<div class="plugin-card">
            <div style="flex:1;min-width:0">
                <div style="display:flex;align-items:center;gap:8px;flex-wrap:wrap">
                    <strong>${escapeHtml(p.name)}</strong>
                    <span style="color:var(--text-dim);font-size:12px">v${escapeHtml(p.version)}</span>
                    ${p.author ? `<span style="color:var(--text-dim);font-size:12px">by ${escapeHtml(p.author)}</span>` : ''}
                    <span class="status-badge ${statusCls}">● ${statusTxt}</span>
                </div>
                ${p.description ? `<div class="setting-hint" style="margin-top:4px">${escapeHtml(p.description)}</div>` : ''}
                <div style="color:var(--text-dim);font-size:12px;margin-top:4px">消息处理器 ${p.message_handlers} 个 · 路由 ${p.routes} 条 · 页面 ${(p.pages || []).length} 个 · 卡片 ${(p.cards || []).length} 张</div>
                ${errHtml}
            </div>
            <div style="display:flex;gap:6px;flex-wrap:wrap;align-items:center">
                <button class="btn btn-small ${running ? 'btn-outline' : 'btn-success'}" onclick="togglePlugin('${escapeHtml(p.name)}', ${!running})">${running ? '⏸ 禁用' : '▶ 启用'}</button>
                <button class="btn btn-small btn-outline" title="重新加载" onclick="reloadPlugin('${escapeHtml(p.name)}')">🔄</button>
            </div>
        </div>`;
    }).join('');
    // 渲染插件页面（Tab）与统一格式卡片
    renderPluginPages(plugins);
}

async function installPlugin() {
    const urlInput = $('pluginInstallUrl');
    const url = urlInput.value.trim();
    if (!url) { showToast('请输入 Git 仓库地址', 'error'); return; }
    showToast('正在克隆并加载插件...');
    const r = await api('/api/plugins/install', { method: 'POST', body: { url } });
    if (r && r.ok) {
        showToast(`插件 ${r.plugin} 安装成功`, 'success');
        urlInput.value = '';
        await loadPlugins();
    } else {
        showToast((r && r.error) || '安装失败', 'error');
    }
}

async function togglePlugin(name, enabled) {
    const r = await api('/api/plugins/toggle', { method: 'POST', body: { name, enabled } });
    if (r && r.ok) {
        showToast(enabled ? `插件 ${name} 已启用` : `插件 ${name} 已禁用`, 'success');
        await loadPlugins();
    } else {
        showToast((r && r.error) || '操作失败', 'error');
    }
}

async function reloadPlugin(name) {
    showToast(`正在重新加载 ${name} ...`);
    const r = await api('/api/plugins/reload', { method: 'POST', body: { name } });
    if (r && r.ok) {
        showToast(`插件 ${name} 已重载`, 'success');
    } else {
        showToast((r && r.error) || '重载失败', 'error');
    }
    await loadPlugins();
}

// 插件前端界面（元数据驱动 · 统一格式）
function renderPluginPages(plugins) {
    // 1. 移除旧的动态 tab / 页面 / 卡片
    document.querySelectorAll('.tab-item[data-plugin-tab="1"]').forEach(el => el.remove());
    document.querySelectorAll('.tab-page[data-plugin-page="1"]').forEach(el => el.remove());
    document.querySelectorAll('.settings-section[data-plugin-card="1"]').forEach(el => el.remove());
    pluginCardData = [];

    // 2. 收集运行中插件的页面与卡片，按权重（小→大）排序
    const pages = [];
    const cards = [];
    (plugins || []).forEach(p => {
        if (!p.active) return;
        (p.pages || []).forEach(pg => pages.push({
            id: pg.id, title: pg.title || p.name, icon: pg.icon || '🧩',
            weight: Number(pg.weight) || 0, plugin: p.name,
        }));
        (p.cards || []).forEach(c => cards.push({
            page: c.page, title: c.title, icon: c.icon || '📋',
            weight: Number(c.weight) || 0, description: c.description || '',
            rows: c.rows || [], actions: c.actions || [], fields: c.fields || [],
            refresh: Number(c.refresh) || 0, plugin: p.name,
        }));
    });
    pages.sort((a, b) => a.weight - b.weight || a.title.localeCompare(b.title, 'zh'));
    cards.sort((a, b) => a.weight - b.weight || a.title.localeCompare(b.title, 'zh'));

    // 3. 在 content 末尾追加页面容器（侧边栏已删除，插件页面在设置页内以卡片区形式展示）
    const content = document.getElementById('content');
    pages.forEach(pg => {
        const page = document.createElement('div');
        page.className = 'tab-page';
        page.id = pg.id;
        page.dataset.pluginPage = '1';
        content.appendChild(page);
    });

    // 4. 把卡片渲染到各自的目标页面（系统页面或插件页面）
    cards.forEach((c, ci) => {
        pluginCardData[ci] = c;  // 必须先记录，供操作按钮取字段；即使目标页不存在也保持索引一致
        const target = document.getElementById(c.page);
        if (!target) return;  // 目标页不存在（如插件被禁用时）则跳过
        const card = document.createElement('div');
        card.className = 'settings-section';
        card.dataset.pluginCard = '1';

        // 头部（标题 + 说明）
        let html = `<div class="section-title" style="margin-bottom:10px">${escapeHtml(c.icon)} ${escapeHtml(c.title)}</div>`;
        if (c.description) {
            html += `<div class="setting-hint" style="margin-bottom:8px">${escapeHtml(c.description)}</div>`;
        }
        // 数据行（静态 / 动态加载）
        if (c.rows.length) {
            html += `<div class="plugin-card-rows">`;
            c.rows.forEach((row, ri) => {
                const rowId = `plugin-row-${ci}-${ri}`;
                if (row.route) {
                    html += `<div class="plugin-row"><span class="plugin-row-label">${escapeHtml(row.label)}</span>` +
                            `<span class="plugin-row-value" id="${rowId}" data-row-route="${escapeHtml(row.route)}" data-row-refresh="${c.refresh}">…</span></div>`;
                } else {
                    html += `<div class="plugin-row"><span class="plugin-row-label">${escapeHtml(row.label)}</span>` +
                            `<span class="plugin-row-value">${escapeHtml(row.value)}</span></div>`;
                }
            });
            html += `</div>`;
        }
        // 表单字段
        if (c.fields.length) {
            html += `<div style="margin-top:10px">`;
            c.fields.forEach(f => {
                const ph = f.placeholder || '';
                html += `<div class="form-group" style="margin-bottom:8px">
                    <label class="form-label">${escapeHtml(f.label || f.name)}</label>
                    <input type="${escapeHtml(f.type || 'text')}" class="form-input"
                           data-field-name="${escapeHtml(f.name)}" placeholder="${escapeHtml(ph)}"
                           value="${escapeHtml(f.default || '')}">
                </div>`;
            });
            html += `</div>`;
        }
        // 操作按钮
        if (c.actions.length) {
            html += `<div class="plugin-card-actions" style="margin-top:10px">`;
            c.actions.forEach((a, ai) => {
                html += `<button class="btn btn-primary btn-small" data-card-idx="${ci}" data-action-idx="${ai}">${escapeHtml(a.text || '执行')}</button>`;
            });
            html += `</div>`;
        }
        card.innerHTML = html;
        target.appendChild(card);
    });

    // 5. 绑定操作按钮
    document.querySelectorAll('button[data-action-idx]').forEach(btn => {
        btn.onclick = () => runPluginAction(Number(btn.dataset.cardIdx), Number(btn.dataset.actionIdx));
    });
    // 6. 加载动态数据行
    document.querySelectorAll('[data-row-route]').forEach(el => loadPluginRow(el));

    // 7. 若当前激活的是插件 Tab，重建后恢复激活态
    const activeTab = document.querySelector('.tab-item.active');
    if (activeTab && activeTab.dataset.pluginTab === '1') {
        switchTab(activeTab.dataset.tab);
    }
    if (window.innerWidth >= 900) updateTabIndicator(activeTab ? activeTab.dataset.tab : 'tab-messages');
}

// 加载卡片动态数据行（route 返回 {"value": ...}），支持定时自动刷新
async function loadPluginRow(el) {
    if (!el.isConnected) return;
    try {
        const r = await api(el.dataset.rowRoute);
        if (r && r.value !== undefined) el.textContent = r.value;
    } catch (e) { /* 静默失败，保留旧值 */ }
    if (!el.isConnected) return;
    const sec = Number(el.dataset.rowRefresh) || 0;
    if (sec > 0) setTimeout(() => loadPluginRow(el), sec * 1000);
}

// 执行卡片操作按钮：收集本卡片表单字段值 + 配置 body，POST 到指定路由
async function runPluginAction(cardIdx, actionIdx) {
    const card = pluginCardData[cardIdx];
    if (!card || !card.actions[actionIdx]) return;
    const action = card.actions[actionIdx];
    // 从被点击的按钮向上找到所属卡片，避免误选同插件的其他卡片
    const btnEl = document.querySelector(`button[data-card-idx="${cardIdx}"][data-action-idx="${actionIdx}"]`);
    const el = btnEl ? btnEl.closest('.settings-section[data-plugin-card="1"]') : null;
    if (!el) return;
    // 收集表单字段
    const body = Object.assign({}, action.body || {});
    el.querySelectorAll('[data-field-name]').forEach(inp => {
        body[inp.dataset.fieldName] = inp.value.trim();
    });
    // 前置校验：必填字段留空提示
    for (const f of card.fields || []) {
        if (f.required && !body[f.name]) {
            showToast(`请填写：${f.label || f.name}`, 'error');
            return;
        }
    }
    const btn = el.querySelector(`button[data-action-idx="${actionIdx}"]`);
    const old = btn ? btn.textContent : '';
    if (btn) { btn.disabled = true; btn.textContent = '处理中...'; }
    try {
        const r = await api(action.route, { method: action.method || 'POST', body });
        if (r && r.ok) {
            showToast(r.message || r.msg || '操作成功', 'success');
        } else {
            showToast((r && (r.message || r.error)) || '操作失败', 'error');
        }
        // 成功后刷新本卡片上的动态行
        el.querySelectorAll('[data-row-route]').forEach(rowEl => loadPluginRow(rowEl));
    } catch (e) {
        showToast('请求异常: ' + e.message, 'error');
    } finally {
        if (btn) { btn.disabled = false; btn.textContent = old; }
    }
}

async function init() {
    await loadSettings();
    await loadStickers();
    // 插件生态（v4.4）：加载插件列表并渲染动态页面/卡片
    try { await loadPlugins(); } catch (e) { console.error('[插件] 初始化失败', e); }
    // 恢复背景设置（v4.0）
    applyBackground();
    // 恢复 QQ 主题（v5.0）
    applyQQTheme();
    renderThemeGrid();
    // 初始化 LaTeX 编辑器（v4.1Pro）
    initLatexControls();
    // 同步黑夜模式开关
    const dmCb = $('settingDarkMode');
    if (dmCb) dmCb.checked = state.theme === 'dark';
    await connect();
    loadGroups();
    loadMembers();
}
init();
'use strict';

/* ── 共享工具：把单个设置项同步到后端 /api/settings（后端化个性化） ── */
function _persistPref(key, value) {
    try { safeSet(key, typeof value === 'string' ? value : JSON.stringify(value)); } catch (e) {}
    fetch('/api/settings', { method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ key, value }) }).catch(() => {});
}
function _loadServerPref(key, fallback) {
    fetch('/api/settings?key=' + encodeURIComponent(key))
        .then(r => r.json()).then(j => {
            if (j && j.ok && typeof j.value !== 'undefined' && j.value !== null) {
                try {
                    const v = (typeof j.value === 'string') ? JSON.parse(j.value) : j.value;
                    if (typeof window._applyServerPref === 'function') window._applyServerPref(key, v);
                } catch (e) {
                    if (typeof window._applyServerPref === 'function') window._applyServerPref(key, j.value);
                }
            }
        }).catch(() => {});
}
window._persistPref = _persistPref;
window._loadServerPref = _loadServerPref;

/* ═══════════════════════════════════════
   🖼️ 图片编辑器（裁剪 / 旋转 / 涂鸦 / 颜色滤镜）
   ═══════════════════════════════════════ */
const ImgEditor = {
    open(file, onDone) {
        this.onDone = onDone;
        const reader = new FileReader();
        reader.onload = () => {
            const img = new Image();
            img.onload = () => {
                this.img = img; this.rotation = 0; this.crop = null; this.doodle = [];
                this.curColor = '#FF3B30'; this.curTool = 'none'; this.filter = 'none';
                this._renderModal();
            };
            img.src = reader.result;
        };
        reader.readAsDataURL(file);
    },
    _renderModal() {
        let ov = $('imgEditorModal');
        if (!ov) {
            ov = document.createElement('div'); ov.id = 'imgEditorModal'; ov.className = 'modal-overlay';
            ov.innerHTML = `<div class="modal-box" style="max-width:680px">
                <div class="modal-header"><span class="modal-title">🖼️ 图片在线编辑</span><button class="modal-close" onclick="ImgEditor.close()">&times;</button></div>
                <div style="display:flex;gap:10px;margin-bottom:10px;flex-wrap:wrap">
                    <button class="chip" onclick="ImgEditor.setTool('crop')">✂️ 裁剪</button>
                    <button class="chip" onclick="ImgEditor.rotate(-90)">↺ 旋转</button>
                    <button class="chip" onclick="ImgEditor.rotate(90)">↻ 旋转</button>
                    <button class="chip" onclick="ImgEditor.setTool('doodle')">✏️ 涂鸦</button>
                    <input type="color" id="doodleColor" value="#FF3B30" onchange="ImgEditor.curColor=this.value" style="width:34px;height:28px;border:1px solid var(--border-soft);border-radius:4px">
                    <button class="chip" data-filter="none" onclick="ImgEditor.setFilter('none')">原色</button>
                    <button class="chip" data-filter="grayscale" onclick="ImgEditor.setFilter('grayscale')">⚫ 黑白</button>
                    <button class="chip" data-filter="sepia" onclick="ImgEditor.setFilter('sepia')">🟫 复古</button>
                    <button class="chip" data-filter="invert" onclick="ImgEditor.setFilter('invert')">🌀 反色</button>
                    <button class="chip" data-filter="blur" onclick="ImgEditor.setFilter('blur')">💫 模糊</button>
                </div>
                <div style="position:relative;overflow:auto;max-height:60vh;background:var(--input-bg);border-radius:8px">
                    <canvas id="imgEditorCanvas" style="display:block;cursor:crosshair"></canvas>
                </div>
                <div class="modal-actions" style="margin-top:12px">
                    <button class="btn btn-outline" onclick="ImgEditor.undo()">↶ 撤销</button>
                    <button class="btn btn-outline" onclick="ImgEditor.close()">取消</button>
                    <button class="btn btn-primary" onclick="ImgEditor.confirm()">✅ 完成</button>
                </div>
            </div>`;
            document.body.appendChild(ov);
        }
        ov.classList.add('active');
        this._redraw();
    },
    setTool(t) { this.curTool = t; showToast('工具：' + t, 'info'); },
    setFilter(f) {
        this.filter = f;
        document.querySelectorAll('[data-filter]').forEach(c => c.classList.toggle('active', c.dataset.filter === f));
        this._redraw();
    },
    rotate(deg) { this.rotation = (this.rotation + deg) % 360; this._redraw(); },
    undo() { this.doodle.pop(); this._redraw(); },
    close() { const ov = $('imgEditorModal'); if (ov) ov.classList.remove('active'); this.img = null; },
    _redraw() {
        if (!this.img) return;
        const c = $('imgEditorCanvas'); if (!c) return;
        const ang = this.rotation * Math.PI / 180;
        const w0 = this.img.width, h0 = this.img.height;
        const w = Math.abs(w0 * Math.cos(ang)) + Math.abs(h0 * Math.sin(ang));
        const h = Math.abs(w0 * Math.sin(ang)) + Math.abs(h0 * Math.cos(ang));
        const max = 600;
        const r = Math.min(max / w, max / h, 1);
        c.width = w * r; c.height = h * r;
        const ctx = c.getContext('2d');
        ctx.clearRect(0, 0, c.width, c.height);
        ctx.save();
        ctx.translate(c.width / 2, c.height / 2);
        ctx.rotate(ang);
        ctx.filter = this.filter === 'none' ? 'none'
            : this.filter === 'grayscale' ? 'grayscale(1)'
            : this.filter === 'sepia' ? 'sepia(1)'
            : this.filter === 'invert' ? 'invert(1)'
            : this.filter === 'blur' ? 'blur(4px)' : 'none';
        ctx.drawImage(this.img, -w0 * r / 2, -h0 * r / 2, w0 * r, h0 * r);
        ctx.filter = 'none';
        // 涂鸦
        for (const path of this.doodle) {
            ctx.strokeStyle = path.color; ctx.lineWidth = path.w; ctx.lineCap = 'round'; ctx.lineJoin = 'round';
            ctx.beginPath();
            path.points.forEach((p, i) => i === 0 ? ctx.moveTo(p[0], p[1]) : ctx.lineTo(p[0], p[1]));
            ctx.stroke();
        }
        ctx.restore();
        // 涂鸦事件
        c.onpointerdown = (e) => {
            const rect = c.getBoundingClientRect();
            const x = (e.clientX - rect.left) * (c.width / rect.width);
            const y = (e.clientY - rect.top) * (c.height / rect.height);
            if (this.curTool === 'doodle') {
                const path = { color: this.curColor, w: 3, points: [[x, y]] };
                this.doodle.push(path);
                const move = (ev) => {
                    const xx = (ev.clientX - rect.left) * (c.width / rect.width);
                    const yy = (ev.clientY - rect.top) * (c.height / rect.height);
                    path.points.push([xx, yy]);
                    // 重绘当前路径
                    ctx.save(); ctx.translate(c.width / 2, c.height / 2); ctx.rotate(ang);
                    ctx.strokeStyle = path.color; ctx.lineWidth = path.w; ctx.lineCap = 'round';
                    ctx.beginPath();
                    path.points.forEach((p, i) => i === 0 ? ctx.moveTo(p[0], p[1]) : ctx.lineTo(p[0], p[1]));
                    ctx.stroke(); ctx.restore();
                };
                const up = () => { c.removeEventListener('pointermove', move); c.removeEventListener('pointerup', up); };
                c.addEventListener('pointermove', move); c.addEventListener('pointerup', up);
            }
        };
    },
    confirm() {
        if (!this.img) return;
        const c = $('imgEditorCanvas');
        // 输出 PNG
        c.toBlob((blob) => {
            const file = new File([blob], 'edited.png', { type: 'image/png' });
            this.close();
            if (this.onDone) this.onDone(file);
        }, 'image/png', 0.92);
    }
};
window.ImgEditor = ImgEditor;

// 拦截 handleCustomBgSelect：上传前先经过图片编辑器
(function () {
    const _orig = window.handleCustomBgSelect;
    window.handleCustomBgSelect = function (e) {
        const file = (e.target.files && e.target.files[0]); if (!file) return;
        if (!file.type.startsWith('image/')) return _orig.call(this, e);
        ImgEditor.open(file, (edited) => {
            // ← 兼容性：旧浏览器无 DataTransfer 构造器时退化为直接使用原文件
            if (typeof DataTransfer === 'function') {
                const dt = new DataTransfer(); dt.items.add(edited);
                const fakeEv = { target: { files: dt.files } };
                _orig.call(this, fakeEv);
                showToast('已应用编辑后的图片', 'success');
            } else {
                _orig.call(this, { target: { files: [edited] } });
                showToast('已应用编辑后的图片', 'success');
            }
        });
        e.target.value = '';
    };
})();

/* ═══════════════════════════════════════
   🔌 后端化设置：开机时尝试从 /api/settings 拉取个性化
   ═══════════════════════════════════════ */
window._applyServerPref = function (key, value) {
    try {
        if (key === 'theme_qq') {
            if (typeof setQQTheme === 'function' && value && value !== state.theme_qq) setQQTheme(value);
        } else if (key === 'theme') {
            if (typeof setDarkMode === 'function' && ((value === 'dark') !== (state.theme === 'dark'))) setDarkMode(value === 'dark');
        } else if (key === 'backgroundMode') {
            if (typeof setBackgroundMode === 'function' && value !== state.backgroundMode) setBackgroundMode(value);
        } else if (key === 'customTheme') {
            state.customTheme = value; safeSet('customTheme', JSON.stringify(value));
            if (typeof applyCustomGradientVars === 'function') applyCustomGradientVars();
        }
    } catch (e) { console.warn('[applyServerPref]', key, e); }
};
document.addEventListener('DOMContentLoaded', () => {
    ['theme_qq','theme','backgroundMode','customTheme'].forEach(k => _loadServerPref(k));
});
