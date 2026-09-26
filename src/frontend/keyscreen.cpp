// keyscreen.cpp — 键位设置界面实现
//
// 不用 SDL 的绘制指令逐像素画字，而是先在 256x240 的 ARGB 缓冲里合成整幅画面、
// 再一次性上传纹理。理由：点阵字体是靠一堆 1x1 小方块拼出来的，
// 逐方块调用 SDL_RenderFillRect 会有上千次绘制调用，直接软合成便宜得多。
#include "keyscreen.h"

#include "font5x7.h"

#include <cstdio>
#include <vector>

namespace fc {

namespace {

// 配色：整体走深色，与游戏画面/编辑器深色主题一致
constexpr u32 kBg        = 0x0F1620;
constexpr u32 kPanel     = 0x18242F;
constexpr u32 kPanelSel  = 0x1E4C8A;   // 当前选中的行
constexpr u32 kPanelBind = 0x7A4E12;   // 正在等待按键的行
constexpr u32 kBorder    = 0x33506E;
constexpr u32 kText      = 0xE6EDF3;
constexpr u32 kTextDim   = 0x7C8B99;
constexpr u32 kAccent    = 0x4FA3FF;
constexpr u32 kWarn      = 0xFFB020;
constexpr u32 kOk        = 0x5CE430;

constexpr int kRowY0   = 36;   // 第一行动作的 y
constexpr int kRowStep = 13;   // 行距
constexpr int kColX0   = 10;   // P1 列起点
constexpr int kColStep = 124;  // 两列间距
constexpr int kColW    = 116;  // 每列高亮条宽度
constexpr int kKeyOffX = 42;   // 键名相对列起点的偏移

std::string shortName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

} // namespace

void drawKeyConfigScreen(u32* p, const KeyMap& km, const KeyScreenView& v) {
    std::fill(p, p + kScreenPixels, kBg);

    // 标题 + 分隔线
    drawTextCenter5x7(p, kScreenWidth, kScreenHeight, kScreenWidth / 2, 6, "KEY CONFIG", kAccent);
    fillRectBuf(p, kScreenWidth, kScreenHeight, 4, 17, kScreenWidth - 8, 1, kBorder);

    // 两块面板
    fillRectBuf(p, kScreenWidth, kScreenHeight, 4, 22, kScreenWidth - 8, 120, kPanel);
    fillRectBuf(p, kScreenWidth, kScreenHeight, 4, 146, kScreenWidth - 8, 88, kPanel);
    fillRectBuf(p, kScreenWidth, kScreenHeight, 4, 22, kScreenWidth - 8, 1, kBorder);
    fillRectBuf(p, kScreenWidth, kScreenHeight, 4, 141, kScreenWidth - 8, 1, kBorder);
    fillRectBuf(p, kScreenWidth, kScreenHeight, 4, 146, kScreenWidth - 8, 1, kBorder);
    fillRectBuf(p, kScreenWidth, kScreenHeight, 4, 233, kScreenWidth - 8, 1, kBorder);

    // 列标题
    drawText5x7(p, kScreenWidth, kScreenHeight, 10, 26, "P1", kAccent);
    drawText5x7(p, kScreenWidth, kScreenHeight, 10 + kColStep, 26, "P2", kAccent);

    // 8 个动作
    for (int pi = 0; pi < kPlayerCount; ++pi) {
        const int xc = kColX0 + pi * kColStep;
        for (int a = 0; a < ACT_COUNT; ++a) {
            const int ry = kRowY0 + a * kRowStep;
            const bool sel = (pi == v.col && a == v.row);
            const bool bin = sel && v.binding;

            if (sel)
                fillRectBuf(p, kScreenWidth, kScreenHeight, xc - 4, ry - 3,
                            kColW, 12, bin ? kPanelBind : kPanelSel);

            drawText5x7(p, kScreenWidth, kScreenHeight, xc, ry,
                        actionLabel(a), sel ? 0xFFFFFFu : kText);

            if (bin) {
                if (v.blink)
                    drawText5x7(p, kScreenWidth, kScreenHeight, xc + kKeyOffX, ry,
                                "PRESS...", kWarn);
            } else {
                drawText5x7(p, kScreenWidth, kScreenHeight, xc + kKeyOffX, ry,
                            keyName(km.get(pi, a)).c_str(), sel ? 0xFFFFFFu : kTextDim);
            }
        }
    }

    // 底部操作说明
    drawText5x7(p, kScreenWidth, kScreenHeight, 10, 152, "MOVE    ARROWS / WASD", kText);
    drawText5x7(p, kScreenWidth, kScreenHeight, 10, 164, "REBIND  ENTER", kText);
    drawText5x7(p, kScreenWidth, kScreenHeight, 10, 176, "DEFAULT R        BACK  ESC", kText);
    drawText5x7(p, kScreenWidth, kScreenHeight, 10, 188,
                "ONE KEY CAN DRIVE ONLY ONE ACTION", kTextDim);

    // 状态行（限时显示，过期就回到默认提示）
    if (!v.msg.empty())
        drawText5x7(p, kScreenWidth, kScreenHeight, 10, 200, v.msg.c_str(),
                    v.msgWarn ? kWarn : kOk);
    else
        drawText5x7(p, kScreenWidth, kScreenHeight, 10, 200,
                    "DEFAULT  P1 = WASD + JK", kTextDim);

    drawText5x7(p, kScreenWidth, kScreenHeight, 10, 212,
                ("FILE  " + (v.fileTag.empty() ? std::string("fc-keys.cfg") : v.fileTag)).c_str(),
                kTextDim);
    drawText5x7(p, kScreenWidth, kScreenHeight, 10, 224,
                "F2 / ESC  BACK TO GAME", kTextDim);
}

bool runKeyConfigScreen(SDL_Window* win, SDL_Renderer* ren,
                        const std::string& configPath, KeyMap& km, bool* quit) {
    if (quit) *quit = false;

    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         kScreenWidth, kScreenHeight);
    if (!tex) return false;
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);

    std::vector<u32> buf(kScreenPixels, kBg);

    KeyScreenView v;
    v.fileTag = shortName(configPath);

    bool changed = false;
    u32  msgUntil = 0;          // 0 表示状态行一直有效（绑定提示用）

    bool wantQuit = false;
    bool done = false;
    while (!done) {
        // ---------------------------------------------------------- 事件
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { wantQuit = true; done = true; break; }
            if (e.type != SDL_KEYDOWN) continue;

            const SDL_Scancode sc = e.key.keysym.scancode;

            if (v.binding) {
                // 绑定状态下 Esc 一律当「取消」，否则一旦把 Esc 绑进去就出不来了
                if (sc == SDL_SCANCODE_ESCAPE) {
                    v.binding = false;
                    v.msg = "CANCELLED";
                    v.msgWarn = false;
                    msgUntil = SDL_GetTicks() + 1200;
                    continue;
                }
                const int other = km.conflictIn(v.col, sc, v.row);
                if (other >= 0) {
                    // 同一玩家内不允许一个键管两个动作，否则游戏里必然串键
                    v.msg = std::string("ALREADY USED BY ") + actionLabel(other);
                    v.msgWarn = true;
                    msgUntil = SDL_GetTicks() + 2500;
                    continue;
                }
                km.set(v.col, v.row, sc);
                changed = true;
                v.binding = false;
                v.msg = std::string("P") + char('1' + v.col) + " " + actionLabel(v.row) +
                        " = " + keyName(sc);
                v.msgWarn = km.overlapsOtherPlayer(v.col, sc);
                if (v.msgWarn)
                    v.msg += "  (ALSO USED BY " + std::string(1, char('1' + (1 - v.col))) + "P)";
                msgUntil = SDL_GetTicks() + 2500;
                continue;
            }

            switch (sc) {
                case SDL_SCANCODE_UP:    case SDL_SCANCODE_W: v.row = (v.row + ACT_COUNT - 1) % ACT_COUNT; break;
                case SDL_SCANCODE_DOWN:  case SDL_SCANCODE_S: v.row = (v.row + 1) % ACT_COUNT; break;
                case SDL_SCANCODE_LEFT:  case SDL_SCANCODE_A: v.col ^= 1; break;
                case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: v.col ^= 1; break;
                case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER:
                    v.binding = true;
                    v.msg = "PRESS ANY KEY  (ESC = CANCEL)";
                    v.msgWarn = false;
                    msgUntil = 0;
                    break;
                case SDL_SCANCODE_R:
                    km.setDefaults();
                    changed = true;
                    v.msg = "RESTORED DEFAULT KEYS";
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
        const u32 now = SDL_GetTicks();
        v.blink = ((now / 350) % 2) == 0;
        if (msgUntil != 0 && now >= msgUntil) { v.msg.clear(); msgUntil = 0; }

        drawKeyConfigScreen(buf.data(), km, v);

        SDL_UpdateTexture(tex, nullptr, buf.data(), kScreenWidth * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);
        if (win) SDL_SetWindowTitle(win, "fc-emulator — 键位设置（F2/Esc 返回游戏）");

        SDL_Delay(16);
    }

    SDL_DestroyTexture(tex);

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
