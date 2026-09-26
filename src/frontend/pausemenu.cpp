// pausemenu.cpp — 游戏内暂停 / 设置覆盖层的绘制与交互
//
// 这一层不做任何「真正的动作」，只把用户意图翻译成 PauseAction 交回主循环。
// 好处是联机时主循环可以选择「菜单开着但游戏继续跑」，而不会卡住对端。
#include "pausemenu.h"

#include <string>
#include <vector>

using namespace fc::ui;

namespace fc {
namespace {

constexpr int kItemH   = 48;
constexpr int kPanelW  = 448;
constexpr int kHeaderH = 62;
constexpr int kFooterH = 88;

// 菜单图标全部用几何画出来，不依赖字体里有没有对应字形
void menuIcon(Ui& u, int kind, int cx, int cy, const RGBA& c) {
    switch (kind) {
        case 0:   // 继续：播放三角
            u.tri(cx - 5, cy - 8, cx - 5, cy + 8, cx + 8, cy, c);
            break;
        case 1:   // 存档：向下箭头 + 底座
            u.thickLine(cx, cy - 9, cx, cy + 3, 2, c);
            u.thickLine(cx - 5, cy - 2, cx, cy + 3, 2, c);
            u.thickLine(cx + 5, cy - 2, cx, cy + 3, 2, c);
            u.thickLine(cx - 8, cy + 9, cx + 8, cy + 9, 2, c);
            break;
        case 2:   // 读档：向上箭头 + 底座
            u.thickLine(cx, cy + 3, cx, cy - 9, 2, c);
            u.thickLine(cx - 5, cy - 3, cx, cy - 9, 2, c);
            u.thickLine(cx + 5, cy - 3, cx, cy - 9, 2, c);
            u.thickLine(cx - 8, cy + 9, cx + 8, cy + 9, 2, c);
            break;
        case 3:   // 键位：键盘
            u.roundOutline(SDL_Rect{ cx - 10, cy - 7, 20, 15 }, 3, 2, c);
            u.fill(cx - 6, cy - 3, 3, 2, c);
            u.fill(cx - 1, cy - 3, 3, 2, c);
            u.fill(cx + 4, cy - 3, 3, 2, c);
            u.fill(cx - 5, cy + 2, 10, 2, c);
            break;
        case 4:   // 全屏：四个内折角
            for (int i = 0; i < 4; ++i) {
                const int sx = (i & 1) ? 1 : -1;
                const int sy = (i & 2) ? 1 : -1;
                const int x = cx + sx * 9, y = cy + sy * 7;
                u.thickLine(x, y, x - sx * 5, y, 2, c);
                u.thickLine(x, y, x, y - sy * 5, 2, c);
            }
            break;
        case 5:   // 缩放：大小两个框
            u.roundOutline(SDL_Rect{ cx - 10, cy - 8, 20, 16 }, 2, 2, c);
            u.roundOutline(SDL_Rect{ cx - 5, cy - 4, 10, 8 }, 2, 2, c);
            break;
        default:  // 退出：电源符号
            u.ring(cx, cy + 1, 8, 2, c, 300, 360);
            u.ring(cx, cy + 1, 8, 2, c, 0, 240);
            u.thickLine(cx, cy - 10, cx, cy - 1, 2, c);
            break;
    }
}

} // namespace

std::string scaleLabel(int scale) {
    return (scale <= 0) ? std::string("自动") : (std::to_string(scale) + "x");
}

int nextScale(int scale) {
    // 0(自动) → 1 → 2 → 3 → 4 → 回到自动
    return (scale >= 4) ? 0 : scale + 1;
}

PauseAction pauseMenuFrame(Ui& ui, const PauseInfo& info, const Input& in, double dt) {
    const int W = ui.width(), H = ui.height();
    PauseAction act = PauseAction::None;

    // ---------------- 遮罩
    // 联机时压得更轻：面板靠右放，玩家仍然看得到、也还得继续操作游戏
    ui.fill(SDL_Rect{ 0, 0, W, H }, rgba(4, 6, 10, info.netActive ? 118 : 170));

    // ---------------- 面板几何
    struct Item {
        const char* label;
        const char* hint;
        int         icon;
        PauseAction act;
        bool        enabled;
        const char* why;      // 禁用原因（显示在右侧）
    };

    const std::string scaleHint = scaleLabel(info.scale);
    const std::vector<Item> items = {
        { "继续游戏", "F1 / Esc",  0, PauseAction::Resume,            true,  nullptr },
        { "保存存档", "F5",        1, PauseAction::SaveState,         true,  nullptr },
        { "读取存档", "F8",        2, PauseAction::LoadState,         !info.netActive, "联机中不可用" },
        { "键位设置", "F2",        3, PauseAction::KeyConfig,         !info.netActive, "联机中不可用" },
        { "切换全屏", "F11",       4, PauseAction::ToggleFullscreen,  true,  nullptr },
        { "画面缩放", "",          5, PauseAction::CycleScale,        true,  nullptr },
        { "退出游戏", "Esc",       6, PauseAction::Quit,              true,  nullptr },
    };

    const int ph = kHeaderH + int(items.size()) * kItemH + kFooterH;
    // 联机时靠右站，留出左边的游戏画面；单机时居中
    const int px = info.netActive ? std::max(12, W - kPanelW - 28)
                                  : (W - kPanelW) / 2;
    const int py = std::max(12, (H - ph) / 2);
    const SDL_Rect panel{ px, py, kPanelW, ph };

    // ---------------- 面板
    ui.shadow(panel, radius::Card, 20, 200);
    ui.round(panel, radius::Card, rgba(20, 26, 35, 252));
    ui.roundOutline(panel, radius::Card, 1, theme::borderHi);
    ui.hline(panel.x + radius::Card, panel.y, panel.w - radius::Card * 2, rgba(255, 255, 255, 18));

    // 标题
    ui.text(Font::Title, "已暂停", panel.x + 24, panel.y + 18, theme::text);
    {
        const std::string sub = info.romName.empty() ? std::string("fc-emulator")
                                                     : info.romName;
        ui.text(Font::Small, ui.clip(Font::Small, sub, kPanelW - 48),
                panel.x + 24, panel.y + 44, theme::textFaint);
    }

    // 联机徽章
    if (info.netActive) {
        const std::string tag = info.netTag.empty() ? std::string("联机中") : info.netTag;
        const int tw = ui.measure(Font::Small, tag);
        const SDL_Rect badge{ panel.x + panel.w - 24 - (tw + 30), panel.y + 20, tw + 30, 26 };
        const RGBA c = info.desynced ? theme::err : theme::ok;
        ui.round(badge, radius::Chip, fade(c, 0.16f));
        ui.roundOutline(badge, radius::Chip, 1, fade(c, 0.5f));
        // 信号点
        ui.fill(badge.x + 11, badge.y + 11, 5, 5, c);
        ui.textVCenter(Font::Small, tag, badge.x + 21, badge, c);
    }

    // ---------------- 菜单项
    int y = panel.y + kHeaderH;
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[size_t(i)];
        const SDL_Rect row{ panel.x + 12, y, panel.w - 24, kItemH - 4 };
        const bool hot = it.enabled && in.inside(row);
        const float k  = animStep("pm:" + std::to_string(i), hot, dt);

        if (k > 0.01f) {
            ui.round(row, radius::Button, fade(theme::text, 0.06f * k));
            // 悬停时左侧点一条强调色，指示当前项
            ui.round(SDL_Rect{ row.x + 3, row.y + 12, 3, row.h - 24 }, 2,
                     fade(theme::accent, k));
        }

        const RGBA fg = it.enabled ? theme::text : theme::textFaint;
        menuIcon(ui, it.icon, row.x + 30, row.y + row.h / 2,
                 it.enabled ? mix(theme::textDim, theme::accent, k) : theme::border);

        ui.textVCenter(Font::Body, it.label, row.x + 54, row, fg);

        if (it.act == PauseAction::CycleScale) {
            // 缩放项右侧直接显示当前档位
            const int tw = ui.measure(Font::Small, scaleHint);
            const SDL_Rect chipRect{ row.x + row.w - tw - 34, row.y + (row.h - 24) / 2, tw + 22, 24 };
            ui.round(chipRect, radius::Chip, fade(theme::accent, 0.22f));
            ui.textCenter(Font::Small, scaleHint, chipRect, theme::accent);
        } else if (!it.enabled && it.why) {
            ui.textRight(Font::Small, it.why, row.x + row.w - 14,
                         row.y + (row.h - ui.lineHeight(Font::Small)) / 2, theme::warn);
        } else if (it.hint[0] != '\0') {
            ui.textRight(Font::Small, it.hint, row.x + row.w - 14,
                         row.y + (row.h - ui.lineHeight(Font::Small)) / 2, theme::textFaint);
        }

        if (hot && in.pressed) act = it.act;
        y += kItemH;
    }

