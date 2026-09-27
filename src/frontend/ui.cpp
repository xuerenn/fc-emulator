// ui.cpp — 现代化 UI 基础层实现
//
// 抗锯齿圆角的关键在 maskTex()：用圆角矩形的有符号距离场（SDF）算出每个像素的
// 覆盖度，写进一张纯 alpha 遮罩纹理。因为遮罩与颜色无关，换 hover 色只需
// SetTextureColorMod，不必重新生成几何，缓存也不会因颜色变化而膨胀。
#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fc {
namespace ui {

namespace {

constexpr float kPi = 3.14159265358979f;

// 各角色的字号（像素）。改动这里等于整体调整信息层级。
constexpr int kFontSizes[int(Font::COUNT)] = {
    34,   // Display
    21,   // Title
    16,   // Body
    13,   // Small
    15,   // Mono
};

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline u8 toU8(float v) {
    const float x = v * 255.0f + 0.5f;
    return u8(x < 0 ? 0 : (x > 255 ? 255 : x));
}

} // namespace

// ---------------------------------------------------------------- 颜色
RGBA mix(const RGBA& a, const RGBA& b, float t) {
    t = clampf(t, 0.f, 1.f);
    return RGBA{
        u8(float(a.r) + (float(b.r) - float(a.r)) * t),
        u8(float(a.g) + (float(b.g) - float(a.g)) * t),
        u8(float(a.b) + (float(b.b) - float(a.b)) * t),
        u8(float(a.a) + (float(b.a) - float(a.a)) * t),
    };
}

RGBA fade(const RGBA& c, float k) {
    return RGBA{ c.r, c.g, c.b, toU8(clampf(float(c.a) / 255.0f * k, 0.f, 1.f)) };
}

// ---------------------------------------------------------------- 控件动画
namespace {
std::unordered_map<std::string, float> g_anims;
}

float animStep(const std::string& id, bool active, double dt, float speed) {
    float& v = g_anims[id];
    v = Ui::approach(v, active ? 1.0f : 0.0f, speed, dt);
    // 夹到端点，避免长尾导致浮点值永远差一点点
    if (v < 0.002f) v = 0.0f;
    if (v > 0.998f) v = 1.0f;
    return v;
}

void resetAnimSteps() { g_anims.clear(); }

// ---------------------------------------------------------------- 生命周期
Ui::~Ui() { shutdown(); }

bool Ui::init(SDL_Renderer* ren) {
    ren_ = ren;
    if (!ren_) { diag_ = "renderer 为空"; return false; }

    if (TTF_WasInit() == 0 && TTF_Init() != 0) {
        diag_ = std::string("TTF_Init 失败: ") + TTF_GetError();
        return false;
    }

    // 候选字体链：前四个都带完整简体中文字形，最后一个是纯英文兜底。
    // 微软雅黑有两个 face（雅黑 / 雅黑 UI），用 index 分别尝试。
    struct Cand { const char* path; int index; };
    const Cand cands[] = {
        { "C:/Windows/Fonts/msyh.ttc",    0 },
        { "C:/Windows/Fonts/msyh.ttc",    1 },
        { "C:/Windows/Fonts/Deng.ttf",    0 },
        { "C:/Windows/Fonts/simhei.ttf",  0 },
        { "C:/Windows/Fonts/simsun.ttc",  0 },
        { "C:/Windows/Fonts/segoeui.ttf", 0 },
    };

    std::string mainPath;
    int mainIndex = 0;
    for (const Cand& c : cands) {
        if (TTF_Font* probe = TTF_OpenFontIndex(c.path, kFontSizes[1], c.index)) {
            TTF_CloseFont(probe);
            mainPath  = c.path;
            mainIndex = c.index;
            break;
        }
    }
    if (mainPath.empty()) {
        diag_ = "系统里找不到可用字体（已尝试: 微软雅黑 / 等线 / 黑体 / 宋体 / Segoe UI）";
        return false;
    }
    fontPath_ = mainPath;

    for (int i = 0; i < int(Font::COUNT); ++i) {
        fonts_[i] = TTF_OpenFontIndex(mainPath.c_str(), kFontSizes[i], mainIndex);
        if (!fonts_[i]) {
            diag_ = std::string("打开字体失败: ") + TTF_GetError();
            shutdown();
            return false;
        }
    }

    // 等宽角色换 Consolas：IP、端口、哈希这些需要字符等宽才好对齐。没有就沿用主字体。
    if (TTF_Font* mono = TTF_OpenFont("C:/Windows/Fonts/consola.ttf", kFontSizes[int(Font::Mono)])) {
        TTF_CloseFont(fonts_[int(Font::Mono)]);
        fonts_[int(Font::Mono)] = mono;
    }

    fontsOk_ = true;
    return true;
}

void Ui::shutdown() {
    for (TTF_Font*& f : fonts_) {
        if (f) { TTF_CloseFont(f); f = nullptr; }
    }
    for (auto& kv : masks_) SDL_DestroyTexture(kv.second);
    masks_.clear();
    for (auto& kv : texts_) SDL_DestroyTexture(kv.second);
    texts_.clear();
    fontsOk_ = false;
    ren_ = nullptr;
}

int Ui::lineHeight(Font role) const {
    TTF_Font* f = fonts_[int(role)];
    return f ? TTF_FontHeight(f) : 0;
}

int Ui::ascent(Font role) const {
    TTF_Font* f = fonts_[int(role)];
    return f ? TTF_FontAscent(f) : 0;
}

// ---------------------------------------------------------------- 遮罩纹理
SDL_Texture* Ui::maskTex(int w, int h, int rad, int thickness) {
    if (!ren_ || w <= 0 || h <= 0) return nullptr;
    rad = std::max(0, rad);
    rad = std::min(rad, std::min(w, h) / 2);
    thickness = std::max(0, thickness);
    if (thickness > 0) thickness = std::min(thickness, std::min(w, h) / 2);

    const u64 key = (u64(u32(w)) << 42) | (u64(u32(h)) << 22)
                  | (u64(u32(rad)) << 11) | u64(u32(thickness));
    auto it = masks_.find(key);
    if (it != masks_.end()) return it->second;

    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return nullptr;

    // SDL2 里 Surface 默认就在内存里、可直接访问；Lock 只是为兼容 RLE 加速表面。
    SDL_LockSurface(surf);
    u32* px = static_cast<u32*>(surf->pixels);

    const float hw = float(w) * 0.5f;
    const float hh = float(h) * 0.5f;
    const float r  = float(rad);
    const float th = float(thickness);

    for (int y = 0; y < h; ++y) {
        const float py = float(y) + 0.5f - hh;
        const float qy = std::fabs(py) - (hh - r);
        for (int x = 0; x < w; ++x) {
            const float px_ = float(x) + 0.5f - hw;
            const float qx  = std::fabs(px_) - (hw - r);
            const float dx  = std::max(qx, 0.0f);
            const float dy  = std::max(qy, 0.0f);
            // 圆角矩形 SDF：负值在内、正值在外，0 恰好在边界上
            const float d = std::sqrt(dx * dx + dy * dy)
                          + std::min(std::max(qx, qy), 0.0f) - r;
            // 描边就是把 SDF 折成「到边界线的距离」，再做 1px 宽的覆盖过渡
            const float sdf = (th > 0.0f) ? (std::fabs(d) - th * 0.5f) : d;
            const u8 a = toU8(clampf(0.5f - sdf, 0.0f, 1.0f));
            px[y * w + x] = (u32(a) << 24) | 0x00FFFFFFu;
        }
    }
    SDL_UnlockSurface(surf);

    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren_, surf);
    SDL_FreeSurface(surf);
    if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    // 先清理再插入：清理会销毁整个缓存，若放在插入之后就等于把刚建好、
    // 正要返回给调用方的纹理一起销毁 —— 调用方随后拿悬空指针去 RenderCopy 会崩。
    purgeIfNeeded();
    masks_[key] = tex;
    return tex;
}

