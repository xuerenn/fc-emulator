// main.cpp — SDL2 前端：启动器 / 窗口 / 渲染 / 音频 / 输入 / 主循环 / 联机接入
//
// 界面架构：
//   没有给 ROM 参数 → 先进图形启动器，选好 ROM 与联机设置再开始；
//   给了 ROM 参数   → 直接进游戏（脚本、批处理、老用法不受影响）。
//   游戏里 Esc / F1 唤出暂停菜单；联机时菜单只是叠加层，模拟不会停 ——
//   锁步一旦停住，对端会一直等这一帧直到超时。
//
// 渲染坐标：不再用 SDL_RenderSetLogicalSize 把一切缩放到 256x240，而是
//   用窗口真实像素绘制界面，游戏画面按整数倍缩放居中。这样界面能用矢量字体
//   画得清楚，像素画面也不会因为非整数缩放而糊掉。
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/emulator.h"
#include "net/net.h"

#include "keycfg.h"
#include "keyscreen.h"
#include "launcher.h"
#include "pausemenu.h"
#include "ui.h"

#ifdef _WIN32
#  include <windows.h>      // SetConsoleOutputCP：修 Windows 控制台中文乱码
#endif

using namespace fc;

namespace {

constexpr double kFrameMs = 1000.0 / 60.0988;   // NTSC 帧周期

// 启动器窗口的初始尺寸。够大才放得下三栏布局，用户也可以随意拉。
constexpr int kInitWindowW = 1280;
constexpr int kInitWindowH = 720;

struct Options {
    std::string rom;
    bool host = false;
    bool connect = false;
    std::string hostIp = "127.0.0.1";
    u16 hostPort = 7777;
    u16 connectPort = 7777;
    int  delay = 3;
    bool delaySet = false;
    int  scale = 3;
    bool scaleSet = false;
    bool fullscreen = false;
    bool noAudio = false;
    std::string stateFile;
    std::string loadState;    // 启动时载入的存档（为空则不载入）
    // 中继
    bool relay = false;
    std::string relayIp = "127.0.0.1";
    u16 relayPort = 7777;
    std::string room = "7777";
    // 键位
    std::string keyFile;      // 为空则用默认位置
    // 开发者自测
    std::string shot;         // --shot <界面名>：把某个界面离屏渲染成图片后退出
    std::string shotFile;     // 输出文件名（.ppm）
    int fpsLog = 0;           // 跑满 N 帧后打印实测帧率并退出（0=关闭）
};

void usage() {
    std::printf(
        "fc-emulator — FC/NES 模拟器（SDL2 + 锁步联机 + 图形启动器）\n"
        "\n"
        "用法:\n"
        "  fc-emulator                       打开图形启动器（选 ROM、配联机、调设置）\n"
        "  fc-emulator <rom.nes> [选项]       跳过启动器直接进游戏\n"
        "\n"
        "游戏内操作:\n"
        "  Esc / F1   暂停菜单（继续 / 存档 / 读档 / 键位 / 全屏 / 缩放 / 退出）\n"
        "  F5=存档  F8=读档  F2=键位设置  F11=全屏  Tab=加速(按住，仅单机)\n"
        "  默认 P1 方向=WASD  A=J  B=K  Select=右Shift  Start=回车\n"
        "  默认 P2 方向=方向键 A=Z  B=X  Select=右Ctrl   Start=右Alt\n"
        "  手柄自动识别（A/B/Start/Back/十字键/左摇杆）\n"
        "\n"
        "联机（局域网 / 主机有公网 IP）:\n"
        "  --host [端口]              作为主机开房（默认端口 7777）\n"
        "  --connect <IP[:端口]>      连接主机（默认端口 7777）\n"
        "  --delay <帧数>             输入延迟帧数，默认 3（越大越抗抖动、越迟钝）\n"
        "\n"
        "联机（走公网中继，双方都不需要公网 IP）:\n"
        "  --relay <IP[:端口]>        使用中继服务器（默认端口 7777）\n"
        "  --room <房间号>            房间号，两端必须一致（默认 7777）\n"
        "  例: fc-emulator game.nes --relay 1.2.3.4:7777 --room 1234 --host\n"
        "      fc-emulator game.nes --relay 1.2.3.4:7777 --room 1234 --connect\n"
        "\n"
        "显示/音频:\n"
        "  --scale <n>                游戏画面放大倍数，默认按窗口自动取整数倍\n"
        "  --fullscreen               全屏启动\n"
        "  --no-audio                 关闭音频\n"
        "\n"
        "按键:\n"
        "  --keys <文件>              指定键位配置文件（默认 <exe目录>/fc-keys.cfg）\n"
        "  配置文件支持热加载：在外部编辑器改完保存，约 1 秒内自动生效（联机中也生效）\n"
        "\n"
        "存档:\n"
        "  存档写到 <rom>.fcstate。\n"
        "  单机：F5 / F8 都可用。\n"
        "  联机：F5 仍可存档（纯本地读取、不影响同步），但 F8 读档被禁用。\n"
        "  --load-state <文件>        启动时先载入存档，再进入游戏/联机\n"
        "  想「从某个进度接着联机」就用它：双方各自拿到同一个档，然后\n"
        "      fc-emulator game.nes --load-state resume.fcstate --host 7777\n"
        "      fc-emulator game.nes --load-state resume.fcstate --connect <IP>\n"
        "  两端 ROM 与存档必须完全一致，否则一连上就会报不同步（这是故意的）。\n"
        "\n"
        "开发者自测:\n"
        "  --shot <界面> <文件.ppm>   离屏渲染界面并存图后退出\n"
        "                             界面: launcher | netplay | settings | keys | pause\n"
        "  --fps-log <帧数>           跑满 N 帧后打印实测帧率并退出\n");
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* def) -> std::string {
            if (i + 1 < argc && argv[i + 1][0] != '-') return argv[++i];
            return def;
        };
        if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        else if (a == "--host") {
            o.host = true;
            const std::string p = next("7777");
            o.hostPort = u16(std::atoi(p.c_str()));
        } else if (a == "--connect") {
            o.connect = true;
            const std::string v = next("");
            const size_t colon = v.find(':');
            if (colon != std::string::npos) {
                o.hostIp = v.substr(0, colon);
                o.connectPort = u16(std::atoi(v.substr(colon + 1).c_str()));
            } else {
                o.hostIp = v.empty() ? "127.0.0.1" : v;
            }
        } else if (a == "--delay") {
            o.delay = std::atoi(next("3").c_str());
            o.delaySet = true;
            if (o.delay < 0) o.delay = 0;
            if (o.delay > 30) o.delay = 30;
        } else if (a == "--relay") {
            o.relay = true;
            const std::string v = next("");
            const size_t colon = v.find(':');
            if (colon != std::string::npos) {
                o.relayIp = v.substr(0, colon);
                o.relayPort = u16(std::atoi(v.substr(colon + 1).c_str()));
            } else if (!v.empty()) {
                o.relayIp = v;
            }
            if (o.relayIp.empty()) o.relayIp = "127.0.0.1";
        } else if (a == "--room") {
            o.room = next("7777");
            if (o.room.empty()) o.room = "7777";
        } else if (a == "--scale") {
            o.scale = std::atoi(next("3").c_str());
            o.scaleSet = true;
            if (o.scale < 1) o.scale = 1;
            if (o.scale > 10) o.scale = 10;
        } else if (a == "--fullscreen") {
            o.fullscreen = true;
        } else if (a == "--load-state") {
            o.loadState = next("");
        } else if (a == "--no-audio") {
            o.noAudio = true;
        } else if (a == "--keys") {
            o.keyFile = next("");
        } else if (a == "--shot") {
            o.shot = next("");
            o.shotFile = next("");
            if (o.shotFile.empty()) o.shotFile = "shot.ppm";
        } else if (a == "--fps-log") {
            o.fpsLog = std::atoi(next("0").c_str());
        } else if (!a.empty() && a[0] != '-') {
            o.rom = a;
        }
    }
    if (o.host && o.connect) {
        std::fprintf(stderr, "错误: --host 与 --connect 不能同时使用\n");
        return false;
    }
    if (o.relay && !o.host && !o.connect) {
        std::fprintf(stderr, "错误: 使用 --relay 时必须指定 --host（坐 1P）或 --connect（坐 2P）\n");
        return false;
    }
    if (o.room.size() > 31) {
        std::fprintf(stderr, "警告: 房间号过长，已截断到 31 个字符\n");
        o.room.resize(31);
    }
    for (char c : o.room) {
        if (c == ' ' || c == '\t') {
            std::fprintf(stderr, "错误: 房间号不能包含空格\n");
            return false;
        }
    }
    return true;
}

