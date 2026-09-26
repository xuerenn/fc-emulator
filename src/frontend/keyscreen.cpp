// keyscreen.cpp — 键位设置界面实现（矢量字体 + 窗口真实分辨率）
//
// 布局：左右两张玩家卡片，每张 8 行「动作 —— 按键」。
// 交互保留了旧版的全部约定（冲突拦截、Esc 取消绑定、R 恢复默认、改动存盘），
// 只是从点阵键盘界面换成了现在的样子，并补上了鼠标操作。
#include "keyscreen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace fc::ui;

namespace fc {
namespace {

std::string shortName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

constexpr int kPad = 24;

// 单列卡片的行高：窗口矮的时候自动压缩，保证 8 行都塞得下
int rowHeight(int cardH) {
    const int usable = cardH - 58 - 16;
    return std::max(30, std::min(46, usable / ACT_COUNT));
}

} // namespace

void drawKeyScreen(Ui& u, const KeyMap& km, const KeyScreenView& v) {
    const int W = u.width(), H = u.height();
    const int TOP = 66, BOTTOM = 56;

    // ---------------- 背景
    u.clear(theme::bg);
    u.gradientV(SDL_Rect{ 0, H - 300, W, 300 }, rgba(8, 11, 16, 0), theme::bgGlow);

    // ---------------- 顶栏
    u.text(Font::Title, "键位设置", kPad, 20, theme::text);
    u.text(Font::Small, "改动会自动保存，游戏里约 1 秒内热加载生效",
           kPad + u.measure(Font::Title, "键位设置") + 16, 28, theme::textFaint);
    if (!v.fileTag.empty())
        u.textRight(Font::Mono, v.fileTag, W - kPad, 26, theme::textFaint);
    u.hline(kPad, TOP - 12, W - kPad * 2, theme::line);

    // ---------------- 两张玩家卡片
    const int gap  = 22;
    const int colW = std::min(430, (W - kPad * 2 - gap) / 2);
    const int totalW = colW * 2 + gap;
    const int x0 = (W - totalW) / 2;
    const int cardH = std::min(H - TOP - BOTTOM - 34, 470);

    for (int pi = 0; pi < kPlayerCount; ++pi) {
        const SDL_Rect card{ x0 + pi * (colW + gap), TOP, colW, cardH };
        u.shadow(card, radius::Card, 13, 115);
        u.round(card, radius::Card, theme::surfaceHi);
        u.roundOutline(card, radius::Card, 1, theme::border);
        u.hline(card.x + radius::Card, card.y, card.w - radius::Card * 2, rgba(255, 255, 255, 12));

        // 卡片头
        u.text(Font::Body, std::string("玩家 ") + char('1' + pi), card.x + 20, card.y + 14, theme::accent);
        const std::string hint = (pi == 0) ? "默认 WASD + JK" : "默认 方向键 + ZX";
        u.textRight(Font::Small, hint, card.x + card.w - 20, card.y + 18, theme::textFaint);
        u.hline(card.x + 20, card.y + 44, card.w - 40, theme::line);

        const int rowH = rowHeight(cardH);
        for (int a = 0; a < ACT_COUNT; ++a) {
            const int ry = card.y + 52 + a * rowH;
            const SDL_Rect row{ card.x + 12, ry, card.w - 24, rowH - 4 };
            const bool sel = (pi == v.col && a == v.row);
            const bool bin = sel && v.binding;

            if (sel) {
                u.round(row, radius::Button, bin ? fade(theme::warn, 0.20f) : fade(theme::accent, 0.18f));
                u.roundOutline(row, radius::Button, 1,
                               bin ? fade(theme::warn, 0.75f) : fade(theme::accent, 0.70f));
            }

            const RGBA labelColor = sel ? theme::text : theme::textDim;
            u.textVCenter(Font::Body, actionLabelCN(a), row.x + 14, row, labelColor);

            if (bin) {
                if (v.blink)
                    u.textRight(Font::Body, "按下任意键…", row.x + row.w - 14, row.y + (row.h - u.lineHeight(Font::Body)) / 2, theme::warn);
            } else {
                const SDL_Scancode sc = km.get(pi, a);
                const std::string kn = keyName(sc);
                // 键名做成小胶囊，和动作名形成区分
                const int kw = u.measure(Font::Body, kn);
                const SDL_Rect chipRect{ row.x + row.w - kw - 26, row.y + (row.h - 26) / 2, kw + 20, 26 };
                if (sel) {
                    u.round(chipRect, radius::Chip, fade(theme::accent, bin ? 0.25f : 0.30f));
                    u.textCenter(Font::Body, kn, chipRect, theme::text);
                } else {
                    u.round(chipRect, radius::Chip, theme::surface);
                    u.textCenter(Font::Body, kn, chipRect, theme::textDim);
                }
            }
        }
    }

    // ---------------- 状态行 / 操作说明
    const int msgY = TOP + cardH + 16;
    if (!v.msg.empty()) {
        const int mw = u.measure(Font::Body, v.msg);
        const SDL_Rect box{ (W - mw) / 2 - 18, msgY - 6, mw + 36, 36 };
        u.round(box, radius::Button, fade(v.msgWarn ? theme::warn : theme::ok, 0.14f));
        u.roundOutline(box, radius::Button, 1, fade(v.msgWarn ? theme::warn : theme::ok, 0.45f));
        u.textCenter(Font::Body, v.msg, box, v.msgWarn ? theme::warn : theme::ok);
    } else {
        u.textCenter(Font::Small, "同一玩家内一个键只能对应一个动作；两个玩家用同一个键会给出提示但不拦截",
                     SDL_Rect{ 0, msgY, W, 24 }, theme::textFaint);
    }

    // 底栏
    u.hline(0, H - BOTTOM, W, theme::line);
    u.fill(SDL_Rect{ 0, H - BOTTOM + 1, W, BOTTOM - 1 }, rgba(12, 16, 22));
    u.textVCenter(Font::Small, "方向键 / WASD  移动", kPad, SDL_Rect{ 0, H - BOTTOM + 14, 0, 18 }, theme::textFaint);
    u.textVCenter(Font::Small, "Enter / 点击  改键", kPad + 200, SDL_Rect{ 0, H - BOTTOM + 14, 0, 18 }, theme::textFaint);
    u.textVCenter(Font::Small, "R  恢复默认", kPad + 400, SDL_Rect{ 0, H - BOTTOM + 14, 0, 18 }, theme::textFaint);
    u.textRight(Font::Small, "Esc  返回", W - kPad, H - BOTTOM + 28, theme::textDim);
}

bool runKeyConfigScreen(SDL_Window* win, SDL_Renderer* ren, Ui& ui,
                        const std::string& configPath, KeyMap& km, bool* quit) {
    if (quit) *quit = false;

    KeyScreenView v;
    v.fileTag = shortName(configPath);

    bool changed  = false;
    u32  msgUntil = 0;          // 0 表示一直有效

    bool wantQuit = false;
    bool done     = false;

    u64 prev = SDL_GetPerformanceCounter();
    const u64 freq = SDL_GetPerformanceFrequency();

    // 事件循环里要知道鼠标落在哪一行，先备好布局参数
    const int W = ui.width(), H = ui.height();
    const int TOP = 66, BOTTOM = 56;
    const int gap = 22;
    const int colW = std::min(430, (W - kPad * 2 - gap) / 2);
    const int totalW = colW * 2 + gap;
    const int x0 = (W - totalW) / 2;
    const int cardH = std::min(H - TOP - BOTTOM - 34, 470);
    const int rowH = rowHeight(cardH);

    while (!done) {
        const u64 now = SDL_GetPerformanceCounter();
        double dt = double(now - prev) / double(freq);
        prev = now;
        if (dt > 0.1) dt = 0.1;
        ui.tick(dt);

        // ---------------------------------------------------------- 事件
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { wantQuit = true; done = true; break; }

            if (e.type == SDL_WINDOWEVENT &&
                (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                 e.window.event == SDL_WINDOWEVENT_RESIZED)) {
                int nw = 0, nh = 0;
                SDL_GetRendererOutputSize(ren, &nw, &nh);
                ui.setViewport(nw, nh);
                continue;
            }

            // 鼠标：点行选中，点已选中的行进入改键
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int ow = 0, ww = 0;
                SDL_GetRendererOutputSize(ren, &ow, nullptr);
                SDL_GetWindowSize(win, &ww, nullptr);
                const float k = (ww > 0) ? float(ow) / float(ww) : 1.0f;
                const float mx = float(e.button.x) * k;
                const float my = float(e.button.y) * k;

                for (int pi = 0; pi < kPlayerCount; ++pi) {
                    for (int a = 0; a < ACT_COUNT; ++a) {
                        const SDL_Rect row{ x0 + pi * (colW + gap) + 12,
                                            TOP + 52 + a * rowH, colW - 24, rowH - 4 };
                        if (mx >= float(row.x) && mx < float(row.x + row.w) &&
                            my >= float(row.y) && my < float(row.y + row.h)) {
                            if (!v.binding) {
                                if (v.col == pi && v.row == a) {
                                    v.binding = true;
                                    v.msg = "按下想绑定的按键（Esc 取消）";
                                    v.msgWarn = false;
                                    msgUntil = 0;
                                } else {
                                    v.col = pi;
                                    v.row = a;
                                }
                            }
                        }
                    }
                }
                continue;
            }

            if (e.type != SDL_KEYDOWN) continue;

            const SDL_Scancode sc = e.key.keysym.scancode;

            if (v.binding) {
                // 绑定状态下 Esc 一律当「取消」，否则一旦把 Esc 绑进去就出不来了
                if (sc == SDL_SCANCODE_ESCAPE) {
                    v.binding = false;
                    v.msg = "已取消";
                    v.msgWarn = false;
                    msgUntil = SDL_GetTicks() + 1200;
                    continue;
                }
                const int other = km.conflictIn(v.col, sc, v.row);
                if (other >= 0) {
                    // 同一玩家内不允许一个键管两个动作，否则游戏里必然串键
                    v.msg = std::string("已被「") + actionLabelCN(other) + "」占用";
                    v.msgWarn = true;
                    msgUntil = SDL_GetTicks() + 2500;
                    continue;
                }
                km.set(v.col, v.row, sc);
                changed = true;
                v.binding = false;
                v.msg = std::string("玩家 ") + char('1' + v.col) + " · " + actionLabelCN(v.row) +
                        " = " + keyName(sc);
                v.msgWarn = km.overlapsOtherPlayer(v.col, sc);
                if (v.msgWarn) v.msg += "（该键同时被另一个玩家使用）";
                msgUntil = SDL_GetTicks() + 2500;
                continue;
            }

            switch (sc) {
                case SDL_SCANCODE_UP:    case SDL_SCANCODE_W:
                    v.row = (v.row + ACT_COUNT - 1) % ACT_COUNT; break;
                case SDL_SCANCODE_DOWN:  case SDL_SCANCODE_S:
                    v.row = (v.row + 1) % ACT_COUNT; break;
                case SDL_SCANCODE_LEFT:  case SDL_SCANCODE_A:
                    v.col ^= 1; break;
                case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D:
                    v.col ^= 1; break;
                case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER:
                    v.binding = true;
                    v.msg = "按下想绑定的按键（Esc 取消）";
                    v.msgWarn = false;
                    msgUntil = 0;
                    break;
                case SDL_SCANCODE_R:
                    km.setDefaults();
                    changed = true;
                    v.msg = "已恢复默认键位";
                    v.msgWarn = false;
                    msgUntil = SDL_GetTicks() + 2500;
                    break;
                case SDL_SCANCODE_ESCAPE:
                    done = true;
                    break;
                default:
                    break;
            }
            if (done) break;
        }
        if (wantQuit) break;

