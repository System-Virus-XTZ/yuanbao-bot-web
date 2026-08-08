/**
 * glass.h - Apple Liquid Glass 着色器引擎
 *
 * 三层合成架构：
 *   CSS层：backdrop-filter: blur() + 伪元素光泽
 *   GLSL层：SDF形状定义 + 背景折射 + 色散 + 镜面高光
 *   物理层：弹簧光源追踪 + GPU加速
 *
 * 依赖：无（纯头文件，可直接 include）
 */
#pragma once

#include <string>
#include <sstream>
#include <cmath>
#include <vector>
#include <cstdint>
#include <algorithm>

namespace glass {

// ═══════════════════════════════════════════════
//  着色器参数
// ═══════════════════════════════════════════════
struct ShaderParams {
    // 基础参数
    float intensity     = 0.8f;
    float noise_scale   = 3.0f;
    float warp_strength = 0.015f;
    float chromatic     = 0.008f;

    // 颜色调整
    float saturation    = 1.1f;
    float brightness    = 1.05f;

    // 边框
    float border_width  = 1.2f;
    float border_fuzz   = 0.3f;
    float inner_glow    = 0.5f;

    // 边框颜色 (RGB)
    float border_r = 0.9f, border_g = 0.9f, border_b = 0.95f;

    // 辉光颜色 (RGB)
    float glow_r = 0.6f, glow_g = 0.7f, glow_b = 1.0f;

    ShaderParams() = default;

    // 从 JSON 解析参数
    static ShaderParams from_json(const std::string& json) {
        ShaderParams p;
        auto getf = [&](const std::string& k, float def) -> float {
            size_t pos = json.find("\"" + k + "\"");
            if (pos == std::string::npos) return def;
            size_t colon = json.find(":", pos);
            if (colon == std::string::npos) return def;
            size_t start = colon + 1;
            while (start < json.size() &&
                   (json[start] == ' ' || json[start] == '\t' || json[start] == ','))
                start++;
            size_t end = start;
            while (end < json.size() &&
                   ((json[end] >= '0' && json[end] <= '9') ||
                    json[end] == '.' || json[end] == '-'))
                end++;
            if (end > start)
                return std::stof(json.substr(start, end - start));
            return def;
        };
        p.intensity     = getf("intensity", 0.8f);
        p.noise_scale   = getf("noise_scale", 3.0f);
        p.warp_strength = getf("warp_strength", 0.015f);
        p.chromatic     = getf("chromatic", 0.008f);
        p.saturation    = getf("saturation", 1.1f);
        p.brightness    = getf("brightness", 1.05f);
        p.border_width  = getf("border_width", 1.2f);
        p.border_fuzz   = getf("border_fuzz", 0.3f);
        p.inner_glow    = getf("inner_glow", 0.5f);
        p.border_r = getf("border_r", 0.9f);
        p.border_g = getf("border_g", 0.9f);
        p.border_b = getf("border_b", 0.95f);
        p.glow_r   = getf("glow_r", 0.6f);
        p.glow_g   = getf("glow_g", 0.7f);
        p.glow_b   = getf("glow_b", 1.0f);
        return p;
    }
};

// ═══════════════════════════════════════════════
//  预设枚举
// ═══════════════════════════════════════════════
enum class GlassPreset {
    DEFAULT = 0, NEON, CRYSTAL, AURORA, MATRIX,
    RETRO, CYBER, HOLOGRAPHIC, GLASSMORPH, FLUID
};

// ═══════════════════════════════════════════════
//  着色器编译引擎
// ═══════════════════════════════════════════════
class GlassShaderEngine {
public:
    // 顶点着色器
    static std::string compile_vertex() {
        return R"(#version 300 es
precision highp float;
in vec2 aPosition;
out vec2 vTexCoord;
out vec2 vPosition;
void main() {
    vTexCoord = aPosition * 0.5 + 0.5;
    vPosition = aPosition;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";
    }

    // 高斯模糊着色器（双 pass）
    static std::string compile_blur() {
        return R"(#version 300 es
precision highp float;
uniform sampler2D uTexture;
uniform vec2 uDirection;
uniform float uRadius;
in vec2 vTexCoord;
out vec4 fragColor;

void main() {
    vec4 sum = vec4(0.0);
    float total = 0.0;
    float sigma = uRadius * 0.5;
    for (float i = -24.0; i <= 24.0; i += 1.0) {
        float w = exp(-0.5 * (i * i) / (sigma * sigma + 1.0));
        vec2 offset = uDirection * i;
        sum += texture(uTexture, vTexCoord + offset) * w;
        total += w;
    }
    fragColor = sum / total;
}
)";
    }

