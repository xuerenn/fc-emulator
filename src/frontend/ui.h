// ui.h — 现代化 UI 基础层（矢量字体 + 抗锯齿图形 + 动效）
//
// 为什么需要单独一层：
//   原来的键位界面是在 256x240 的「游戏逻辑分辨率」里用 5x7 手写点阵字拼出来的。
//   那条路走到头也只能是像素风，做不出圆角、层次、平滑字重。要做现代观感，
//   必须先解决两件事：
//     1) 绘制用窗口真实像素坐标，而不是游戏画面的 256x240 虚拟坐标；
//     2) 文字用矢量字体（SDL2_ttf）渲染，而不是手写点阵。
//   这一层把这两件事封装好，上层只需要「画一张圆角卡片 + 写一行字」。
//
// 设计取舍：
//   - 抗锯齿圆角不是逐帧现算的，而是按 (宽,高,圆角,描边) 生成一张纯 alpha 遮罩
//     纹理缓存起来，颜色通过 SetTextureColorMod 调制。这样同一元素换 hover 色
//     不会产生新缓存，颜色与几何彻底解耦。
//   - 文字纹理按 (字号, 颜色, 内容) 缓存。UI 文本量小且高度重复，命中率很高。
//   - 缓存不设 LRU，只在超过上限时整体清空 —— UI 场景下这比维护 LRU 更简单也够用。
#pragma once

#include <SDL.h>
#include <SDL_ttf.h>

#include <string>
#include <unordered_map>

#include "core/types.h"

namespace fc {
namespace ui {

// ---------------------------------------------------------------- 颜色
struct RGBA {
    u8 r = 0, g = 0, b = 0, a = 255;
};

constexpr RGBA rgba(u8 r, u8 g, u8 b, u8 a = 255) { return RGBA{r, g, b, a}; }

// 线性插值（t=0 取 a，t=1 取 b）
RGBA mix(const RGBA& a, const RGBA& b, float t);
// 整体乘一个透明度系数
RGBA fade(const RGBA& c, float k);

// ---------------------------------------------------------------- 主题
// 走深色：与游戏画面、以及用户的深色编辑器环境一致。
// 配色参考现代桌面应用（低饱和背景 + 单强调色 + 三级文字灰阶）。
namespace theme {

constexpr RGBA bg        = rgba(8, 11, 16);        // 窗口最底层
constexpr RGBA bgGlow    = rgba(22, 34, 52);       // 底部渐变辉光
constexpr RGBA surface   = rgba(17, 22, 30);       // 一级面板
constexpr RGBA surfaceHi = rgba(25, 32, 43);       // 卡片 / 二级面板
constexpr RGBA hover     = rgba(35, 45, 60);       // 悬停态
constexpr RGBA pressed   = rgba(44, 57, 76);       // 按下态
constexpr RGBA line      = rgba(35, 44, 58);       // 分隔细线
constexpr RGBA border    = rgba(43, 54, 70);       // 常规描边
constexpr RGBA borderHi  = rgba(74, 93, 119);      // 焦点 / 悬停描边

constexpr RGBA text      = rgba(232, 239, 247);    // 主文字
constexpr RGBA textDim   = rgba(142, 155, 173);    // 次要文字
constexpr RGBA textFaint = rgba(95, 107, 123);     // 极弱文字 / 占位

constexpr RGBA accent    = rgba(88, 166, 255);     // 强调色（蓝）
constexpr RGBA accentInk = rgba(10, 20, 32);       // 压在强调色上的文字
constexpr RGBA ok        = rgba(74, 209, 130);
constexpr RGBA warn      = rgba(240, 178, 74);
constexpr RGBA err       = rgba(245, 101, 101);

} // namespace theme

// 圆角档位（像素）。Pill 会在绘制时自动收敛为 高/2。
namespace radius {
constexpr int Card   = 14;
constexpr int Panel  = 12;
constexpr int Button = 9;
constexpr int Input  = 8;
constexpr int Chip   = 7;
constexpr int Pill   = 1000;
} // namespace radius

// ---------------------------------------------------------------- 字体
// 按用途分组。同一个角色在整套界面里只用一个字号，避免视觉噪音。
enum class Font {
    Display,   // 超大标题 / 关键数字
    Title,     // 区块标题
    Body,      // 正文、按钮
    Small,     // 辅助说明、标签
    Mono,      // 等宽：IP、端口、哈希、房间号
    COUNT
};

// ---------------------------------------------------------------- 输入快照
// 一帧的输入状态。各界面判定「这个控件被点了吗」都靠它，共享同一份结构，
// 免得每个界面各自造一个长得一样的东西。
struct Input {
    float mx = 0, my = 0;
    bool  down    = false;   // 左键当前是否按住
    bool  pressed = false;   // 本帧内是否发生按下
    float wheel   = 0;

