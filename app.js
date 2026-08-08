'use strict';

// ── 按钮鼠标跟随光晕 ──
document.addEventListener('mousemove',e=>{const t=e.target.closest('.btn');if(!t)return;const r=t.getBoundingClientRect();t.style.setProperty('--mx',((e.clientX-r.left)/r.width*100)+'%');t.style.setProperty('--my',((e.clientY-r.top)/r.height*100)+'%')});

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
    botId: '',           // ← 修复：从 status API 读取
    forwardMode: false,
    msgLogEnabled: true,
    recallMonitorEnabled: false,
    theme: localStorage.getItem('theme') || 'light',
    glassEnabled: localStorage.getItem('glassEnabled') !== '0',  // 液态玻璃默认开启（增强）
    frostEnabled: localStorage.getItem('frostEnabled') === '1',    // 毛玻璃默认关闭（V4.2 修复）
    effectEnabled: localStorage.getItem('effectEnabled') === '1',  // 界面效果总开关默认关闭
    selectedSticker: null,
    statusPollingStarted: false,  // ← 修复：防止重复轮询
    backgroundMode: localStorage.getItem('bgMode') || 'colorful',  // colorful | glass | custom
    customBg: localStorage.getItem('customBg') || '',            // 自定义背景 Data URL
    bgDim: localStorage.getItem('bgDim') === '1',
    theme_qq: localStorage.getItem('themeQQ') || 'default',
    effect: localStorage.getItem('phoneEffect') || '',  // V4.2 手机厂商效果（替代液态玻璃）
    customTheme: JSON.parse(localStorage.getItem('customTheme') || 'null'),
    techMode: localStorage.getItem('techMode') === '1',          // 大公司科技感模式
    memberBadges: JSON.parse(localStorage.getItem('memberBadges') || '{}'),  // {user_id: {text, type, color, auth, avatar}}
    memberAuth: JSON.parse(localStorage.getItem('memberAuth') || '{}'),  // {user_id: true} 认证蓝标
    memberAvatars: JSON.parse(localStorage.getItem('memberAvatars') || '{}'),  // {user_id: dataURL} 自定义头像
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
        const res = await fetch(path, {
            headers: { 'Content-Type': 'application/json' },
            ...opts,
            body: opts.body ? JSON.stringify(opts.body) : undefined,
        });
        const j = await res.json();
        return j;
    } catch (e) {
        console.error(`[API] ${path} 请求失败:`, e);
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

// ── 液态玻璃开关（V4.2 修复：默认关闭） ──
function toggleGlass(enabled) {
    state.glassEnabled = enabled;
    localStorage.setItem('glassEnabled', enabled ? '1' : '0');
    const cb = $('settingGlassEffect');
    if (cb) cb.checked = enabled;
    if (enabled) {
        document.documentElement.classList.remove('no-glass');
    } else {
        document.documentElement.classList.add('no-glass');
    }
    // 与全场景液态玻璃总开关联动（合并后的单一总开关）
    if (typeof LG !== 'undefined') {
        LG.setEnabled(enabled);
        localStorage.setItem('lgEnabled', enabled ? '1' : '0');
    }
}

// ── 毛玻璃效果开关（V4.2 新增：默认关闭） ──
function toggleFrost(enabled) {
    state.frostEnabled = enabled;
    localStorage.setItem('frostEnabled', enabled ? '1' : '0');
    const cb = $('settingFrostEffect');
    if (cb) cb.checked = enabled;
    if (enabled) {
        document.documentElement.classList.remove('no-frost');
    } else {
        document.documentElement.classList.add('no-frost');
    }
}

// ── 界面效果总开关（V4.2 新增：默认关闭） ──
function toggleEffectEnabled(enabled) {
    state.effectEnabled = enabled;
    localStorage.setItem('effectEnabled', enabled ? '1' : '0');
    const cb = $('settingEffectEnabled');
    if (cb) cb.checked = enabled;
    if (enabled) {
        document.documentElement.classList.remove('no-effect');
    } else {
        document.documentElement.classList.add('no-effect');
    }
}

// ── 黑夜模式（V4.2 修复：与厂商效果完全兼容） ──
function setDarkMode(enabled) {
    state.theme = enabled ? 'dark' : 'light';
    document.documentElement.setAttribute('data-theme', state.theme);
    localStorage.setItem('theme', state.theme);
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
    localStorage.setItem('bgMode', mode);
    if (mode !== 'custom') {
        // 切换走自定义时保留已存图片，方便切回
        state.customBg = localStorage.getItem('customBg') || '';
    }
    applyBackground();
}

function toggleBgDim(enabled) {
    state.bgDim = enabled;
    localStorage.setItem('bgDim', enabled ? '1' : '0');
    applyBackground();
}

function clearCustomBg() {
    state.customBg = '';
    localStorage.removeItem('customBg');
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
        localStorage.setItem('customBg', dataUrl);
        state.backgroundMode = 'custom';
        localStorage.setItem('bgMode', 'custom');
        applyBackground();
        showToast('自定义背景已应用', 'success');
    });
}