    // 最终合成着色器
    static std::string compile_composite() {
        return R"(#version 300 es
precision highp float;
uniform sampler2D uGlass;
uniform sampler2D uContent;
uniform float uOpacity;
in vec2 vTexCoord;
out vec4 fragColor;

void main() {
    vec4 glass = texture(uGlass, vTexCoord);
    vec4 content = texture(uContent, vTexCoord);
    fragColor = mix(content, glass, uOpacity);
    fragColor.a = 1.0;
}
)";
    }

    // 主 Fragment Shader（液态玻璃核心）
    // 架构: SDF形状 → 背景折射 → 色散 → 镜面高光 → 边缘辉光
    static std::string compile_fragment(const ShaderParams& /*params*/) {
        return R"(#version 300 es
precision highp float;
precision highp int;

uniform sampler2D uTexture;
uniform sampler2D uBgTexture;
uniform float uTime;
uniform vec2 uResolution;
uniform float uIntensity;
uniform float uNoiseScale;
uniform float uWarpStrength;
uniform float uChromatic;
uniform float uSaturation;
uniform float uBrightness;
uniform float uBorderWidth;
uniform float uBorderFuzz;
uniform vec3 uBorderColor;
uniform vec3 uGlowColor;
uniform float uInnerGlow;
uniform vec2 uLightPos;
uniform vec2 uCornerRadius;

in vec2 vTexCoord;
in vec2 vPosition;
out vec4 fragColor;

// 噪声函数
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    mat2 rot = mat2(cos(0.5), sin(0.5), -sin(0.5), cos(0.5));
    for (int i = 0; i < 5; i++) {
        v += a * noise(p);
        p = rot * p * 2.0;
        a *= 0.5;
    }
    return v;
}

// SDF 圆角矩形
float sdRoundedBox(vec2 p, vec2 b, float r) {
    vec2 d = abs(p) - b + r;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
}

// 主效果函数
vec3 liquidGlass(vec2 uv, vec2 pos) {
    float t = uTime * 0.3;

    // 1. 噪声扭曲（液体流动感）
    vec2 warp = vec2(
        fbm(uv * uNoiseScale + vec2(t, 0.0)),
        fbm(uv * uNoiseScale + vec2(0.0, t) + 5.2)
    ) - 0.5;
    vec2 warpedUV = uv + warp * uWarpStrength * uIntensity;

    // 2. 色散 (Chromatic Aberration)
    float cs = uChromatic * uIntensity;
    vec2 dir = warpedUV - 0.5;
    vec2 offR = dir * cs * 1.0;
    vec2 offG = dir * cs * 0.5;
    vec2 offB = dir * cs * 0.3;

    float r = texture(uBgTexture, warpedUV + offR).r;
    float g = texture(uBgTexture, warpedUV + offG).g;
    float b = texture(uBgTexture, warpedUV + offB).b;
    vec3 bg = vec3(r, g, b);

    // 3. 饱和度与亮度
    float lum = dot(bg, vec3(0.299, 0.587, 0.114));
    bg = mix(vec3(lum), bg, uSaturation);
    bg *= uBrightness;

    // 4. 内部纹理噪声
    float edge = fbm(warpedUV * 8.0 + t * 0.2) * 0.12;
    bg += edge * uInnerGlow * uIntensity;

    // 5. SDF 边缘高光（玻璃厚度）
    float sdf = sdRoundedBox(pos, vec2(0.95, 0.95), 0.04);
    float edgeGlow = smoothstep(uBorderFuzz * 0.05, -uBorderFuzz * 0.05, sdf);
    bg = mix(bg, uBorderColor, edgeGlow * uBorderWidth * 0.5 * uIntensity);

    // 6. 镜面高光（光源反射）
    vec2 lightDir = uLightPos - uv;
    float specular = smoothstep(0.3, 0.0, length(lightDir));
    specular = pow(specular, 3.0) * 0.25;
    bg += uGlowColor * specular * uInnerGlow * uIntensity;

    // 7. 中心辉光
    vec2 center = uv - 0.5;
    float centerGlow = 1.0 - smoothstep(0.0, 0.6, length(center));
    bg += uGlowColor * centerGlow * uInnerGlow * 0.2 * uIntensity;

    // 8. 顶部渐变（环境光）
    float topGrad = (1.0 - uv.y) * 0.15;
    bg += vec3(topGrad) * 0.3;

    return bg;
}

void main() {
    vec2 uv = vTexCoord;
    vec2 pos = vPosition;
    vec3 result = liquidGlass(uv, pos);

    // SDF 裁剪确保边缘透明
    float sdf = sdRoundedBox(pos, vec2(0.98, 0.98), 0.03);
    float alpha = 1.0 - smoothstep(-0.02, 0.01, sdf);

    fragColor = vec4(result, alpha * 0.95);
}
)";
    }

    // ── 预设 ──
    static const char* preset_json(GlassPreset p) {
        static const char* presets[] = {
            // 0: Default
            R"({"intensity":0.75,"noise_scale":3.0,"warp_strength":0.012,"chromatic":0.006,"saturation":1.15,"brightness":1.05,"border_width":1.0,"border_fuzz":0.25,"border_r":0.92,"border_g":0.94,"border_b":0.98,"glow_r":0.65,"glow_g":0.75,"glow_b":1.0,"inner_glow":0.45})",
            // 1: Neon
            R"({"intensity":0.95,"noise_scale":2.0,"warp_strength":0.018,"chromatic":0.015,"saturation":1.5,"brightness":1.15,"border_width":1.4,"border_fuzz":0.35,"border_r":0.0,"border_g":0.9,"border_b":1.0,"glow_r":0.0,"glow_g":0.85,"glow_b":1.0,"inner_glow":0.65})",
            // 2: Crystal
            R"({"intensity":0.55,"noise_scale":4.5,"warp_strength":0.006,"chromatic":0.003,"saturation":1.25,"brightness":1.08,"border_width":0.7,"border_fuzz":0.18,"border_r":0.96,"border_g":0.98,"border_b":1.0,"glow_r":0.8,"glow_g":0.9,"glow_b":1.0,"inner_glow":0.35})",
            // 3: Aurora
            R"({"intensity":0.85,"noise_scale":1.5,"warp_strength":0.022,"chromatic":0.012,"saturation":1.4,"brightness":1.1,"border_width":1.8,"border_fuzz":0.45,"border_r":0.15,"border_g":0.85,"border_b":0.5,"glow_r":0.2,"glow_g":0.8,"glow_b":0.45,"inner_glow":0.75})",
            // 4: Matrix
            R"({"intensity":0.9,"noise_scale":2.8,"warp_strength":0.028,"chromatic":0.025,"saturation":0.6,"brightness":0.95,"border_width":0.9,"border_fuzz":0.28,"border_r":0.0,"border_g":0.95,"border_b":0.15,"glow_r":0.0,"glow_g":0.9,"glow_b":0.1,"inner_glow":0.55})",
            // 5: Retro
            R"({"intensity":0.45,"noise_scale":5.5,"warp_strength":0.004,"chromatic":0.002,"saturation":0.85,"brightness":1.0,"border_width":0.9,"border_fuzz":0.28,"border_r":0.95,"border_g":0.65,"border_b":0.2,"glow_r":0.95,"glow_g":0.6,"glow_b":0.15,"inner_glow":0.25})",
            // 6: Cyber
            R"({"intensity":1.0,"noise_scale":2.2,"warp_strength":0.016,"chromatic":0.018,"saturation":1.45,"brightness":1.12,"border_width":1.6,"border_fuzz":0.38,"border_r":0.0,"border_g":0.45,"border_b":0.95,"glow_r":0.15,"glow_g":0.45,"glow_b":1.0,"inner_glow":0.68})",
            // 7: Holographic
            R"({"intensity":1.0,"noise_scale":1.0,"warp_strength":0.03,"chromatic":0.022,"saturation":1.7,"brightness":1.15,"border_width":2.2,"border_fuzz":0.48,"border_r":0.75,"border_g":0.15,"border_b":0.95,"glow_r":0.65,"glow_g":0.25,"glow_b":0.9,"inner_glow":0.85})",
            // 8: Glassmorph
            R"({"intensity":0.35,"noise_scale":7.0,"warp_strength":0.003,"chromatic":0.001,"saturation":1.05,"brightness":1.02,"border_width":0.4,"border_fuzz":0.1,"border_r":0.98,"border_g":0.99,"border_b":1.0,"glow_r":0.95,"glow_g":0.97,"glow_b":1.0,"inner_glow":0.15})",
            // 9: Fluid
            R"({"intensity":1.0,"noise_scale":0.8,"warp_strength":0.04,"chromatic":0.01,"saturation":1.25,"brightness":1.08,"border_width":1.3,"border_fuzz":0.35,"border_r":0.25,"border_g":0.55,"border_b":0.85,"glow_r":0.35,"glow_g":0.65,"glow_b":1.0,"inner_glow":0.75})",
        };
        return presets[(int)p];
    }

    // 获取预设参数
    static ShaderParams get_preset_params(GlassPreset p) {
        return ShaderParams::from_json(preset_json(p));
    }

    // 预设名称列表
    static const char* preset_name(GlassPreset p) {
        static const char* names[] = {
            "default", "neon", "crystal", "aurora", "matrix",
            "retro", "cyber", "holographic", "glassmorph", "fluid"
        };
        return names[(int)p];
    }

    // 预设总数
    static constexpr int preset_count() { return 10; }

    // ── 编译完整着色器包 ──
    static std::string compile_full(const std::string& params_json) {
        auto params = ShaderParams::from_json(params_json);
        std::string frag = compile_fragment(params);
        std::string vert = compile_vertex();
        std::string blur = compile_blur();
        std::string comp = compile_composite();

        std::ostringstream oss;
        oss << "{\"fragment_shader\":" << json_escape(frag)
            << ",\"vertex_shader\":" << json_escape(vert)
            << ",\"blur_shader\":" << json_escape(blur)
            << ",\"composite_shader\":" << json_escape(comp) << "}";
        return oss.str();
    }

    // 编译预设
    static std::string compile_preset(const std::string& name) {
        GlassPreset p = GlassPreset::DEFAULT;
        if (name == "neon") p = GlassPreset::NEON;
        else if (name == "crystal") p = GlassPreset::CRYSTAL;
        else if (name == "aurora") p = GlassPreset::AURORA;
        else if (name == "matrix") p = GlassPreset::MATRIX;
        else if (name == "retro") p = GlassPreset::RETRO;
        else if (name == "cyber") p = GlassPreset::CYBER;
        else if (name == "holographic") p = GlassPreset::HOLOGRAPHIC;
        else if (name == "glassmorph") p = GlassPreset::GLASSMORPH;
        else if (name == "fluid") p = GlassPreset::FLUID;

        auto params = get_preset_params(p);
        std::string frag = compile_fragment(params);

        std::ostringstream oss;
        oss << "{\"name\":\"" << name << "\",\"params\":" << preset_json(p)
            << ",\"fragment_shader\":" << json_escape(frag) << "}";
        return oss.str();
    }

    // 所有预设列表
    static std::string all_presets_json() {
        std::ostringstream oss;
        oss << "[";
        for (int i = 0; i < preset_count(); i++) {
            if (i) oss << ",";
            oss << "{\"name\":\"" << preset_name((GlassPreset)i)
                << "\",\"json\":" << preset_json((GlassPreset)i) << "}";
        }
        oss << "]";
        return oss.str();
    }