void Ui::purgeIfNeeded() {
    if (masks_.size() > 512) {
        for (auto& kv : masks_) SDL_DestroyTexture(kv.second);
        masks_.clear();
    }
    if (texts_.size() > 1024) {
        for (auto& kv : texts_) SDL_DestroyTexture(kv.second);
        texts_.clear();
    }
}

// ---------------------------------------------------------------- 基础图形
void Ui::clear(const RGBA& c) {
    SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, c.a);
    SDL_RenderClear(ren_);
}

void Ui::fill(const SDL_Rect& r, const RGBA& c) {
    if (r.w <= 0 || r.h <= 0) return;
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(ren_, &r);
}

void Ui::fill(int x, int y, int w, int h, const RGBA& c) {
    const SDL_Rect r{x, y, w, h};
    fill(r, c);
}

void Ui::round(const SDL_Rect& r, int rad, const RGBA& c) {
    if (r.w <= 0 || r.h <= 0) return;
    SDL_Texture* t = maskTex(r.w, r.h, rad, 0);
    if (!t) return;
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
    SDL_RenderCopy(ren_, t, nullptr, &r);
}

void Ui::roundOutline(const SDL_Rect& r, int rad, int thickness, const RGBA& c) {
    if (r.w <= 0 || r.h <= 0 || thickness <= 0) return;
    SDL_Texture* t = maskTex(r.w, r.h, rad, thickness);
    if (!t) return;
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
    SDL_RenderCopy(ren_, t, nullptr, &r);
}