    bool inside(const SDL_Rect& r) const {
        return mx >= float(r.x) && mx < float(r.x + r.w)
            && my >= float(r.y) && my < float(r.y + r.h);
    }
};

// ---------------------------------------------------------------- 控件动画
// 把「目标状态（悬停 / 选中）」平滑成 0..1 的插值，控件按它混色，避免鼠标一动
// 颜色就跳变。按 id 索引并跨帧保持。
//   id 建议带上坐标，否则不同位置但同名的控件会互相干扰动画值。
float animStep(const std::string& id, bool active, double dt, float speed = 14.0f);
void  resetAnimSteps();

// ---------------------------------------------------------------- 绘制器
class Ui {
public:
    Ui() = default;
    ~Ui();

    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    // 绑定 renderer 并加载系统字体。返回 false 时 diag() 给出原因。
    // 注意：字体加载失败视为致命错误 —— 整套界面都依赖它，没有降级路径。
    bool init(SDL_Renderer* ren);
    void shutdown();

    SDL_Renderer* ren() const { return ren_; }

    // ---- 视口（窗口真实像素尺寸，由调用方在 resize 时同步）----
    void setViewport(int w, int h) { vw_ = w; vh_ = h; }
    int  width()  const { return vw_; }
    int  height() const { return vh_; }

    // ---- 帧节拍：动效统一读这个时间轴 ----
    void   tick(double dt) { t_ += dt; }
    double time() const { return t_; }

    // ---- 字体信息 ----
    bool fontsOk() const { return fontsOk_; }
    const std::string& fontPath() const { return fontPath_; }
    const std::string& diag() const { return diag_; }
    int  lineHeight(Font role) const;
    int  ascent(Font role) const;

    // ---- 基础图形 ----
    void clear(const RGBA& c);
    void fill(const SDL_Rect& r, const RGBA& c);
    void fill(int x, int y, int w, int h, const RGBA& c);

    // 抗锯齿实心圆角矩形
    void round(const SDL_Rect& r, int rad, const RGBA& c);
    // 抗锯齿圆角描边（居中描边：一半在内一半在外）
    void roundOutline(const SDL_Rect& r, int rad, int thickness, const RGBA& c);
    // 软阴影：由多层递减透明度的圆角矩形近似
    void shadow(const SDL_Rect& r, int rad, int depth, int alpha = 110);

    void gradientV(const SDL_Rect& r, const RGBA& top, const RGBA& bottom);
    void gradientH(const SDL_Rect& r, const RGBA& left, const RGBA& right);

    void hline(int x, int y, int w, const RGBA& c);
    void vline(int x, int y, int h, const RGBA& c);
    void rectOutline(const SDL_Rect& r, const RGBA& c);

    // ---- 几何（图标用，不依赖字体里有没有对应字形）----
    void tri(int x1, int y1, int x2, int y2, int x3, int y3, const RGBA& c);
    void thickLine(int x1, int y1, int x2, int y2, int w, const RGBA& c);
    void ring(int cx, int cy, float rad, int thickness, const RGBA& c, float fromDeg = 0.f, float toDeg = 360.f);

    // ---- 文字 ----
    int  measure(Font role, const std::string& utf8) const;
    int  text(Font role, const std::string& utf8, int x, int y, const RGBA& c);
    // 超宽时自动加省略号，返回实际绘制宽度
    int  textClipped(Font role, const std::string& utf8, int x, int y, int maxW, const RGBA& c);
    std::string clip(Font role, const std::string& utf8, int maxW) const;

    void textVCenter(Font role, const std::string& utf8, int x, const SDL_Rect& box, const RGBA& c);
    void textCenter(Font role, const std::string& utf8, const SDL_Rect& box, const RGBA& c);
    void textCenterClipped(Font role, const std::string& utf8, const SDL_Rect& box, const RGBA& c);
    void textRight(Font role, const std::string& utf8, int rightX, int y, const RGBA& c);

    // ---- 缓动工具 ----
    static float easeOutCubic(float t);
    static float easeInOutCubic(float t);
    // 帧率无关的指数趋近：cur 以 speed 的速率追 target（dt 为秒）
    static float approach(float cur, float target, float speed, double dt);

private:
    SDL_Texture* maskTex(int w, int h, int rad, int thickness);
    SDL_Texture* textTex(Font role, const std::string& utf8, const RGBA& c, int* w, int* h);
    void purgeIfNeeded();

    SDL_Renderer* ren_ = nullptr;
    TTF_Font*     fonts_[int(Font::COUNT)]{};
    std::string   fontPath_, diag_;
    bool          fontsOk_ = false;

    int    vw_ = 1280, vh_ = 720;
    double t_  = 0.0;

    std::unordered_map<u64, SDL_Texture*> masks_;
    std::unordered_map<std::string, SDL_Texture*> texts_;
};

} // namespace ui
} // namespace fc