private:
    // JSON 字符串转义
    static std::string json_escape(const std::string& s) {
        std::ostringstream oss;
        oss << '"';
        for (char c : s) {
            if (c == '"') oss << "\\\"";
            else if (c == '\\') oss << "\\\\";
            else if (c == '\n') oss << "\\n";
            else if (c == '\r') oss << "\\r";
            else if (c == '\t') oss << "\\t";
            else oss << c;
        }
        oss << '"';
        return oss.str();
    }
};

// ═══════════════════════════════════════════════
//  基础工具（来自 class.h）
// ═══════════════════════════════════════════════
static inline float clamp01f(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

static inline int clamp_byte(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/** smoothstep（与 shader 一致） */
static inline float smooth_step(float a, float b, float t) {
    float n = clamp01f((t - a) / (b - a));
    return n * n * (3.f - 2.f * n);
}

/** 2D 长度 */
static inline float length2f(float x, float y) { return std::sqrt(x * x + y * y); }

/** 圆角矩形 SDF */
static inline float rounded_rect_sdf(float x, float y, float halfW, float halfH, float radius) {
    float qx = std::fabs(x) - halfW + radius;
    float qy = std::fabs(y) - halfH + radius;
    return std::min(std::max(qx, qy), 0.f) + length2f(std::max(qx, 0.f), std::max(qy, 0.f)) - radius;
}

/** 轴对齐矩形"方角距离场"（L∞） */
static inline float rect_sdf_linf(float x, float y, float halfW, float halfH) {
    return std::max(std::fabs(x) - halfW, std::fabs(y) - halfH);
}

/** 椭圆近似 SDF */
static inline float ellipse_sdf(float x, float y, float rx, float ry) {
    float a = std::max(1e-6f, rx), b = std::max(1e-6f, ry);
    float px = std::fabs(x), py = std::fabs(y);
    if (px < 1e-6f && py < 1e-6f) return -std::min(a, b);
    float invA = 1.f / a, invB = 1.f / b;
    float nx = px * invA, ny = py * invB;
    float f = nx * nx + ny * ny - 1.f;
    float gx = px * invA * invA, gy = py * invB * invB;
    float grad = 2.f * std::max(1e-6f, std::sqrt(gx * gx + gy * gy));
    return f / grad;
}

// ═══════════════════════════════════════════════
//  极简 PNG 编码器（stored-deflate，无 zlib 依赖）
// ═══════════════════════════════════════════════
namespace png {

static inline uint32_t crc32_table() {
    // 单字节 CRC 查表可运行时算；此处用标准逐字节算法即可，无需大表。
    return 0;
}

static uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            uint32_t mask = (crc & 1u) ? 0xEDB88320u : 0u;
            crc = (crc >> 1) ^ mask;
        }
    }
    return ~crc;
}