void Ui::shadow(const SDL_Rect& r, int rad, int depth, int alpha) {
    if (depth <= 0) return;
    // 由外向内叠加：外圈最淡，靠近主体处略深。偏移往下，符合常见光照直觉。
    for (int i = depth; i >= 1; --i) {
        const float k = 1.0f - float(i) / float(depth + 1);
        const int a = int(float(alpha) * k * k * 0.30f);
        if (a <= 0) continue;
        const int grow = i;
        const SDL_Rect rr{ r.x - grow, r.y - grow + grow / 3, r.w + grow * 2, r.h + grow * 2 };
        round(rr, rad + grow, rgba(0, 0, 0, u8(a)));
    }
}

void Ui::gradientV(const SDL_Rect& r, const RGBA& top, const RGBA& bottom) {
    if (r.w <= 0 || r.h <= 0) return;
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < r.h; ++i) {
        const float k = (r.h > 1) ? float(i) / float(r.h - 1) : 0.0f;
        const RGBA c = mix(top, bottom, k);
        SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, c.a);
        const SDL_Rect row{ r.x, r.y + i, r.w, 1 };
        SDL_RenderFillRect(ren_, &row);
    }
}

void Ui::gradientH(const SDL_Rect& r, const RGBA& left, const RGBA& right) {
    if (r.w <= 0 || r.h <= 0) return;
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < r.w; ++i) {
        const float k = (r.w > 1) ? float(i) / float(r.w - 1) : 0.0f;
        const RGBA c = mix(left, right, k);
        SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, c.a);
        const SDL_Rect col{ r.x + i, r.y, 1, r.h };
        SDL_RenderFillRect(ren_, &col);
    }
}

void Ui::hline(int x, int y, int w, const RGBA& c) { fill(x, y, w, 1, c); }
void Ui::vline(int x, int y, int h, const RGBA& c) { fill(x, y, 1, h, c); }

void Ui::rectOutline(const SDL_Rect& r, const RGBA& c) {
    hline(r.x, r.y, r.w, c);
    hline(r.x, r.y + r.h - 1, r.w, c);
    vline(r.x, r.y, r.h, c);
    vline(r.x + r.w - 1, r.y, r.h, c);
}

// ---------------------------------------------------------------- 几何
void Ui::tri(int x1, int y1, int x2, int y2, int x3, int y3, const RGBA& c) {
    SDL_Vertex v[3];
    const SDL_Color sc{ c.r, c.g, c.b, c.a };
    const float xs[3] = { float(x1), float(x2), float(x3) };
    const float ys[3] = { float(y1), float(y2), float(y3) };
    for (int i = 0; i < 3; ++i) {
        v[i].position  = SDL_FPoint{ xs[i], ys[i] };
        v[i].color     = sc;
        v[i].tex_coord = SDL_FPoint{ 0.f, 0.f };
    }
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(ren_, nullptr, v, 3, nullptr, 0);
}

void Ui::thickLine(int x1, int y1, int x2, int y2, int w, const RGBA& c) {
    const float dx = float(x2 - x1), dy = float(y2 - y1);
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6f || w <= 0) return;
    // 法线方向偏移半个线宽，把线段扩成一个四边形
    const float nx = -dy / len * (float(w) * 0.5f);
    const float ny =  dx / len * (float(w) * 0.5f);

    const SDL_Color sc{ c.r, c.g, c.b, c.a };
    SDL_Vertex v[4];
    const float pts[4][2] = {
        { float(x1) + nx, float(y1) + ny },
        { float(x2) + nx, float(y2) + ny },
        { float(x2) - nx, float(y2) - ny },
        { float(x1) - nx, float(y1) - ny },
    };
    for (int i = 0; i < 4; ++i) {
        v[i].position  = SDL_FPoint{ pts[i][0], pts[i][1] };
        v[i].color     = sc;
        v[i].tex_coord = SDL_FPoint{ 0.f, 0.f };
    }
    const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(ren_, nullptr, v, 4, idx, 6);
}