        // ---------------------------------------------------------- 画面
        const u32 t = SDL_GetTicks();
        v.blink = ((t / 380) % 2) == 0;
        if (msgUntil != 0 && t >= msgUntil) { v.msg.clear(); msgUntil = 0; }

        drawKeyScreen(ui, km, v);
        SDL_RenderPresent(ren);
        SDL_Delay(1);
    }

    if (changed) {
        if (km.save(configPath))
            std::printf("[键位] 已保存到 %s\n", configPath.c_str());
        else
            std::fprintf(stderr, "[键位] 保存失败: %s（本次改动仅在本次运行有效）\n", configPath.c_str());
    }
    for (int pi = 0; pi < kPlayerCount; ++pi) {
        std::printf("[键位] P%d: 上=%s 下=%s 左=%s 右=%s A=%s B=%s 选择=%s 开始=%s\n", pi + 1,
                    keyName(km.get(pi, ACT_UP)).c_str(), keyName(km.get(pi, ACT_DOWN)).c_str(),
                    keyName(km.get(pi, ACT_LEFT)).c_str(), keyName(km.get(pi, ACT_RIGHT)).c_str(),
                    keyName(km.get(pi, ACT_A)).c_str(), keyName(km.get(pi, ACT_B)).c_str(),
                    keyName(km.get(pi, ACT_SELECT)).c_str(), keyName(km.get(pi, ACT_START)).c_str());
    }

    if (quit) *quit = wantQuit;
    return changed;
}

} // namespace fc