static uint32_t adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static inline void put_u32_be(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((uint8_t)(v >> 24));
    out.push_back((uint8_t)(v >> 16));
    out.push_back((uint8_t)(v >> 8));
    out.push_back((uint8_t)v);
}

static inline void put_u16_le(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back((uint8_t)(v & 0xFF));
    out.push_back((uint8_t)(v >> 8));
}

static void append_chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put_u32_be(out, (uint32_t)data.size());
    size_t typePos = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    uint32_t crc = crc32(out.data() + typePos, out.size() - typePos);
    put_u32_be(out, crc);
}

/** zlib stored-deflate 包装（含 zlib 头与 Adler32） */
static std::vector<uint8_t> zlib_stored(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out;
    out.push_back(0x78); out.push_back(0x01);  // zlib header（CMF/FLG）
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t chunk = std::min<size_t>(raw.size() - pos, 65535);
        bool final = (pos + chunk == raw.size());
        out.push_back(final ? 0x01 : 0x00);   // BFINAL:1 BTYPE:00
        put_u16_le(out, (uint16_t)chunk);
        put_u16_le(out, (uint16_t)(~chunk & 0xFFFF));
        out.insert(out.end(), raw.begin() + pos, raw.begin() + pos + chunk);
        pos += chunk;
    }
    uint32_t ad = adler32(raw.data(), raw.size());
    put_u32_be(out, ad);
    return out;
}