void Ui::ring(int cx, int cy, float rad, int thickness, const RGBA& c,
              float fromDeg, float toDeg) {
    constexpr float kStep = 4.0f;
    for (float a = fromDeg; a < toDeg; a += kStep) {
        const float a0 = a * kPi / 180.0f;
        const float a1 = std::min(a + kStep, toDeg) * kPi / 180.0f;
        thickLine(int(float(cx) + std::cos(a0) * rad), int(float(cy) + std::sin(a0) * rad),
                  int(float(cx) + std::cos(a1) * rad), int(float(cy) + std::sin(a1) * rad),
                  thickness, c);
    }
}

// ---------------------------------------------------------------- 文字
SDL_Texture* Ui::textTex(Font role, const std::string& s, const RGBA& c, int* ow, int* oh) {
    TTF_Font* f = fonts_[int(role)];
    if (!f || s.empty()) return nullptr;

    std::string key;
    key.reserve(s.size() + 24);
    key += char('0' + int(role));
    key += '|';
    key += char(c.r); key += char(c.g); key += char(c.b); key += char(c.a);
    key += '|';
    key += s;

    auto it = texts_.find(key);
    if (it != texts_.end()) {
        if (ow || oh) SDL_QueryTexture(it->second, nullptr, nullptr, ow, oh);
        return it->second;
    }

    const SDL_Color sc{ c.r, c.g, c.b, 255 };
    SDL_Surface* surf = TTF_RenderUTF8_Blended(f, s.c_str(), sc);
    if (!surf) return nullptr;

    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren_, surf);
    if (ow) *ow = surf->w;
    if (oh) *oh = surf->h;
    SDL_FreeSurface(surf);
    if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    purgeIfNeeded();
    texts_[key] = tex;
    return tex;
}

int Ui::measure(Font role, const std::string& utf8) const {
    TTF_Font* f = fonts_[int(role)];
    if (!f || utf8.empty()) return 0;
    int w = 0, h = 0;
    TTF_SizeUTF8(f, utf8.c_str(), &w, &h);
    return w;
}

int Ui::text(Font role, const std::string& utf8, int x, int y, const RGBA& c) {
    if (utf8.empty()) return 0;
    int w = 0, h = 0;
    SDL_Texture* t = textTex(role, utf8, c, &w, &h);
    if (!t) return 0;
    const SDL_Rect dst{ x, y, w, h };
    SDL_RenderCopy(ren_, t, nullptr, &dst);
    return w;
}

std::string Ui::clip(Font role, const std::string& utf8, int maxW) const {
    if (maxW <= 0 || measure(role, utf8) <= maxW) return utf8;
    const std::string ell = "…";
    if (measure(role, ell) > maxW) return "";
    std::string out = utf8;
    while (!out.empty()) {
        // 按 UTF-8 字符边界回退，避免把多字节字符截成半个
        out.pop_back();
        while (!out.empty() && (u8(out.back()) & 0xC0) == 0x80) out.pop_back();
        if (measure(role, out + ell) <= maxW) return out + ell;
    }
    return ell;
}

int Ui::textClipped(Font role, const std::string& utf8, int x, int y, int maxW, const RGBA& c) {
    return text(role, clip(role, utf8, maxW), x, y, c);
}

void Ui::textVCenter(Font role, const std::string& utf8, int x, const SDL_Rect& box, const RGBA& c) {
    const int lh = lineHeight(role);
    text(role, utf8, x, box.y + (box.h - lh) / 2, c);
}

void Ui::textCenter(Font role, const std::string& utf8, const SDL_Rect& box, const RGBA& c) {
    const int lh = lineHeight(role);
    const int w  = measure(role, utf8);
    text(role, utf8, box.x + (box.w - w) / 2, box.y + (box.h - lh) / 2, c);
}

void Ui::textCenterClipped(Font role, const std::string& utf8, const SDL_Rect& box, const RGBA& c) {
    textCenter(role, clip(role, utf8, box.w), box, c);
}

void Ui::textRight(Font role, const std::string& utf8, int rightX, int y, const RGBA& c) {
    text(role, utf8, rightX - measure(role, utf8), y, c);
}

// ---------------------------------------------------------------- 缓动
float Ui::easeOutCubic(float t) {
    t = clampf(t, 0.f, 1.f);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

float Ui::easeInOutCubic(float t) {
    t = clampf(t, 0.f, 1.f);
    return t < 0.5f ? 4.0f * t * t * t
                    : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
}

float Ui::approach(float cur, float target, float speed, double dt) {
    // 指数趋近：每帧按 (1 - e^{-speed*dt}) 的比例补齐差值，因此与帧率无关。
    const float k = 1.0f - std::exp(-speed * float(dt));
    return cur + (target - cur) * k;
}

} // namespace ui
} // namespace fc