std::string baseName(const std::string& path) {
    std::string s = path;
    const size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && dot > 0) s = s.substr(0, dot);
    return s;
}

std::string shortName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

u8 samplePad(SDL_GameController* gc) {
    if (!gc) return 0;
    u8 b = 0;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A)) b |= BTN_A;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B)) b |= BTN_B;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK)) b |= BTN_SELECT;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START)) b |= BTN_START;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP)) b |= BTN_UP;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) b |= BTN_DOWN;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) b |= BTN_LEFT;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= BTN_RIGHT;
    const Sint16 ax = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
    const Sint16 ay = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
    if (ay < -8000) b |= BTN_UP;
    if (ay > 8000)  b |= BTN_DOWN;
    if (ax < -8000) b |= BTN_LEFT;
    if (ax > 8000)  b |= BTN_RIGHT;
    return b;
}

bool writeFile(const std::string& path, const std::vector<u8>& data) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return true;
}

bool readFile(const std::string& path, std::vector<u8>& data) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    data.resize(size_t(n > 0 ? n : 0));
    if (n > 0) std::fread(data.data(), 1, size_t(n), f);
    std::fclose(f);
    return n > 0;
}

bool fileExists(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// 写 PPM（无依赖的图片格式，项目里的 tools/ppm2png.py 可转 PNG）
bool writePPM(const std::string& path, const u32* fb, int w, int h) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        const u32 c = fb[i];
        const unsigned char rgb[3] = {
            (unsigned char)((c >> 16) & 0xFF),
            (unsigned char)((c >> 8) & 0xFF),
            (unsigned char)(c & 0xFF),
        };
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    return true;
}