/** RGBA 像素 → PNG 文件字节 */
static std::vector<uint8_t> encode_rgba(int width, int height, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> out;
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    out.insert(out.end(), sig, sig + 8);

    // IHDR
    std::vector<uint8_t> ihdr;
    put_u32_be(ihdr, (uint32_t)width);
    put_u32_be(ihdr, (uint32_t)height);
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(6);   // color type: RGBA
    ihdr.push_back(0);   // compression
    ihdr.push_back(0);   // filter
    ihdr.push_back(0);   // interlace
    append_chunk(out, "IHDR", ihdr);

    // IDAT：每行前加 filter byte 0
    std::vector<uint8_t> scanlines;
    scanlines.reserve((size_t)height * ((size_t)width * 4 + 1));
    for (int y = 0; y < height; y++) {
        scanlines.push_back(0);
        scanlines.insert(scanlines.end(),
                         rgba.begin() + (size_t)y * width * 4,
                         rgba.begin() + ((size_t)y + 1) * width * 4);
    }
    std::vector<uint8_t> idat = zlib_stored(scanlines);
    append_chunk(out, "IDAT", idat);

    append_chunk(out, "IEND", std::vector<uint8_t>());
    return out;
}

static std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string r;
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        r += b64[(v >> 18) & 63];
        r += b64[(v >> 12) & 63];
        r += b64[(v >> 6) & 63];
        r += b64[v & 63];
    }
    if (i < data.size()) {
        uint32_t v = data[i] << 16;
        r += b64[(v >> 18) & 63];
        r += b64[(v >> 12) & 63];
        r += (i + 1 < data.size()) ? b64[(v >> 6) & 63] : '=';
        r += '=';
    }
    return r;
}

} // namespace png