    // ---------------- 底部状态
    ui.hline(panel.x + 20, panel.y + ph - kFooterH, panel.w - 40, theme::line);
    {
        const std::string row1 = info.netActive
            ? ("延迟 " + std::to_string(info.delay) + " 帧 · 网络抖动 " + std::to_string(info.lag) + " 帧")
            : std::string("单机模式");
        ui.text(Font::Small, row1, panel.x + 24, panel.y + ph - kFooterH + 14, theme::textDim);

        if (info.netActive && info.desynced)
            ui.text(Font::Small, "检测到状态不同步", panel.x + 24, panel.y + ph - kFooterH + 38, theme::err);
        else
            ui.text(Font::Small, "FPS " + std::to_string(info.fps),
                    panel.x + 24, panel.y + ph - kFooterH + 38, theme::textFaint);

        const std::string st = info.hasState ? "磁盘上有存档" : "尚无存档";
        ui.textRight(Font::Small, st, panel.x + panel.w - 24, panel.y + ph - kFooterH + 38,
                     info.hasState ? theme::textDim : theme::textFaint);
    }

    // ---------------- 一次性提示
    if (!info.toast.empty()) {
        const int tw = ui.measure(Font::Body, info.toast);
        const SDL_Rect box{ panel.x + (panel.w - tw) / 2 - 20, panel.y + ph + 14, tw + 40, 40 };
        ui.shadow(box, radius::Button, 12, 170);
        ui.round(box, radius::Button, theme::surfaceHi);
        ui.roundOutline(box, radius::Button, 1,
                        info.toastWarn ? fade(theme::warn, 0.6f) : theme::border);
        ui.textCenter(Font::Body, info.toast, box,
                      info.toastWarn ? theme::warn : theme::text);
    }

    return act;
}

} // namespace fc