// 游戏画面的目标矩形：整数倍缩放居中，霓虹黑边（像素完美，不会糊）
SDL_Rect gameRectFor(int winW, int winH, int scaleSetting) {
    int s = scaleSetting;
    if (s <= 0) s = std::max(1, std::min(winW / kScreenWidth, winH / kScreenHeight));
    const int w = kScreenWidth * s;
    const int h = kScreenHeight * s;
    return SDL_Rect{ (winW - w) / 2, (winH - h) / 2, w, h };
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Windows 控制台默认代码页是 GBK(936)，而源文件里的中文按 UTF-8 存储，
    // 直接输出必然乱码。切到 UTF-8 即可（Win10 1903+ 的 conhost 与 Windows Terminal 均支持）。
    SetConsoleOutputCP(CP_UTF8);
#endif
    Options opt;
    if (!parseArgs(argc, argv, opt)) { usage(); return 1; }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL 初始化失败: %s\n", SDL_GetError());
        return 1;
    }

    // ------------------------------------------------------------ 窗口
    // 窗口统一按启动器尺寸来，游戏画面在里面整数缩放居中。
    // 好处是进游戏不用改窗口大小，进出暂停菜单也不会跳尺寸。
    SDL_Window* win = SDL_CreateWindow(
        "fc-emulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kInitWindowW, kInitWindowH,
        SDL_WINDOW_RESIZABLE | (opt.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) { std::fprintf(stderr, "创建窗口失败: %s\n", SDL_GetError()); SDL_Quit(); return 1; }

    // 截图模式用软件渲染，保证 target texture 一定可用
    SDL_Renderer* ren = nullptr;
    if (opt.shot.empty()) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        std::fprintf(stderr, "创建渲染器失败: %s\n", SDL_GetError());
        SDL_DestroyWindow(win); SDL_Quit(); return 1;
    }
    SDL_SetWindowMinimumSize(win, 960, 620);

    ui::Ui ui;
    if (!ui.init(ren)) {
        std::fprintf(stderr, "界面初始化失败：%s\n", ui.diag().c_str());
        SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
        return 1;
    }
    {
        int ow = 0, oh = 0;
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        ui.setViewport(ow, oh);
    }
    std::printf("字体    : %s\n", ui.fontPath().c_str());

    // ------------------------------------------------------------ 配置
    LaunchConfig cfg;
    cfg.load(LaunchConfig::defaultPath());
    const std::string keyFile = opt.keyFile.empty() ? defaultKeyConfigPath() : opt.keyFile;

    // 命令行参数优先于上次保存的启动器配置
    if (!opt.rom.empty()) cfg.rom = opt.rom;
    if (opt.host)    { cfg.net = LaunchConfig::Net::Host;   cfg.hostPort = opt.hostPort; }
    if (opt.connect) { cfg.net = LaunchConfig::Net::Client; cfg.hostIp = opt.hostIp;
                       cfg.connectPort = opt.connectPort; }
    if (opt.relay)   { cfg.relay = true; cfg.relayIp = opt.relayIp;
                       cfg.relayPort = opt.relayPort; cfg.room = opt.room; }
    if (opt.delaySet) cfg.delay = opt.delay;
    if (opt.scaleSet) cfg.scale = opt.scale;
    if (opt.fullscreen) cfg.fullscreen = true;
    if (opt.noAudio) cfg.audio = false;
    if (!opt.loadState.empty()) cfg.loadState = opt.loadState;

    // ------------------------------------------------------------ 离屏截图（开发自测）
    // 这套界面全是像素级细节（圆角、阴影、字体回退、对齐），靠读代码判断不了对错，
    // 必须能一键出图肉眼比对。--shot 就是干这个的。
    if (!opt.shot.empty()) {
        SDL_Texture* tgt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_TARGET, ui.width(), ui.height());
        if (!tgt) {
            std::fprintf(stderr, "创建离屏目标失败: %s\n", SDL_GetError());
        } else {
            SDL_SetRenderTarget(ren, tgt);

            if (opt.shot == "keys") {
                KeyMap km;
                km.setDefaults();
                km.load(keyFile);
                KeyScreenView kv;
                kv.fileTag = shortName(keyFile);
                kv.msg = "玩家 1 · A = J";
                drawKeyScreen(ui, km, kv);
            } else if (opt.shot == "pause") {
                // 先铺一层示意背景，方便看出遮罩的层次
                ui.clear(ui::rgba(12, 18, 28));
                ui.gradientV(SDL_Rect{ 0, 0, ui.width(), ui.height() },
                             ui::rgba(24, 40, 62), ui::rgba(8, 11, 16));
                PauseInfo pi;
                pi.romName = "68in1_HKX5268";
                pi.fps = 60;
                ui::Input in;
                in.mx = -100; in.my = -100;
                pauseMenuFrame(ui, pi, in, 0.0);
            } else {
                int tab = 0;
                if (opt.shot == "netplay")  tab = 1;
                if (opt.shot == "settings") tab = 2;
                drawLauncherPreview(ui, cfg, tab);
            }

            std::vector<u32> px(size_t(ui.width()) * size_t(ui.height()));
            if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                     px.data(), ui.width() * 4) != 0) {
                std::fprintf(stderr, "读像素失败: %s\n", SDL_GetError());
            } else if (writePPM(opt.shotFile, px.data(), ui.width(), ui.height())) {
                std::printf("已导出界面截图: %s (%dx%d)\n",
                            opt.shotFile.c_str(), ui.width(), ui.height());
            } else {
                std::fprintf(stderr, "写文件失败: %s\n", opt.shotFile.c_str());
            }
            SDL_DestroyTexture(tgt);
        }
        ui.shutdown();
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 0;
    }

    // ------------------------------------------------------------ 启动器
    if (opt.rom.empty()) {
        std::printf("=== fc-emulator 启动器 ===\n");
        if (!runLauncher(win, ren, ui, cfg, keyFile)) {
            ui.shutdown();
            SDL_DestroyRenderer(ren);
            SDL_DestroyWindow(win);
            SDL_Quit();
            std::printf("已退出。\n");
            return 0;
        }
        // 启动器可能改过窗口尺寸，重新同步一次视口
        int ow = 0, oh = 0;
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        ui.setViewport(ow, oh);
    }

    // ------------------------------------------------------------ 载入 ROM
    Emulator emu;
    if (!emu.loadROM(cfg.rom)) {
        std::fprintf(stderr, "ROM 加载失败（需要 iNES/NES2.0 格式的 .nes 文件）: %s\n", cfg.rom.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "fc-emulator",
                                 ("ROM 加载失败：\n" + cfg.rom).c_str(), win);
        ui.shutdown();
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    const std::string title = baseName(cfg.rom);
    std::printf("=== fc-emulator ===\n");
    std::printf("ROM     : %s\n", title.c_str());
    std::printf("Mapper  : %s\n", emu.cartridge().mapperName());
    std::printf("镜像    : %d\n", emu.cartridge().mirrorMode());

    // ------------------------------------------------------------ 启动即读档
    // 这是「从某个进度接着联机」的关键：联机只能由命令行/启动器开启，没有这个选项
    // 时根本没法带着内存里的档进入联机（一重启就丢）。
    // 加载失败必须硬报错退出 —— 若静默地从开机状态起跑，联机时只会看到莫名其妙的
    // 「不同步」，反而更难查。
    if (!cfg.loadState.empty()) {
        std::vector<u8> buf;
        std::string why;
        if (!readFile(cfg.loadState, buf)) {
            std::fprintf(stderr, "读档失败: 打不开 %s\n", cfg.loadState.c_str());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "fc-emulator",
                                     ("读档失败：打不开\n" + cfg.loadState).c_str(), win);
            ui.shutdown(); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
            return 1;
        }
        if (!emu.loadState(buf.data(), buf.size(), &why)) {
            std::fprintf(stderr, "读档失败: %s —— %s\n", cfg.loadState.c_str(), why.c_str());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "fc-emulator",
                                     ("读档失败：\n" + why).c_str(), win);
            ui.shutdown(); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
            return 1;
        }
        std::printf("读档    : %s（从第 %llu 帧继续）\n",
                    cfg.loadState.c_str(), (unsigned long long)emu.frameCount());
    }

    // ------------------------------------------------------------ 等待界面
    // 握手等待期间被反复调用：泵 SDL 事件（窗口因此不会「未响应」）+ 重绘等待动画，
    // 按 Esc 或关闭窗口即取消。
    u32 lastDraw = 0;
    auto waitScreen = [&](const std::string& what) -> bool {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) return false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) return false;
        }
        const u32 t = SDL_GetTicks();
        if (t - lastDraw < 16) return true;       // 重绘限到 ~60fps
        lastDraw = t;
        ui.tick(0.016);

        const int W = ui.width(), H = ui.height();
        ui.clear(ui::theme::bg);
        ui.gradientV(SDL_Rect{ 0, H - 320, W, 320 }, ui::rgba(8, 11, 16, 0), ui::theme::bgGlow);
        ui.textCenter(ui::Font::Title, what, SDL_Rect{ 0, H / 2 - 70, W, 30 }, ui::theme::text);
        ui.textCenter(ui::Font::Small, "按 Esc 取消，取消后将停留在单机模式",
                      SDL_Rect{ 0, H / 2 - 32, W, 20 }, ui::theme::textFaint);
        // 三个跳动的圆点表示「进行中」
        for (int i = 0; i < 3; ++i) {
            const float phase = float(t) / 380.0f - float(i) * 0.28f;
            const float k = std::max(0.0f, std::sin(phase * 3.14159f));
            const int r = 5 + int(6.0f * k);
            const ui::RGBA c = ui::mix(ui::theme::textFaint, ui::theme::accent, k);
            const int cx = W / 2 + (i - 1) * 34;
            const int cy = H / 2 + 26;
            ui.round(SDL_Rect{ cx - r, cy - r, r * 2, r * 2 }, r, c);
        }
        SDL_RenderPresent(ren);
        return true;
    };

    // ------------------------------------------------------------ 联机
    NetSession net;
    opt.stateFile = cfg.rom + ".fcstate";

    if (cfg.net != LaunchConfig::Net::Solo) {
        std::string err;
        bool ok = false;
        if (cfg.relay && !opt.delaySet) cfg.delay = kRelayDefaultDelay;   // 中继多一跳，默认给足

        if (cfg.relay) {
            RelayConfig rc;
            rc.host = cfg.relayIp;
            rc.port = u16(cfg.relayPort);
            rc.room = cfg.room;
            const std::string waitTitle = "房间 " + cfg.room + " 等待对端…";
            std::printf("模式    : %s（经中继 %s:%u，房间 %s），输入延迟 %d 帧\n",
                        cfg.net == LaunchConfig::Net::Host ? "主机" : "客机",
                        cfg.relayIp.c_str(), cfg.relayPort, cfg.room.c_str(), cfg.delay);
            std::printf("          本地无需放行端口；等待对端加入同一房间（按 Esc 取消）...\n");
            std::fflush(stdout);
            ok = cfg.net == LaunchConfig::Net::Host
                ? net.startHostViaRelay(rc, cfg.delay, &err, [&] { return waitScreen(waitTitle); })
                : net.startClientViaRelay(rc, cfg.delay, &err, [&] { return waitScreen(waitTitle); });
        } else if (cfg.net == LaunchConfig::Net::Host) {
            std::printf("模式    : 主机，监听 0.0.0.0:%d，输入延迟 %d 帧\n", cfg.hostPort, cfg.delay);
            std::printf("          等待客机连接中（按 Esc 取消）...\n");
            std::fflush(stdout);
            ok = net.startHost(u16(cfg.hostPort), cfg.delay, &err,
                               [&] { return waitScreen("等待客机连接中…"); });
        } else {
            std::printf("模式    : 客机，连接 %s:%d，输入延迟 %d 帧\n",
                        cfg.hostIp.c_str(), cfg.connectPort, cfg.delay);
            ok = net.startClient(cfg.hostIp, u16(cfg.connectPort), cfg.delay, &err,
                                 [&] { return waitScreen("正在连接主机…"); });
        }

        if (!ok) {
            // 取消（Esc）或超时：直接切回单机继续玩，而不是让用户白等一场再退出。
            std::fprintf(stderr, "联机失败: %s\n", err.c_str());
            if (cfg.relay) {
                std::fprintf(stderr,
                             "已切回单机模式。提示: ① 中继服务器需放行 UDP %d（不是 TCP）；"
                             "② 两端房间号必须完全一致；③ 房间号被别人占了就换一个。\n",
                             cfg.relayPort);
            } else {
                std::fprintf(stderr, "已切回单机模式。提示: 主机需放行 UDP 端口；公网互联时需在主机做端口映射。\n");
            }
            net.stop();
            cfg.net = LaunchConfig::Net::Solo;
            std::printf("模式    : 单机（联机未建立）\n");
        } else {
            if (net.relayed()) {
                std::printf("联机    : 已连接（经中继 %s:%u，房间 %s），同步方式 %s\n",
                            net.relayConfig().host.c_str(), net.relayConfig().port,
                            net.relayConfig().room.c_str(), net.strategy().name());
            } else {
                std::printf("联机    : 已连接（%s），同步方式 %s\n",
                            net.peer().c_str(), net.strategy().name());
            }
            std::printf("延迟    : %d 帧\n", net.inputDelay());
            std::printf("座位    : 你控制 %s；用本机键盘的 %s 键位\n",
                        net.role() == NetSession::Role::Host ? "1P" : "2P",
                        net.role() == NetSession::Role::Host ? "P1" : "P2");
        }
    } else {
        std::printf("模式    : 单机\n");
    }

    // ------------------------------------------------------------ 音频
    SDL_AudioDeviceID adev = 0;
    SDL_AudioSpec have{};
    if (cfg.audio) {
        SDL_AudioSpec want{};
        want.freq = 44100;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = 1024;
        want.callback = nullptr;
        adev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (adev) {
            emu.bus().apu().setSampleRate(have.freq);
            SDL_PauseAudioDevice(adev, 0);
            std::printf("音频    : %d Hz\n", have.freq);
        } else {
            std::fprintf(stderr, "音频设备打开失败: %s（已静音运行）\n", SDL_GetError());
        }
    } else {
        std::printf("音频    : 已关闭\n");
    }

    // ------------------------------------------------------------ 键位
    KeyMap keymap;
    keymap.setDefaults();
    if (keymap.load(keyFile)) {
        std::printf("键位    : 已从 %s 载入自定义设置\n", keyFile.c_str());
    } else {
        std::printf("键位    : 使用默认（P1 = WASD + JK，P2 = 方向键 + Z/X）\n");
        std::printf("          在启动器或暂停菜单里可以自定义，改完自动存到 %s\n", keyFile.c_str());
    }

    // ------------------------------------------------------------ 手柄
    std::vector<SDL_GameController*> pads;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            if (SDL_GameController* gc = SDL_GameControllerOpen(i)) {
                pads.push_back(gc);
                std::printf("手柄    : %s\n", SDL_GameControllerName(gc));
            }
        }
    }

    // ------------------------------------------------------------ 主循环
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, kScreenWidth, kScreenHeight);
    if (!tex) {
        std::fprintf(stderr, "创建纹理失败: %s\n", SDL_GetError());
        for (SDL_GameController* gc : pads) SDL_GameControllerClose(gc);
        if (adev) SDL_CloseAudioDevice(adev);
        ui.shutdown(); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
        return 1;
    }
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);

    std::vector<u32> pixels(kScreenPixels, 0xFF000000u);
    std::vector<s16> samples;
    samples.reserve(8192);

    const u64 perfFreq = SDL_GetPerformanceFrequency();
    u64 prevTick = SDL_GetPerformanceCounter();
    u64 nextTick = prevTick;

    bool running = true;
    bool fullscreen = opt.fullscreen || cfg.fullscreen;
    if (fullscreen) SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);

    int  curScale = cfg.scale;
    bool menuOpen = false;
    std::string menuToast;
    bool   menuToastWarn = false;
    double menuToastUntil = 0.0;

    u32 fpsT0 = SDL_GetTicks();
    int fpsFrames = 0, fps = 0;
    u32 titleT0 = SDL_GetTicks();
    bool netBroken = false;
    int  audioDrops = 0;             // 音频被丢弃的连续帧数（用于限频报警）

    // 键位配置热加载：记下「上次生效时的文件内容」，每秒比对一次。
    // 用内容比对而不是修改时间 —— mtime 在 Windows 上只有秒级精度，
    // 同一秒内改两次会漏检；配置文件只有 1KB 左右，读一次不要钱。
    std::string keyCfgText;
    u32 keyWatchT0 = SDL_GetTicks();
    { std::vector<u8> raw; if (readFile(keyFile, raw)) keyCfgText.assign(raw.begin(), raw.end()); }

    const u64 fpsLogT0 = SDL_GetPerformanceCounter();
    int fpsLogDone = 0;

    // 打开 / 关闭暂停菜单时要清一次控件动画表，否则上次的悬停高亮会残留
    auto setMenu = [&](bool open) {
        menuOpen = open;
        ui::resetAnimSteps();
    };

    while (running) {
        // ------------------------------------------------ 计时
        const u64 nowT = SDL_GetPerformanceCounter();
        double dt = double(nowT - prevTick) / double(perfFreq);
        prevTick = nowT;
        if (dt > 0.1) dt = 0.1;
        ui.tick(dt);

        // ------------------------------------------------ 输入快照
        ui::Input in;
        {
            int mw = 0, mh = 0;
            const Uint32 mstate = SDL_GetMouseState(&mw, &mh);
            int ow = 0, ww = 0;
            SDL_GetRendererOutputSize(ren, &ow, nullptr);
            SDL_GetWindowSize(win, &ww, nullptr);
            const float dpiK = (ww > 0) ? float(ow) / float(ww) : 1.0f;
            in.mx = float(mw) * dpiK;
            in.my = float(mh) * dpiK;
            in.down = (mstate & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
        }

        // ------------------------------------------------ 事件
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT:
                    running = false;
                    break;

                case SDL_WINDOWEVENT:
                    if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                        ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                        int nw = 0, nh = 0;
                        SDL_GetRendererOutputSize(ren, &nw, &nh);
                        ui.setViewport(nw, nh);
                        ui::resetAnimSteps();
                    }
                    break;

                case SDL_MOUSEMOTION:
                    in.mx = float(ev.motion.x);
                    in.my = float(ev.motion.y);
                    break;

                case SDL_MOUSEBUTTONDOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        in.pressed = true;
                        in.mx = float(ev.button.x);
                        in.my = float(ev.button.y);
                    }
                    break;

                case SDL_MOUSEWHEEL:
                    in.wheel += float(ev.wheel.y);
                    break;

                case SDL_CONTROLLERDEVICEADDED:
                    if (SDL_IsGameController(ev.cdevice.which)) {
                        if (SDL_GameController* gc = SDL_GameControllerOpen(ev.cdevice.which))
                            pads.push_back(gc);
                    }
                    break;

                case SDL_CONTROLLERDEVICEREMOVED: {
                    SDL_GameController* gc = SDL_GameControllerFromInstanceID(ev.cdevice.which);
                    if (gc) {
                        pads.erase(std::remove(pads.begin(), pads.end(), gc), pads.end());
                        SDL_GameControllerClose(gc);
                    }
                    break;
                }

                case SDL_KEYDOWN: {
                    const SDL_Keycode k = ev.key.keysym.sym;

                    if (k == SDLK_F1 || k == SDLK_ESCAPE) {
                        setMenu(!menuOpen);
                    } else if (k == SDLK_F11) {
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    } else if (k == SDLK_F5) {
                        // 存档只是「把当前状态读出来写盘」，不改变模拟状态，所以联机中也安全。
                        std::vector<u8> buf;
                        emu.saveState(buf);
                        if (writeFile(opt.stateFile, buf)) {
                            std::printf("[存档] %zu 字节 -> %s\n", buf.size(), opt.stateFile.c_str());
                            menuToast = "已保存存档";
                            menuToastWarn = false;
                            menuToastUntil = double(SDL_GetTicks()) + 2200.0;
                        }
                    } else if (k == SDLK_F8) {
                        if (net.active()) {
                            // 锁步（feed-forward 帧同步）要求两端在同一帧上状态逐位一致。
                            // 单方面 loadState 会立刻让本机状态偏离对端，且无法再自行收敛，
                            // 所以这里必须拦住，而不是让它去触发一次「不同步」告警。
                            std::fprintf(stderr,
                                         "[读档] 联机中不能读档：锁步要求两端逐帧状态一致，"
                                         "单方面读档会立刻不同步。\n"
                                         "       需要读档请先在暂停菜单里退出联机，单机读完档再重新联机。\n");
                            menuToast = "联机中不能读档";
                            menuToastWarn = true;
                            menuToastUntil = double(SDL_GetTicks()) + 2600.0;
                        } else {
                            std::vector<u8> buf;
                            std::string why;
                            if (readFile(opt.stateFile, buf) && emu.loadState(buf.data(), buf.size(), &why)) {
                                std::printf("[读档] 成功（第 %llu 帧）\n",
                                            (unsigned long long)emu.frameCount());
                                menuToast = "已读取存档";
                                menuToastWarn = false;
                            } else {
                                std::printf("[读档] 失败：%s\n",
                                            why.empty() ? "读不到文件，先按 F5 存一个" : why.c_str());
                                menuToast = why.empty() ? "没有可用的存档" : "读档失败";
                                menuToastWarn = true;
                            }
                            menuToastUntil = double(SDL_GetTicks()) + 2200.0;
                        }
                    } else if (k == SDLK_F2) {
                        // 锁步联机时本机一停下来对端就会干等这一帧，所以联机中不开设置界面。
                        if (net.active()) {
                            std::fprintf(stderr,
                                         "[键位] 联机中不能打开键位设置（会让对端一直等帧）。"
                                         "请先退出联机、单机改好后再联机。\n");
                            menuToast = "联机中不能改键位";
                            menuToastWarn = true;
                            menuToastUntil = double(SDL_GetTicks()) + 2600.0;
                        } else {
                            // 打开前先从磁盘重读：在外部编辑器手改过配置也能立刻看到最新值
                            {
                                KeyMap fresh;
                                fresh.setDefaults();
                                if (fresh.load(keyFile)) keymap = fresh;
                            }
                            bool keyQuit = false;
                            runKeyConfigScreen(win, ren, ui, keyFile, keymap, &keyQuit);
                            if (keyQuit) running = false;
                            // 界面里可能刚存过盘，刷新基线内容，免得下一轮热加载又提示一次
                            {
                                std::vector<u8> raw;
                                if (readFile(keyFile, raw)) keyCfgText.assign(raw.begin(), raw.end());
                            }
                            ui::resetAnimSteps();
                            titleT0 = 0;                  // 让标题栏立刻刷新
                        }
                    }
                    break;
                }

                default:
                    break;
            }
        }
        if (!running) break;

        // ------------------------------------------------ 本机物理输入
        // 两套键位（可自定义）+ 全部已连接手柄（手柄归入「本机玩家」那一路）
        u8 padBits = 0;
        for (SDL_GameController* gc : pads) padBits |= samplePad(gc);

        const u8 p1 = u8(keymap.sample(0) | padBits);   // 单机时的 1P；联机时即本机玩家输入
        const u8 p2 = keymap.sample(1);                 // 仅单机时使用（联机时由对端输入填充）
        const u8 localPad = p1;

        const bool turbo = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_TAB] != 0;

        // 暂停只在单机成立：联机时停住会让对端一直等这一帧
        const bool paused = menuOpen && !net.active();

        // ------------------------------------------------ 推进模拟
        bool simulated = false;
        bool waiting   = false;
        if (!paused) {
            if (net.active()) {
                // 锁步：等待远端延迟帧输入后再跑当前帧
                const u32 simFrame = net.beginFrame(localPad);
                if (net.disconnected()) {
                    std::fprintf(stderr, "\n[联机] 与 %s 的连接已断开，切回单机。\n", net.peer().c_str());
                    net.stop();
                    netBroken = true;
                } else if (simFrame == NetSession::NO_FRAME) {
                    // 对端还没到这一帧：本机先等，但界面照常刷新（菜单也还能点）
                    waiting = true;
                } else {
                    // 座位固定：host 坐 1P、client 坐 2P，对端输入填另一路。
                    // 两端填完后各自看到的 P1/P2 完全镜像，所以状态哈希仍可逐位比对。
                    const u8 mine   = net.localInput(simFrame);
                    const u8 theirs = net.remoteInput(simFrame);
                    if (net.role() == NetSession::Role::Host) {
                        emu.bus().player1().setButtons(mine);
                        emu.bus().player2().setButtons(theirs);
                    } else {
                        emu.bus().player1().setButtons(theirs);
                        emu.bus().player2().setButtons(mine);
                    }
                    emu.runFrame();
                    simulated = true;
                    if ((simFrame % 120) == 0) net.sendHash(simFrame, emu.stateHash());
                    // 只播报「新产生」的不同步，否则同一次不同步会每帧刷屏
                    NetSession::DesyncInfo di;
                    if (net.takeDesyncEvent(di)) {
                        std::fprintf(stderr,
                                     "\n[联机] 警告: 检测到状态不同步（可能 Mapper 或 ROM 版本不一致）\n"
                                     "       帧 %u  本机 %016llX  对端 %016llX\n",
                                     di.frame, (unsigned long long)di.local,
                                     (unsigned long long)di.remote);
                        menuToast = "检测到状态不同步";
                        menuToastWarn = true;
                        menuToastUntil = double(SDL_GetTicks()) + 4000.0;
                    }
                }
            } else {
                emu.bus().player1().setButtons(p1);
                emu.bus().player2().setButtons(p2);
                emu.runFrame();
                simulated = true;
                if (turbo) { emu.bus().player1().setButtons(p2); emu.runFrame(); }
            }
        }
        if (netBroken) { netBroken = false; }   // 已切回单机，正常继续

        // ------------------------------------------------ 画面
        {
            const SDL_Rect gr = gameRectFor(ui.width(), ui.height(), curScale);
            if (simulated) {
                const u32* fb = emu.framebuffer();
                for (int i = 0; i < kScreenPixels; ++i) pixels[size_t(i)] = 0xFF000000u | fb[i];
            }
            ui.clear(ui::rgba(0, 0, 0));
            SDL_UpdateTexture(tex, nullptr, pixels.data(), kScreenWidth * 4);
            SDL_RenderCopy(ren, tex, nullptr, &gr);

            if (menuOpen) {
                PauseInfo pi;
                pi.romName   = title;
                pi.fps       = fps;
                pi.netActive = net.active();
                pi.netRelayed = net.relayed();
                pi.desynced  = net.desynced();
                if (net.active()) {
                    pi.netTag = net.relayed()
                        ? ("中继 · 房间 " + net.relayConfig().room)
                        : std::string(netRoleName(net.role()));
                    pi.lag   = net.lag();
                    pi.delay = net.inputDelay();
                }
                pi.hasState   = fileExists(opt.stateFile);
                pi.fullscreen = fullscreen;
                pi.scale      = curScale;
                pi.toast      = (double(SDL_GetTicks()) < menuToastUntil) ? menuToast : std::string();
                pi.toastWarn  = menuToastWarn;

                const PauseAction act = pauseMenuFrame(ui, pi, in, dt);
                switch (act) {
                    case PauseAction::Resume:
                        setMenu(false);
                        break;
                    case PauseAction::SaveState: {
                        std::vector<u8> buf;
                        emu.saveState(buf);
                        if (writeFile(opt.stateFile, buf)) {
                            std::printf("[存档] %zu 字节 -> %s\n", buf.size(), opt.stateFile.c_str());
                            menuToast = "已保存存档";
                            menuToastWarn = false;
                        } else {
                            menuToast = "存档写入失败";
                            menuToastWarn = true;
                        }
                        menuToastUntil = double(SDL_GetTicks()) + 2200.0;
                        break;
                    }
                    case PauseAction::LoadState: {
                        if (net.active()) {
                            menuToast = "联机中不能读档";
                            menuToastWarn = true;
                        } else {
                            std::vector<u8> buf;
                            std::string why;
                            if (readFile(opt.stateFile, buf) && emu.loadState(buf.data(), buf.size(), &why)) {
                                std::printf("[读档] 成功（第 %llu 帧）\n",
                                            (unsigned long long)emu.frameCount());
                                menuToast = "已读取存档";
                                menuToastWarn = false;
                            } else {
                                std::printf("[读档] 失败：%s\n", why.c_str());
                                menuToast = why.empty() ? "没有可用的存档" : "读档失败";
                                menuToastWarn = true;
                            }
                        }
                        menuToastUntil = double(SDL_GetTicks()) + 2200.0;
                        break;
                    }
                    case PauseAction::KeyConfig: {
                        if (net.active()) {
                            menuToast = "联机中不能改键位";
                            menuToastWarn = true;
                            menuToastUntil = double(SDL_GetTicks()) + 2600.0;
                        } else {
                            KeyMap fresh;
                            fresh.setDefaults();
                            if (fresh.load(keyFile)) keymap = fresh;
                            bool keyQuit = false;
                            runKeyConfigScreen(win, ren, ui, keyFile, keymap, &keyQuit);
                            {
                                std::vector<u8> raw;
                                if (readFile(keyFile, raw)) keyCfgText.assign(raw.begin(), raw.end());
                            }
                            ui::resetAnimSteps();
                            prevTick = SDL_GetPerformanceCounter();   // 别把停留时间算进 dt
                            if (keyQuit) running = false;
                        }
                        break;
                    }
                    case PauseAction::ToggleFullscreen:
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                        ui::resetAnimSteps();
                        break;
                    case PauseAction::CycleScale:
                        curScale = nextScale(curScale);
                        break;
                    case PauseAction::Quit:
                        running = false;
                        break;
                    default:
                        break;
                }
            }
            SDL_RenderPresent(ren);
        }

        // 等对端时不消耗帧预算（否则等待期间会白算掉节拍、越等越晚），但要先把画面刷出去
        if (waiting) {
            SDL_Delay(1);
            continue;
        }

        // ------------------------------------------------ 音频
        if (adev) {
            samples.clear();
            emu.bus().apu().takeSamples(samples);
            if (!samples.empty()) {
                // 声卡按 1 倍速消费，本机每帧产出 1/60 秒的样本，正常时积压一直在
                // 「半帧」上下（≈8ms）。可一旦模拟快于声卡，积压就会单调上涨：
                // 旧代码的安全阀写死在 0.4 秒，涨到那儿就长期卡住 —— 听到的是 0.4 秒
                // 以前的声音、且绝大部分样本被丢掉，听感就是「回音」。
                // 现在按实际设备采样率计算，并把安全阀收紧到约 0.12 秒。
                const Uint32 cap = Uint32(double(have.freq) * double(sizeof(s16)) * 0.12);
                const Uint32 queued = SDL_GetQueuedAudioSize(adev);
                if (queued < cap) {
                    SDL_QueueAudio(adev, samples.data(), Uint32(samples.size() * sizeof(s16)));
                } else if (++audioDrops == 60) {     // 约每秒提示一次，不刷屏
                    audioDrops = 0;
                    std::fprintf(stderr, "[音频] 积压 %.0f ms，已丢弃本帧样本（声音会断续）\n",
                                 double(queued) * 1000.0 / (double(have.freq) * sizeof(s16)));
                }
            }
        }

        // ------------------------------------------------ 帧率统计
        fpsFrames++;
        if (SDL_GetTicks() - fpsT0 >= 1000) {
            fps = fpsFrames;
            fpsFrames = 0;
            fpsT0 = SDL_GetTicks();
        }

        // ------------------------------------------------ 帧节拍
        // 单机和联机都必须节流。
        //
        // 曾经的写法是「联机时跳过节流，反正锁步等待会自行节流」—— 这是错的：
        // 输入延迟锁步只提供「上限」约束（本机不会跑到对端前面），不提供「节奏」约束。
        // 两端各自一路跑下去时，对端第 (slot - delay) 帧的输入早就躺在本地缓冲区里了，
        // waitForRemote() 立刻返回、根本不等，于是模拟就以 CPU 能跑多快跑多快 ——
        // 表现就是「联机比单机快很多」。所以联机也要按 kFrameMs 打拍子。
        //
        // 暂停时同样要打拍子（否则暂停期间的 dt 会累积成一大坨，恢复时一顿猛跑）。
        {
            const u64 now = SDL_GetPerformanceCounter();
            // Tab 加速只在单机生效：联机时钟必须两端一致，本机抢跑没有意义（会被对端拖住）
            const double span = (turbo && !net.active()) ? kFrameMs / 4.0 : kFrameMs;
            nextTick += u64(double(perfFreq) * span / 1000.0);
            if (now < nextTick) {
                const double ms = double(nextTick - now) * 1000.0 / double(perfFreq);
                if (ms > 0.6) SDL_Delay(Uint32(ms - 0.5));
            } else if (double(now - nextTick) * 1000.0 / double(perfFreq) > 120.0) {
                nextTick = now;   // 落后过多则重置节拍，避免雪崩
            }
        }

        // ------------------------------------------------ 键位配置热加载
        // 每秒比对一次内容：在外部编辑器改完保存，约 1 秒内自动生效。
        // 联机中也允许 —— 键位只是「物理键 → 手柄位」的本地映射，
        // 传出去的是算好的手柄位，改键不会影响两端的确定性。
        if (SDL_GetTicks() - keyWatchT0 > 1000) {
            keyWatchT0 = SDL_GetTicks();
            std::vector<u8> raw;
            if (readFile(keyFile, raw)) {
                const std::string txt(raw.begin(), raw.end());
                if (txt != keyCfgText) {
                    KeyMap fresh;
                    fresh.setDefaults();
                    if (fresh.load(keyFile)) {       // 解析失败就保持当前映射，不要把键位清空
                        keymap = fresh;
                        keyCfgText = txt;
                        std::printf("[键位] 检测到 %s 有改动，已热加载生效\n", keyFile.c_str());
                        std::fflush(stdout);
                    } else {
                        std::fprintf(stderr, "[键位] %s 有改动但解析失败，继续沿用当前键位\n",
                                     keyFile.c_str());
                    }
                }
            }
        }

        // ------------------------------------------------ 开发者自测：实测帧率
        if (opt.fpsLog > 0 && ++fpsLogDone >= opt.fpsLog) {
            const double sec = double(SDL_GetPerformanceCounter() - fpsLogT0) / double(perfFreq);
            std::printf("实测帧率 : %d 帧 / %.3f 秒 = %.2f FPS（%s）\n", opt.fpsLog, sec,
                        sec > 0 ? double(opt.fpsLog) / sec : 0.0,
                        net.active() ? "联机" : "单机");
            std::fflush(stdout);
            running = false;
        }

        // ------------------------------------------------ 标题栏
        if (SDL_GetTicks() - titleT0 > 500) {
            titleT0 = SDL_GetTicks();
            char buf[256];
            if (net.active()) {
                // 中继模式把「主机/客机」换成「中继·房间 xxx」，一眼能看出走的哪条路
                const std::string tag = net.relayed()
                    ? ("中继·房间 " + net.relayConfig().room)
                    : std::string(netRoleName(net.role()));
                char des[40] = "";
                if (net.desynced()) std::snprintf(des, sizeof(des), "  [不同步!]");
                std::snprintf(buf, sizeof(buf), "%s  |  FPS %d  |  %s %s lag %d  delay %d%s",
                              title.c_str(), fps, tag.c_str(),
                              net.role() == NetSession::Role::Host ? "(1P)" : "(2P)",
                              net.lag(), net.inputDelay(), des);
            } else {
                std::snprintf(buf, sizeof(buf), "%s  |  FPS %d  |  单机  |  Esc 菜单",
                              title.c_str(), fps);
            }
            SDL_SetWindowTitle(win, buf);
        }
    }

    net.stop();
    for (SDL_GameController* gc : pads) SDL_GameControllerClose(gc);
    if (adev) SDL_CloseAudioDevice(adev);
    SDL_DestroyTexture(tex);
    ui.shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    std::printf("已退出。\n");
    return 0;
}