// ═══════════════════════════════════════════════
//  液态玻璃位移贴图生成器
//  通道约定：R = X 位移，B = Y 位移（对应 feDisplacementMap xChannelSelector=R / yChannelSelector=B）
// ═══════════════════════════════════════════════
class DisplacementMapGenerator {
public:
    /**
     * 生成位移贴图 PNG 的 data URL。
     * @param width/height 贴图尺寸（px）
     * @param flatAreaScale 平坦区大小（0~1，越大越贴近容器边缘）
     * @param edgeHardness  边缘硬度（越大过渡越短）
     * @param radiusX/radiusY 圆角半径（px）
     */
    static std::string generate_png_data_url(int width, int height,
                                             float flatAreaScale, float edgeHardness,
                                             float radiusX, float radiusY) {
        int w = std::max(1, width), h = std::max(1, height);
        std::vector<uint8_t> rgba((size_t)w * h * 4);

        float halfW = w * 0.5f, halfH = h * 0.5f;
        float rX = std::max(0.f, std::min(radiusX, halfW));
        float rY = std::max(0.f, std::min(radiusY, halfH));
        bool isEllipse = rX >= halfW - 0.5f && rY >= halfH - 0.5f;

        float flat = clamp01f(flatAreaScale);
        float maxInset = std::max(0.f, std::min(halfW, halfH) - 1.f);
        float insetPx = (1.f - flat) * maxInset;
        float innerHalfW = std::max(1.f, halfW - insetPx);
        float innerHalfH = std::max(1.f, halfH - insetPx);
        float corner = std::min(rX, rY);
        float innerRadius = std::max(0.f, corner * flat);
        innerRadius = std::min(innerRadius, std::min(innerHalfW, innerHalfH));
        float hardness = std::max(0.1f, edgeHardness);
        float transitionPx = std::max(1.f, insetPx / hardness);

        // 第一遍：计算原始位移并求 maxScale 归一化
        std::vector<float> dxs((size_t)w * h), dys((size_t)w * h);
        float maxScale = 0.f;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float uvx = (x + 0.5f) / w, uvy = (y + 0.5f) / h;
                float px = (uvx - 0.5f) * w, py = (uvy - 0.5f) * h;
                float dist;
                if (isEllipse)
                    dist = ellipse_sdf(px, py, innerHalfW, innerHalfH);
                else if (innerRadius <= 0.001f)
                    dist = rect_sdf_linf(px, py, innerHalfW, innerHalfH);
                else
                    dist = rounded_rect_sdf(px, py, innerHalfW, innerHalfH, innerRadius);
                float d = smooth_step(transitionPx, 0.f, dist);
                float scaled = smooth_step(0.f, 1.f, d);
                float newX = px * scaled + halfW, newY = py * scaled + halfH;
                float dx = newX - (x + 0.5f), dy = newY - (y + 0.5f);
                dxs[(size_t)y * w + x] = dx;
                dys[(size_t)y * w + x] = dy;
                maxScale = std::max(maxScale, std::fabs(dx));
                maxScale = std::max(maxScale, std::fabs(dy));
            }
        }
        maxScale = std::max(1.f, maxScale);

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                size_t idx = (size_t)y * w + x;
                // 边缘 2px 平滑，避免硬边
                int edgeDist = std::min({ x, y, w - x - 1, h - y - 1 });
                float edgeFactor = std::min(1.f, edgeDist / 2.f);
                float sx = dxs[idx] * edgeFactor, sy = dys[idx] * edgeFactor;
                float r = clamp01f(sx / maxScale + 0.5f);
                float b = clamp01f(sy / maxScale + 0.5f);
                size_t pi = idx * 4;
                rgba[pi]     = (uint8_t)clamp_byte((int)(r * 255.f));
                rgba[pi + 1] = (uint8_t)clamp_byte((int)(b * 255.f));  // G: Y 位移
                rgba[pi + 2] = (uint8_t)clamp_byte((int)(b * 255.f));  // B: Y 位移
                rgba[pi + 3] = 255;
            }
        }

        auto pngBytes = png::encode_rgba(w, h, rgba);
        return std::string("data:image/png;base64,") + png::base64_encode(pngBytes);
    }
};