// ── V4.2 各手机厂商 UI 效果系统（替代液态玻璃）──
const PHONE_EFFECTS = [
    { id: 'ios',       name: '🍎 iOS 18',     swatch: 'linear-gradient(135deg,#007AFF,#5AC8FA)' },
    { id: 'harmony',   name: '🌸 鸿蒙 HarmonyOS', swatch: 'linear-gradient(135deg,#FF6B6B,#FFB088)' },
    { id: 'harmony-spatial', name: '🌌 鸿蒙空间光感', swatch: 'linear-gradient(135deg,#2B9DFA,#7DD8FF)' },
    { id: 'hyperos',   name: '🟠 HyperOS 小米', swatch: 'linear-gradient(135deg,#FF6700,#FF9F1A)' },
    { id: 'coloros',   name: '💚 ColorOS OPPO', swatch: 'linear-gradient(135deg,#1A8C5E,#3DCCA6)' },
    { id: 'origin',    name: '🔵 OriginOS vivo', swatch: 'linear-gradient(135deg,#4158D0,#0093E9)' },
    { id: 'oneui',     name: '🌌 OneUI 三星', swatch: 'linear-gradient(135deg,#1428A0,#5B6CE5)' },
    { id: 'flyme',     name: '🪶 Flyme 魅族', swatch: 'linear-gradient(135deg,#00B0F0,#00D8C8)' },
    { id: 'oxygen',    name: '🔴 OxygenOS 一加', swatch: 'linear-gradient(135deg,#EB0028,#FF4444)' },
    { id: 'magic',     name: '💜 MagicOS 荣耀', swatch: 'linear-gradient(135deg,#7B61FF,#A78BFA)' },
    { id: 'material',  name: '🤖 Material You', swatch: 'linear-gradient(135deg,#6750A4,#D0BCFF)' },
];
function renderEffectGrid() {
    const grid = $('effectGrid');
    if (!grid) return;
    grid.innerHTML = PHONE_EFFECTS.map(e => `
        <button class="theme-chip ${state.effect === e.id ? 'active' : ''}" data-effect-id="${e.id}" onclick="setPhoneEffect('${e.id}')">
            <span class="theme-swatch" style="background:${e.swatch}"></span>
            <span>${e.name}</span>
        </button>
    `).join('');
}
function setPhoneEffect(effectId) {
    state.effect = effectId;
    localStorage.setItem('phoneEffect', effectId);
    // V4.2 修复：选择效果时自动开启总开关
    if (effectId && !state.effectEnabled) {
        state.effectEnabled = true;
        localStorage.setItem('effectEnabled', '1');
        const cb = $('settingEffectEnabled');
        if (cb) cb.checked = true;
        document.documentElement.classList.remove('no-effect');
    }
    applyPhoneEffect();
    flashThemeSwitch();
    showToast(`已切换为 ${PHONE_EFFECTS.find(e=>e.id===effectId)?.name || '原生'} 效果`, 'success');
}
function applyPhoneEffect() {
    const html = document.documentElement;
    PHONE_EFFECTS.forEach(e => html.classList.remove('effect-' + e.id));
    // V4.2 修复：总开关关闭时不应用任何厂商效果
    if (state.effectEnabled && state.effect) {
        html.classList.add('effect-' + state.effect);
    }
    document.querySelectorAll('#effectGrid .theme-chip').forEach(c => {
        c.classList.toggle('active', c.dataset.effectId === state.effect && state.effectEnabled);
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
                <label style="display:flex;align-items:center;gap:6px;font-size:12px"><span>主色 A</span><input type="color" id="customFrom" value="${state.customTheme?.from || CUSTOM_THEME_DEFAULTS.from}" onchange="onCustomThemeChange()" style="width:36px;height:28px;border:1px solid var(--border-soft);border-radius:4px;background:none"></label>
                <label style="display:flex;align-items:center;gap:6px;font-size:12px"><span>主色 B</span><input type="color" id="customTo" value="${state.customTheme?.to || CUSTOM_THEME_DEFAULTS.to}" onchange="onCustomThemeChange()" style="width:36px;height:28px;border:1px solid var(--border-soft);border-radius:4px;background:none"></label>
                <label style="display:flex;align-items:center;gap:6px;font-size:12px"><span>背景 A</span><input type="color" id="customBgFrom" value="${state.customTheme?.bgFrom || CUSTOM_THEME_DEFAULTS.bgFrom}" onchange="onCustomThemeChange()" style="width:36px;height:28px;border:1px solid var(--border-soft);border-radius:4px;background:none"></label>
                <label style="display:flex;align-items:center;gap:6px;font-size:12px"><span>背景 B</span><input type="color" id="customBgTo" value="${state.customTheme?.bgTo || CUSTOM_THEME_DEFAULTS.bgTo}" onchange="onCustomThemeChange()" style="width:36px;height:28px;border:1px solid var(--border-soft);border-radius:4px;background:none"></label>
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
    localStorage.setItem('customTheme', JSON.stringify(state.customTheme));
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
    localStorage.setItem('customTheme', JSON.stringify(state.customTheme));
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
    const h = document.documentElement;
    h.classList.add('theme-switching');
    clearTimeout(h.__tsTimer);
    h.__tsTimer = setTimeout(() => h.classList.remove('theme-switching'), 480);
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
    localStorage.setItem('themeQQ', themeId);
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

/* 鸿蒙空间光感：点击坐标生成灵动粒子爆发 */
function spawnHarmonyParticles(clientX, clientY){
    const count = 8 + Math.floor(Math.random() * 5);
    const frag = document.createDocumentFragment();
    for (let i = 0; i < count; i++){
        const p = document.createElement('span');
        p.className = 'harmony-particle';
        const angle = Math.random() * Math.PI * 2;
        const dist = 20 + Math.random() * 36;
        p.style.left = clientX + 'px';
        p.style.top = clientY + 'px';
        p.style.setProperty('--tx', Math.cos(angle) * dist + 'px');
        p.style.setProperty('--ty', Math.sin(angle) * dist + 'px');
        frag.appendChild(p);
        setTimeout(() => { if (p.parentNode) p.parentNode.removeChild(p); }, 720);
    }
    document.body.appendChild(frag);
}
function initHarmonySpatialInteractions(){
    document.addEventListener('click', (e) => {
        if (!document.documentElement.classList.contains('theme-qq-harmony-spatial')) return;
        const t = e.target.closest('.btn, .chip, .theme-chip, .toggle .slider, input[type="range"], .sticker-item, .tab-item');
        if (!t) return;
        spawnHarmonyParticles(e.clientX, e.clientY);
    });
}

// ── Tab 切换 ──
// 右上角"⋯"在 设置 ↔ 消息 之间切换（侧边栏已删除，作为唯一入口）
function toggleSettings() {
    const active = document.querySelector('.tab-page.active');
    switchTab(active && active.id === 'tab-settings' ? 'tab-messages' : 'tab-settings');
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
    }
    if (tabId === 'tab-messages' || tabId === 'tab-settings') {
        refreshBadgeStats();
        const cb = $('settingTechMode');
        if (cb) cb.checked = state.techMode;
    }
    if (tabId === 'tab-settings') {
        // 插件生态已合并到设置页：进入设置时加载插件列表
        if (typeof loadPlugins === 'function') loadPlugins();
    }
    updateTabIndicator(tabId);
}

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

// ── Tab 指示器位置更新（跑道形液态玻璃滑块，支持横屏垂直模式） ──
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
document.addEventListener('DOMContentLoaded', () => updateTabIndicator('tab-messages'));
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
    const limit = $('msgLimit').value;
    // ← 修复：按当前监听群请求，后端只返回该群消息，避免混入其他群历史消息
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
        if (m.media_info && m.media_info.type === 'image' && m.media_info.image_urls && m.media_info.image_urls.length) {
            const extra = (m.content && m.content !== '[图片]') ? `<div>${escHtml(m.content)}</div>` : '';
            const imgs = m.media_info.image_urls.map(u =>
                `<img src="${u}" class="msg-image" alt="[图片]" referrerpolicy="no-referrer" onclick="event.stopPropagation()">`
            ).join('');
            contentHtml = extra + `<div class="msg-images">${imgs}</div>`;
        } else {
            const tag = m.media_info ? (m.media_info.type === 'sticker' ? ' 😀' : m.media_info.type === 'file' ? ' 📎' : '') : '';
            contentHtml = escHtml(m.content || '') + tag;
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
    container.replaceChildren(frag);
}
function escHtml(s) {
    const d = document.createElement('div');
    d.textContent = s;
    return d.innerHTML;
}
function filterMessages() {
    const q = $('searchMessage').value.toLowerCase();
    const items = document.querySelectorAll('.message-entry');
    items.forEach(el => { el.style.display = el.textContent.toLowerCase().includes(q) ? '' : 'none'; });
}
async function clearMessages() {
    // ← 修复：同时清空后端缓存并重新加载，避免清空后自动刷新又把旧消息拉回来
    await api('/api/messages/clear');
    state.messages = [];
    state._msgSig = '';
    $('messageLog').innerHTML = '<div class="empty-state">已清空</div>';
    loadRecentMessages();
}

// ── 自动刷新 (消息 2 秒 / 元宝派+成员 5 秒) ──
let autoRefreshTimer = null;
let panelRefreshTimer = null;
function toggleAutoRefresh() {
    const checked = $('autoRefreshToggle').checked;
    if (checked) {
        autoRefreshTimer = setInterval(() => loadRecentMessages(), 2000);
        // 元宝派（群列表）与成员：5 秒刷新一次，避免群名查询/成员列表请求过于频繁
        panelRefreshTimer = setInterval(() => {
            loadGroups();
            loadMembers();
        }, 5000);
    } else {
        if (autoRefreshTimer) { clearInterval(autoRefreshTimer); autoRefreshTimer = null; }
        if (panelRefreshTimer) { clearInterval(panelRefreshTimer); panelRefreshTimer = null; }
    }
}
// 页面加载后立即启动自动刷新
document.addEventListener('DOMContentLoaded', () => {
    // autoRefreshToggle 默认是 checked，手动触发生效
    setTimeout(() => toggleAutoRefresh(), 500);
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
    const needsTarget = ['at', 'atspam', 'dm', 'dmspam', 'latex', 'latex-spam'].includes(mode);
    $('composerTargetRow').style.display = needsTarget ? 'block' : 'none';
    $('composerCountRow').style.display = ['spam', 'atspam', 'dmspam', 'latex-spam'].includes(mode) ? 'block' : 'none';
    $('composerLatexControls').style.display = ['latex', 'latex-spam'].includes(mode) ? 'block' : 'none';
}
async function sendFromComposer() {
    const text = $('composerText').value.trim();
    if (!text) { showToast('请输入消息内容', 'warning'); return; }
    const body = { text, mode: _composerMode };
    if (['at', 'atspam'].includes(_composerMode)) {
        // at 模式：后端 /api/send 读取 at_user / at_nickname
        body.at_user = $('composerTargetId').value.trim();
        body.at_nickname = $('composerTargetNick').value.trim();
    } else if (_composerMode === 'multi-at') {
        // 批量 @：多个用户ID以逗号/空格/换行分隔
        const ids = $('composerTargetId').value.split(/[,，\s]+/).filter(Boolean);
        const nicks = $('composerTargetNick').value.split(/[,，\s]+/).filter(Boolean);
        const users = ids.map((id, i) => ({ user_id: id, nickname: nicks[i] || '' }));
        body.users = users;
    } else if (['dm', 'dmspam'].includes(_composerMode)) {
        body.target_id = $('composerTargetId').value.trim();
        body.target_nick = $('composerTargetNick').value.trim();
    } else if (['latex', 'latex-spam'].includes(_composerMode)) {
        body.target_id = $('composerTargetId').value.trim();
        body.target_nick = $('composerTargetNick').value.trim();
    }
    if (['spam', 'atspam', 'dmspam', 'latex-spam'].includes(_composerMode)) {
        body.count = parseInt($('composerCount').value) || 5;
        body.interval = parseFloat($('composerInterval').value) || 0.1;
    }
    if (['latex', 'latex-spam'].includes(_composerMode)) {
        body.scale = parseFloat($('composerScaleSlider').value) || 3.0;
        body.rotate = parseInt($('composerRotateSlider').value) || 15;
    }
    const endpoint = (_composerMode === 'multi-at') ? '/api/send-multi-at' : '/api/send';
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

// ── 发送消息 ──
let currentMode = 'normal';
function setMode(mode) {
    currentMode = mode;
    document.querySelectorAll('.chip').forEach(c => c.classList.remove('active'));
    const chip = document.querySelector(`.chip[data-mode="${mode}"]`);
    if (chip) chip.classList.add('active');
    const isMedia = mode === 'media';
    const isSticker = mode === 'sticker';
    const needsTarget = !isMedia && !isSticker && ['at', 'atspam', 'multi-at', 'dm', 'dmspam'].includes(mode);
    const needsCount = !isMedia && !isSticker && ['spam', 'atspam', 'dmspam', 'latex-spam'].includes(mode);
    // media 模式：显示图片/文件功能区；sticker 模式：显示贴纸区；均隐藏文本发送区
    const mediaCtl = $('mediaControls');
    if (mediaCtl) mediaCtl.style.display = isMedia ? 'block' : 'none';
    const stickerCtl = $('stickerControls');
    if (stickerCtl) stickerCtl.style.display = isSticker ? 'block' : 'none';
    const sendText = $('sendTextArea');
    if (sendText) sendText.style.display = (isMedia || isSticker) ? 'none' : '';
    $('targetRow').style.display = needsTarget ? '' : 'none';
    $('countRow').style.display = needsCount ? '' : 'none';
    // 艾特全体：仅在「@ 艾特」模式下显示
    const atAllRow = $('atAllRow');
    if (atAllRow) atAllRow.style.display = (mode === 'at') ? '' : 'none';
    const latexCtl = $('latexControls');
    if (latexCtl) latexCtl.style.display = (mode === 'latex' || mode === 'latex-spam') ? 'block' : 'none';
    $('targetLabel').textContent = (mode === 'dm' || mode === 'dmspam') ? '目标用户ID' : '@ 目标用户ID';
    $('targetHint').textContent = needsTarget ? '💡 可在「成员」面板点击用户快速填入' : '';
    if (mode === 'latex' || mode === 'latex-spam') updateLatexPreview();
}
async function sendMessage() {
    const text = $('inputMessage').value.trim();

    // LaTeX 模式：组合所有 LaTeX 片段作为发送内容
    if (currentMode === 'latex' || currentMode === 'latex-spam') {
        if (latexItems.length === 0) {
            showToast('请先添加至少一个 LaTeX 片段到消息列表', 'warning');
            return;
        }
        const finalMessage = latexItems.map(it => it.code).join('\n');
        const body = {
            text: finalMessage,
            mode: currentMode === 'latex-spam' ? 'spam' : 'normal',
        };
        if ($('countRow').style.display !== 'none') {
            body.count = parseInt($('inputCount').value) || 5;
            body.interval = parseFloat($('inputInterval').value) || 0.1;
        }
        const r = await api('/api/send', { method: 'POST', body });
        if (r && r.ok) {
            showToast(r.message || 'LaTeX 消息已发送', 'success');
            if (currentMode === 'latex') clearLatexList();
        } else {
            showToast((r && r.message) || '发送失败', 'error');
        }
        return;
    }

    if (!text) { showToast('请输入消息内容', 'warning'); return; }
    const body = { text, mode: currentMode };
    let endpoint = '/api/send';
    if ($('targetRow').style.display !== 'none') {
        const uid = $('inputTargetId').value.trim();
        body.target_id = uid;
        if (currentMode === 'at' || currentMode === 'atspam') {
            body.at_user = uid;
            body.at_nickname = $('inputTargetNick').value.trim();
        } else if (currentMode === 'multi-at') {
            // 批量 @：多个用户ID以逗号/空格/换行分隔
            const ids = uid.split(/[,，\s]+/).filter(Boolean);
            const nicks = $('inputTargetNick').value.split(/[,，\s]+/).filter(Boolean);
            body.users = ids.map((id, i) => ({ user_id: id, nickname: nicks[i] || '' }));
            endpoint = '/api/send-multi-at';
        }
    }
    if ($('countRow').style.display !== 'none') {
        body.count = parseInt($('inputCount').value) || 1;
        body.interval = parseFloat($('inputInterval').value) || 0.1;
    }
    const r = await api(endpoint, { method: 'POST', body });
    if (r && r.ok) {
        showToast('发送成功', 'success');
        $('inputMessage').value = '';
    } else {
        showToast((r && r.message) || '发送失败', 'error');
    }
}

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
    latexItems.push({ text, code: generateLatexCode(), settings: { ...latexSettings } });
    $('latexListContainer').style.display = 'block';
    renderLatexList();
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
async function loadStickers() {
    const r = await api('/api/stickers');
    if (r && r.stickers) {
        stickerList = r.stickers;
        renderStickers();
    }
}
function renderStickers() {
    const grid = $('stickerGrid');
    const q = ($('stickerSearch').value || '').toLowerCase();
    const items = stickerList.filter(s => !q || s.name.includes(q));
    grid.innerHTML = items.map(s => `<div class="sticker-item ${state.selectedSticker === s.name ? 'selected' : ''}" onclick="selectSticker('${s.name}')" ondblclick="sendStickerByName('${s.name}')"><span class="sticker-emoji">😊</span>${s.name}</div>`).join('');
}
function selectSticker(name) { state.selectedSticker = name; renderStickers(); }
function sendStickerByName(name) {
    state.selectedSticker = name;
    renderStickers();
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
    if (r && r.ok) { showToast('贴纸已发送', 'success'); }
    else { showToast((r && r.message) || '发送失败', 'error'); }
}

// ── 群成员 ──
async function loadMembers() {
    // ← 修复：返回 { ok, members: [...] }
    const r = await api('/api/members');
    if (r && r.ok && r.members) {
        state.members = r.members;
        state.groupOwnerUserId = r.group_owner_user_id || '';
        renderMembers();
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
    _currentBadgeText = existing.text || BADGE_PRESETS[_currentBadgeType]?.text || '成员';
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
    const auth = $('badgeAuthToggle')?.checked || false;
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
    const auth = $('badgeAuthToggle')?.checked || false;
    state.memberBadges[_editingBadgeUid] = { text, type, nick, updated: Date.now() };
    localStorage.setItem('memberBadges', JSON.stringify(state.memberBadges));
    state.memberAuth[_editingBadgeUid] = auth;
    localStorage.setItem('memberAuth', JSON.stringify(state.memberAuth));
    if (_currentAvatar) {
        state.memberAvatars[_editingBadgeUid] = _currentAvatar;
        localStorage.setItem('memberAvatars', JSON.stringify(state.memberAvatars));
    } else {
        delete state.memberAvatars[_editingBadgeUid];
        localStorage.setItem('memberAvatars', JSON.stringify(state.memberAvatars));
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
    localStorage.setItem('memberBadges', JSON.stringify(state.memberBadges));
    localStorage.setItem('memberAuth', JSON.stringify(state.memberAuth));
    localStorage.setItem('memberAvatars', JSON.stringify(state.memberAvatars));
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
    localStorage.setItem('memberBadges', '{}');
    localStorage.setItem('memberAuth', '{}');
    localStorage.setItem('memberAvatars', '{}');
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
                    localStorage.setItem('memberBadges', JSON.stringify(state.memberBadges));
                }
                if (data.auth) {
                    state.memberAuth = data.auth;
                    localStorage.setItem('memberAuth', JSON.stringify(state.memberAuth));
                }
                if (data.avatars) {
                    state.memberAvatars = data.avatars;
                    localStorage.setItem('memberAvatars', JSON.stringify(state.memberAvatars));
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
    const list = state.members.filter(m => !q || (m.nick_name || '').toLowerCase().includes(q) || (m.user_id || '').includes(q));
    if (!list.length) { $('memberList').innerHTML = '<div class="empty-state">无成员</div>'; return; }
    $('memberList').innerHTML = list.map(m => {
        const isOwner = m.user_id === state.groupOwnerUserId;
        const uid = escapeHtml(m.user_id);
        const b = state.memberBadges[m.user_id] || {};
        const avatarUrl = state.memberAvatars[m.user_id];
        const auth = state.memberAuth[m.user_id] === true;
        const displayName = b.nick || m.nick_name || '(无名)';
        let badges = '';
        // 系统默认铭牌（群主/元宝AI/机器人/成员）
        if (isOwner) badges += '<span class="member-badge admin">群主</span>';
        else if (m.member_type === 2) badges += '<span class="member-badge ai">元宝AI</span>';
        else if (m.member_type === 3) badges += '<span class="member-badge bot">机器人</span>';
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
                 title="左键单击复制ID / 双击使用 / 右键设置"
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
    if (uid) useMemberAsTarget(uid, el.dataset.nick || '');
}
function memberContextMenu(e, el) {
    e.preventDefault();
    const uid = el.dataset.uid;
    if (uid) openBadgeEditor(uid);
}
function useMemberAsTarget(uid, nick) {
    switchTab('tab-messages');
    // 智能切换：当前是 at/atspam 模式则保持 at，否则切到 dm（便于私聊）
    if (currentMode !== 'at' && currentMode !== 'atspam' && currentMode !== 'multi-at') {
        setMode('dm');
    }
    $('inputTargetId').value = uid;
    $('inputTargetNick').value = nick || '';
    // 发送面板悬浮于消息之上（吸底），滚动到最新消息即可看到
    const ml = $('messageLog');
    if (ml) ml.scrollIntoView({ behavior: 'smooth', block: 'end' });
    showToast(`已选择 ${nick || uid}${currentMode === 'dm' ? '（私聊）' : '（@）'}`, 'success');
}
function copyText(text) {
    navigator.clipboard.writeText(text).then(() => showToast('已复制', 'success')).catch(() => showToast('复制失败', 'error'));
}

// ── 科技感大公司级模式（v6.0） ──
function applyTechMode(enabled) {
    document.documentElement.classList.toggle('tech-mode', enabled);
    const cb = $('settingTechMode');
    if (cb) cb.checked = enabled;
}
function toggleTechMode(enabled) {
    state.techMode = enabled;
    localStorage.setItem('techMode', enabled ? '1' : '0');
    applyTechMode(enabled);
    showToast(enabled ? '🚀 已进入科技感 HUD 模式' : '已恢复经典模式', 'success');
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
    const body = { index: idx, text };
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
function handleImageFileSelect(e) {
    const file = e.target.files[0];
    if (!file) return;
    $('imageFileLabel').classList.add('has-file');
    $('imageFileLabel').querySelector('.file-name').textContent = file.name;
    $('imagePreview').style.display = 'block';
    $('imagePreviewImg').src = URL.createObjectURL(file);
    $('imageFileSize').textContent = (file.size / 1024).toFixed(1) + ' KB';
    $('imageFileType').textContent = file.type;
}
async function sendImage() {
    const file = $('imageFile').files[0];
    if (!file) { showToast('请先选择图片', 'warning'); return; }
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
        if (j.ok) showToast('图片已发送', 'success');
        else showToast(j.message || '发送失败', 'error');
    } catch(e) { showToast('发送异常: ' + e.message, 'error'); }
}

// ── 文件上传发送 ──
function handleFileSelect(e) {
    const file = e.target.files[0];
    if (!file) return;
    $('documentFileLabel').classList.add('has-file');
    $('documentFileLabel').querySelector('.file-name').textContent = file.name;
    $('fileInfo').style.display = 'block';
    $('fileSize').textContent = (file.size / 1024).toFixed(1) + ' KB';
    $('fileType').textContent = file.type || '未知类型';
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
        if (j.ok) showToast('文件已发送', 'success');
        else showToast(j.message || '发送失败', 'error');
    } catch(e) { showToast('发送异常: ' + e.message, 'error'); }
}

// ── 批量发送图片（V4.6）──
let _batchFiles = [];
let _batchCancelled = false;
const _BATCH_MAX = 50;

function handleBatchImageSelect(e) {
    const files = Array.from(e.target.files || []);
    if (!files.length) return;
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
const filterMessagesDebounced = debounce(filterMessages, 200);
const filterStickersDebounced = debounce(filterStickers, 200);
const filterMembersDebounced = debounce(filterMembers, 200);

// ── @全体（V4.2 增强：诊断 + 详细错误） ──
async function sendAtAll() {
    const text = $('atAllText').value.trim();
    if (!text) { showToast('请输入内容', 'warning'); return; }
    if (!confirm('确认发送 @全体成员？（将触发强提醒）')) return;
    // 先做协议诊断
    try {
        const diag = await api('/api/diag/at-all', { method: 'GET' });
        if (!diag.connected) { showToast('WebSocket 未连接，请先在设置中连接', 'error'); return; }
        if (!diag.group_code) { showToast('未设置默认群', 'error'); return; }
    } catch (_) {}
    const r = await api('/api/send/at-all', { method: 'POST', body: { text } });
    if (r && r.ok) {
        showToast(r.message || '@全体已发送', 'success');
        $('atAllText').value = '';
    } else {
        const msg = (r && r.message) || '发送失败';
        const code = (r && r.code) || 'UNKNOWN';
        showToast(`${msg} [${code}]`, 'error', 6000);
    }
}

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
    const body = {
        default_group: $('settingDefaultGroup').value.trim(),
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
    if (s.default_group) $('settingDefaultGroup').value = s.default_group;
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
        // 同步当前监听群
        if (r.current_group) state.currentGroup = r.current_group;
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
            gl.innerHTML = r.groups.map(g => {
                const active = g.group_code === state.currentGroup;
                const meta = g.last_message || (g.message_count ? `${g.message_count} 条消息` : '');
                return `<div class="member-item group-item${active ? ' active' : ''}" data-group="${escapeHtml(g.group_code)}" onclick="switchGroupByCode('${escapeHtml(g.group_code)}')">
                    <div class="member-info">
                        <div class="member-name">${active ? '🎧 ' : ''}${escapeHtml(gname(g))}</div>
                        <div class="member-id">${escapeHtml(meta)}</div>
                    </div>
                </div>`;
            }).join('') || '<div class="empty-state">暂无</div>';
        }
    }
}
function switchGroupByCode(code) {
    if (!code) return;
    state.currentGroup = code;
    api('/api/groups/switch', { method: 'POST', body: { group_code: code } }).then(r => {
        if (r && r.ok) {
            showToast('已切换群', 'success');
            loadGroups();
            // ← 修复：切换监听群后立即刷新消息面板，跟随显示新群消息
            loadRecentMessages();
        } else {
            showToast((r && r.message) || '切换失败', 'error');
        }
    });
}

// ── 插件生态（v4.4）──
// 插件只需注册"页面"与"卡片"元数据，这里用系统统一 settings-section 样式渲染，
// 自动继承 CSS 变量 / 液态玻璃 / 深色浅色 / 厂商效果主题，插件无法自定义 CSS。
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


// ── 🌊 全场景液态玻璃 LiquidGlass（V4.5 新增）──
// 物理引擎（弹簧-阻尼）+ 设备感应器（重力/陀螺仪）+ 鼠标视差；折射/光影/性能 独立开关
const LG = (() => {
  const state = {
    enabled: false, physics: false, refraction: false, lighting: false, perf: false,
    extreme: false,                // 🚀 性能极致模式（V6.0）：折射光彩 + 液态玻璃真流动
    _mem: null,                    // 进入极致模式前记忆的子开关状态，便于退出时还原
    tilt: { x: 0, y: 0 },          // 当前（弹簧平滑后）倾斜
    vel: { x: 0, y: 0 },           // 速度
    target: { x: 0, y: 0 },        // 目标（来自感应器/鼠标）
    dispScale: 26,
    lightX: 50, lightY: 16,
    running: false, raf: 0, last: 0,
    fps: 60, frames: 0, fpsT: 0,
    sensorsGranted: false, autoDegrade: false,
  };
  const K = 0.06;   // 弹簧刚度
  const D = 0.86;   // 阻尼
  const MAXT = 1;   // 倾斜幅度上限
  const root = document.documentElement;
  const dispEl = () => document.getElementById('lgDispMap');

  function applyVars() {
    root.style.setProperty('--lg-hx', state.lightX.toFixed(1) + '%');
    root.style.setProperty('--lg-hy', state.lightY.toFixed(1) + '%');
    root.style.setProperty('--lg-disp', state.dispScale.toFixed(1));
    const de = dispEl();
    if (de) de.setAttribute('scale', state.dispScale.toFixed(1));
  }

  function step() {
    // 弹簧-阻尼物理：target 由感应器/鼠标设定，tilt 平滑追随
    state.vel.x += (state.target.x - state.tilt.x) * K;
    state.vel.y += (state.target.y - state.tilt.y) * K;
    state.vel.x *= D; state.vel.y *= D;
    state.tilt.x = Math.max(-MAXT, Math.min(MAXT, state.tilt.x + state.vel.x));
    state.tilt.y = Math.max(-MAXT, Math.min(MAXT, state.tilt.y + state.vel.y));
    const tiltMag = Math.hypot(state.tilt.x, state.tilt.y);
    let base = state.refraction ? (state.perf ? 35 : 75) : 0;   // 增强：折射位移强度明显提升
    // 🚀 性能极致模式：拉满折射位移（仅在已授权物理引擎+感应器时配合 lg-refract-flow 流动滤镜生效）
    if (state.extreme && state.refraction) base = state.perf ? 60 : 110;
    state.dispScale = base * (0.6 + tiltMag * 0.9);
    // 高光位置随倾斜移动（光源固定、玻璃倾斜 → 高光偏移）
    state.lightX = 50 - state.tilt.x * 38;
    state.lightY = 16 - state.tilt.y * 30;
    applyVars();
  }

  function loop(t) {
    if (!state.running) return;
    if (!state.last) state.last = t;
    let dt = t - state.last; state.last = t;
    if (dt > 60) dt = 60;
    // FPS 监控 → 自动降级，避免卡死
    state.frames++; state.fpsT += dt;
    if (state.fpsT >= 1000) {
      state.fps = Math.round(state.frames * 1000 / state.fpsT);
      state.frames = 0; state.fpsT = 0;
      if (!state.perf && state.fps < 30 && !state.autoDegrade) {
        state.autoDegrade = true;
        root.classList.add('lg-perf');
        const pe = $('lgPerf'); if (pe) pe.checked = true;
        showToast('液态玻璃已自动切换性能模式（帧率偏低）', 'warning');
      }
    }
    if (state.physics || state.lighting) step();
    state.raf = requestAnimationFrame(loop);
  }
  function start() { if (state.running) return; state.running = true; state.last = 0; state.raf = requestAnimationFrame(loop); }
  function stop() { state.running = false; if (state.raf) cancelAnimationFrame(state.raf); }

  function onOrientation(e) {
    if (!state.enabled || !state.physics) return;
    const gamma = e.gamma || 0, beta = e.beta || 0;
    state.target.x = Math.max(-1, Math.min(1, gamma / 35));
    state.target.y = Math.max(-1, Math.min(1, (beta - 45) / 35));
  }
  function onMotion(e) {
    if (!state.enabled || !state.physics) return;
    const a = e.accelerationIncludingGravity || e.acceleration;
    if (!a) return;
    state.vel.x += (a.x || 0) * 0.0008;   // 晃动 → 瞬态冲量
    state.vel.y += (a.y || 0) * 0.0008;
  }
  function onMouse(e) {
    if (!state.enabled || !state.physics) return;
    state.target.x = (e.clientX / window.innerWidth - 0.5) * 1.6;
    state.target.y = (e.clientY / window.innerHeight - 0.5) * 1.6;
  }
  function onVisibility() {
    if (document.hidden) stop();
    else if (state.enabled && (state.physics || state.lighting)) start();
  }
  function bindSensors() {
    window.addEventListener('deviceorientation', onOrientation, { passive: true });
    window.addEventListener('devicemotion', onMotion, { passive: true });
    window.addEventListener('mousemove', onMouse, { passive: true });
    document.addEventListener('visibilitychange', onVisibility);
  }
  async function requestSensors() {
    try {
      if (typeof DeviceOrientationEvent !== 'undefined' && typeof DeviceOrientationEvent.requestPermission === 'function') {
        const res = await DeviceOrientationEvent.requestPermission();
        state.sensorsGranted = (res === 'granted');
        if (res === 'granted' && typeof DeviceMotionEvent !== 'undefined' && typeof DeviceMotionEvent.requestPermission === 'function') {
          try { await DeviceMotionEvent.requestPermission(); } catch (_) {}
        }
      } else { state.sensorsGranted = true; } // 非 iOS：默认可用
    } catch (err) { state.sensorsGranted = false; }
    applyAuth(); updateExtremeStatus();
    return state.sensorsGranted;
  }
  function setEnabled(on) {
    state.enabled = on;
    root.classList.toggle('lg-full', on);
    if (on) {
      root.classList.toggle('lg-refraction', state.refraction);
      root.classList.toggle('lg-lighting', state.lighting);
      root.classList.toggle('lg-physics', state.physics);
      root.classList.toggle('lg-perf', state.perf);
      if (state.physics || state.lighting) start(); else stop();
      applyVars();
    } else { stop(); root.classList.remove('lg-full', 'lg-refraction', 'lg-lighting', 'lg-physics', 'lg-perf'); }
    applyAuth(); updateExtremeStatus();
  }
  function setOpt(key, on) {
    state[key] = on;
    if (key === 'refraction') root.classList.toggle('lg-refraction', on && state.enabled);
    if (key === 'lighting') root.classList.toggle('lg-lighting', on && state.enabled);
    if (key === 'physics') root.classList.toggle('lg-physics', on && state.enabled);
    if (key === 'perf') { root.classList.toggle('lg-perf', on && state.enabled); state.autoDegrade = false; }
    if (key === 'physics') {
      if (on && state.enabled && (state.physics || state.lighting)) start();
      else if (!state.physics && !state.lighting) stop();
    }
    applyAuth(); updateExtremeStatus();
    applyVars();
  }
  function applySubClasses() {
    root.classList.toggle('lg-refraction', state.refraction && state.enabled);
    root.classList.toggle('lg-lighting', state.lighting && state.enabled);
    root.classList.toggle('lg-physics', state.physics && state.enabled);
    root.classList.toggle('lg-perf', state.perf && state.enabled);
  }
  function syncToggle(id, v) { const el = $(id); if (el) el.checked = v; }
  // 🚀 极致渲染仅在「极端模式 + 全场景开启 + 物理引擎启用 + 设备感应器已授权」四者齐备时生效
  function applyAuth() {
    root.classList.toggle('lg-authorized',
      !!(state.extreme && state.enabled && state.physics && state.sensorsGranted));
  }
  function updateExtremeStatus() {
    const el = $('lgExtremeStatus'); if (!el) return;
    if (!state.extreme) { el.style.display = 'none'; el.textContent = ''; return; }
    el.style.display = 'block';
    if (state.physics && state.sensorsGranted) {
      el.style.color = 'var(--success, #16a34a)';
      el.innerHTML = '✅ 极致渲染已激活：折射光彩 + 液态玻璃真流动 生效中';
    } else {
      el.style.color = 'var(--warning, #d97706)';
      const miss = [];
      if (!state.physics) miss.push('🧲 物理引擎');
      if (!state.sensorsGranted) miss.push('📡 设备感应器');
      el.innerHTML = '⏳ 待授权：请启用 ' + miss.join(' 与 ') + ' 后，折射光彩与液态玻璃真流动才会生效';
    }
  }
  // 🚀 性能极致模式：一键拉满折射光彩 + 液态玻璃真流动 + 增强光影；退出时还原用户原有子开关
  function setExtreme(on) {
    state.extreme = on;
    if (on) {
      state._mem = { enabled: state.enabled, physics: state.physics, refraction: state.refraction, lighting: state.lighting };
      if (!state.enabled) { setEnabled(true); syncToggle('lgEnabled', true); }
      if (!state.physics) { state.physics = true; syncToggle('lgPhysics', true); }
      if (!state.refraction) { state.refraction = true; syncToggle('lgRefraction', true); }
      if (!state.lighting) { state.lighting = true; syncToggle('lgLighting', true); }
      root.classList.add('lg-extreme');
      applyAuth();
      if (state.physics || state.lighting) start();
    } else {
      root.classList.remove('lg-extreme', 'lg-authorized');
      if (state._mem) {
        if (state._mem.enabled !== state.enabled) { setEnabled(state._mem.enabled); syncToggle('lgEnabled', state._mem.enabled); }
        state.physics = state._mem.physics; syncToggle('lgPhysics', state.physics);
        state.refraction = state._mem.refraction; syncToggle('lgRefraction', state.refraction);
        state.lighting = state._mem.lighting; syncToggle('lgLighting', state.lighting);
      }
      applySubClasses();
      if (!state.physics && !state.lighting) stop();
    }
    applyVars(); updateExtremeStatus();
  }
  function init() {
    bindSensors();
    state.enabled = localStorage.getItem('lgEnabled') !== '0';
    state.physics = localStorage.getItem('lgPhysics') === '1';
    state.refraction = localStorage.getItem('lgRefraction') !== '0';  // 折射默认开启（增强）
    state.lighting = localStorage.getItem('lgLighting') === '1';
    state.perf = localStorage.getItem('lgPerf') === '1';
    state.extreme = localStorage.getItem('lgExtreme') === '1';
    const s = (id, v) => { const el = $(id); if (el) el.checked = v; };
    s('lgEnabled', state.enabled); s('lgPhysics', state.physics);
    s('lgRefraction', state.refraction); s('lgLighting', state.lighting); s('lgPerf', state.perf);
    s('lgExtreme', state.extreme);
    if (state.enabled) setEnabled(true);
    if (state.extreme) setExtreme(true);
  }
  return { init, setEnabled, setOpt, setExtreme, requestSensors, state };
})();

function onLgEnabled(v) { LG.setEnabled(v); localStorage.setItem('lgEnabled', v ? '1' : '0'); toggleGlass(v); }
function onLgPhysics(v) { LG.setOpt('physics', v); localStorage.setItem('lgPhysics', v ? '1' : '0'); }
function onLgRefraction(v) { LG.setOpt('refraction', v); localStorage.setItem('lgRefraction', v ? '1' : '0'); }
function onLgLighting(v) { LG.setOpt('lighting', v); localStorage.setItem('lgLighting', v ? '1' : '0'); }
function onLgPerf(v) { LG.setOpt('perf', v); localStorage.setItem('lgPerf', v ? '1' : '0'); if (v) onLgExtreme(false); }  // 互斥：开性能兼容关性能极致
function onLgExtreme(v) { LG.setExtreme(v); localStorage.setItem('lgExtreme', v ? '1' : '0'); if (v) onLgPerf(false); }  // 互斥：开性能极致关性能兼容
async function requestLgSensors() {
  const ok = await LG.requestSensors();
  const el = $('lgSensorStatus');
  if (el) el.textContent = ok ? '✅ 感应器已授权' : '⚠️ 未授权（用鼠标视差）';
}

async function init() {
    await loadSettings();
    await loadStickers();
    // 插件生态（v4.4）：加载插件列表并渲染动态页面/卡片
    try { await loadPlugins(); } catch (e) { console.error('[插件] 初始化失败', e); }
    // 恢复液态玻璃开关（V4.2 默认关闭）
    toggleGlass(state.glassEnabled);
    // 恢复毛玻璃开关（V4.2 默认关闭）
    toggleFrost(state.frostEnabled);
    // 恢复界面效果总开关（V4.2 默认关闭）
    toggleEffectEnabled(state.effectEnabled);
    // 恢复背景设置（v4.0）
    applyBackground();
    // 恢复 QQ 主题（v5.0）
    applyQQTheme();
    initHarmonySpatialInteractions();   // 🌌 鸿蒙空间光感：点击粒子交互
    renderThemeGrid();
    // 恢复手机厂商效果（受总开关控制）
    applyPhoneEffect();
    renderEffectGrid();
    // 恢复科技感 HUD 模式（v4.1Pro）
    applyTechMode(state.techMode);
    // 初始化 LaTeX 编辑器（v4.1Pro）
    initLatexControls();
    // 同步黑夜模式开关
    const dmCb = $('settingDarkMode');
    if (dmCb) dmCb.checked = state.theme === 'dark';
    await connect();
    loadGroups();
    loadMembers();
    // V4.5：全场景液态玻璃（物理引擎 + 感应器）
    try { LG.init(); } catch (e) { console.error('[liquid-glass] 初始化失败', e); }
}
init();
'use strict';

/* ── 共享工具：把单个设置项同步到后端 /api/settings（后端化个性化） ── */
function _persistPref(key, value) {
    try { localStorage.setItem(key, typeof value === 'string' ? value : JSON.stringify(value)); } catch (e) {}
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
   🎬 视频背景（自定义 bg-mode：video）
   ═══════════════════════════════════════ */
const VideoBG = {
    el: null,
    ensure() {
        if (this.el) return;
        this.el = document.createElement('video');
        this.el.id = 'videoBgEl';
        this.el.autoplay = true; this.el.muted = true; this.el.loop = true; this.el.playsInline = true;
        Object.assign(this.el.style, {
            position: 'fixed', inset: '0', width: '100%', height: '100%',
            objectFit: 'cover', zIndex: '-2', pointerEvents: 'none', display: 'none'
        });
        document.body.prepend(this.el);
    },
    apply(url) {
        this.ensure();
        if (!url) { this.clear(); return; }
        this.el.src = url; this.el.style.display = 'block';
        document.documentElement.classList.add('bg-mode-video');
        this.el.play().catch(() => {});
    },
    clear() {
        if (this.el) { this.el.pause(); this.el.removeAttribute('src'); this.el.style.display = 'none'; }
        document.documentElement.classList.remove('bg-mode-video');
    }
};
window.VideoBG = VideoBG;

// 扩展原 setBackgroundMode 以支持 video 模式
(function () {
    const _orig = window.setBackgroundMode;
    window.setBackgroundMode = function (mode) {
        if (mode === 'video') {
            state.backgroundMode = 'video';
            localStorage.setItem('bgMode', 'video');
            document.documentElement.classList.remove('bg-mode-colorful', 'bg-mode-glass', 'bg-mode-custom');
            document.documentElement.classList.add('bg-mode-video');
            showToast('请选择本地视频文件', 'info');
            const input = document.createElement('input');
            input.type = 'file'; input.accept = 'video/*';
            input.onchange = (e) => {
                const f = e.target.files?.[0]; if (!f) return;
                const url = URL.createObjectURL(f);
                VideoBG.apply(url);
                state.customVideoBg = url;
                localStorage.setItem('customVideoBg', url);
                showToast('视频背景已应用', 'success');
            };
            input.click();
        } else {
            VideoBG.clear();
            localStorage.removeItem('customVideoBg');
            return _orig.apply(this, arguments);
        }
    };
    // 启动时恢复视频背景
    document.addEventListener('DOMContentLoaded', () => {
        const saved = localStorage.getItem('customVideoBg');
        if (saved && localStorage.getItem('bgMode') === 'video') VideoBG.apply(saved);
    });
})();

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
        const file = e.target.files?.[0]; if (!file) return;
        if (!file.type.startsWith('image/')) return _orig.call(this, e);
        ImgEditor.open(file, (edited) => {
            const dt = new DataTransfer(); dt.items.add(edited);
            const fakeEv = { target: { files: dt.files } };
            _orig.call(this, fakeEv);
            showToast('已应用编辑后的图片', 'success');
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
        } else if (key === 'effect') {
            if (typeof setPhoneEffect === 'function' && value !== state.effect) setPhoneEffect(value);
        } else if (key === 'theme') {
            if (typeof setDarkMode === 'function' && ((value === 'dark') !== (state.theme === 'dark'))) setDarkMode(value === 'dark');
        } else if (key === 'backgroundMode') {
            if (typeof setBackgroundMode === 'function' && value !== state.backgroundMode) setBackgroundMode(value);
        } else if (key === 'glassEnabled') {
            if (typeof toggleGlass === 'function') toggleGlass(!!value);
        } else if (key === 'effectEnabled') {
            if (typeof toggleEffectEnabled === 'function') toggleEffectEnabled(!!value);
        } else if (key === 'techMode') {
            if (typeof applyTechMode === 'function') applyTechMode(!!value);
        } else if (key === 'customTheme') {
            state.customTheme = value; localStorage.setItem('customTheme', JSON.stringify(value));
            if (typeof applyCustomGradientVars === 'function') applyCustomGradientVars();
        }
    } catch (e) { console.warn('[applyServerPref]', key, e); }
};
document.addEventListener('DOMContentLoaded', () => {
    ['theme_qq','effect','theme','backgroundMode','glassEnabled','effectEnabled','techMode','customTheme'].forEach(k => _loadServerPref(k));
});