// ═══════════════════════════════════════════════
//  边缘环 alpha 遮罩生成器（ring → center 平滑过渡）
// ═══════════════════════════════════════════════
class EdgeRingMaskGenerator {
public:
    static std::string generate_ring_png_data_url(int width, int height,
                                                  float radiusX, float radiusY,
                                                  float ringWidthPx, float innerFeatherPx) {
        int w = std::max(1, width), h = std::max(1, height);
        std::vector<uint8_t> rgba((size_t)w * h * 4);

        float halfW = w * 0.5f, halfH = h * 0.5f;
        float outerR = std::min(std::max(0.f, std::min(radiusX, halfW)),
                                std::max(0.f, std::min(radiusY, halfH)));
        bool isEllipse = std::min(radiusX, halfW) >= halfW - 0.5f && std::min(radiusY, halfH) >= halfH - 0.5f;
        float ring = std::max(0.f, ringWidthPx);
        float innerHalfW = std::max(1.f, halfW - ring);
        float innerHalfH = std::max(1.f, halfH - ring);
        float innerR = std::min(std::max(0.f, outerR - ring), std::min(innerHalfW, innerHalfH));
        float innerFeather = std::max(0.1f, innerFeatherPx);
        float outerFeather = 1.5f;

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float px = x + 0.5f - halfW, py = y + 0.5f - halfH;
                float outerSdf, innerSdf;
                if (isEllipse) {
                    outerSdf = ellipse_sdf(px, py, halfW, halfH);
                    innerSdf = ellipse_sdf(px, py, innerHalfW, innerHalfH);
                } else if (outerR <= 0.001f) {
                    outerSdf = rect_sdf_linf(px, py, halfW, halfH);
                    innerSdf = rect_sdf_linf(px, py, innerHalfW, innerHalfH);
                } else {
                    outerSdf = rounded_rect_sdf(px, py, halfW, halfH, outerR);
                    innerSdf = rounded_rect_sdf(px, py, innerHalfW, innerHalfH, innerR);
                }
                float outerInside = 1.f - smooth_step(0.f, outerFeather, outerSdf);
                float outsideInner = smooth_step(-innerFeather, 0.f, innerSdf);
                float alpha = clamp01f(outerInside * outsideInner);
                size_t pi = ((size_t)y * w + x) * 4;
                rgba[pi] = rgba[pi + 1] = rgba[pi + 2] = 255;
                rgba[pi + 3] = (uint8_t)clamp_byte((int)(alpha * 255.f));
            }
        }

        auto pngBytes = png::encode_rgba(w, h, rgba);
        return std::string("data:image/png;base64,") + png::base64_encode(pngBytes);
    }
};

} // namespace glass
