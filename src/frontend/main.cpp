// main.cpp — SDL2 前端：窗口/渲染/音频/输入/主循环/联机接入
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/emulator.h"
#include "net/net.h"

#include "keycfg.h"
#include "keyscreen.h"

#ifdef _WIN32
#  include <windows.h>      // SetConsoleOutputCP：修 Windows 控制台中文乱码
#endif

using namespace fc;

namespace {

constexpr double kFrameMs = 1000.0 / 60.0988;   // NTSC 帧周期

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
    std::string keyPreview;   // 开发者自测：把键位界面导出成 PPM 后退出
    int fpsLog = 0;           // 开发者自测：跑满 N 帧后打印实测帧率并退出（0=关闭）
};

void usage() {
    std::printf(
        "fc-emulator — FC/NES 模拟器（SDL2 + 锁步联机）\n"
        "\n"
        "用法:\n"
        "  fc-emulator <rom.nes> [选项]\n"
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
        "      （中继模式下 --host 的端口与 --connect 的地址都可省略）\n"
        "\n"
        "显示/音频:\n"
        "  --scale <n>                窗口放大倍率，默认 3\n"
        "  --fullscreen               全屏启动\n"
        "  --no-audio                 关闭音频\n"
        "\n"
        "按键:\n"
        "  默认 P1 方向=WASD   A=J  B=K  Select=右Shift  Start=回车\n"
        "  默认 P2 方向=方向键 A=Z  B=X  Select=右Ctrl   Start=右Alt\n"
        "  F2=键位设置（可逐键自定义，自动存到 fc-keys.cfg）\n"
        "  --keys <文件>              指定键位配置文件（默认 <exe目录>/fc-keys.cfg）\n"
        "  配置文件支持热加载：在外部编辑器改完保存，约 1 秒内自动生效（联机中也生效）\n"
        "  --keypreview <图.ppm>      开发者自测：把键位界面导出成图片后退出\n"
        "  手柄自动识别（A/B/Start/Back/十字键/左摇杆）\n"
        "\n"
        "存档:\n"
        "  F5=存档（写到 <rom>.fcstate）  F8=读档\n"
        "  单机：F5 / F8 都可用。\n"
        "  联机：F5 仍可存档（纯本地读取、不影响同步），但 F8 读档被禁用。\n"
        "  --load-state <文件>        启动时先载入存档，再进入游戏/联机\n"
        "  想「从某个进度接着联机」就用它：双方各自拿到同一个档，然后\n"
        "      fc-emulator game.nes --load-state resume.fcstate --host 7777\n"
        "      fc-emulator game.nes --load-state resume.fcstate --connect <IP>\n"
        "  两端 ROM 与存档必须完全一致，否则一连上就会报不同步（这是故意的）。\n"
        "\n"
        "其它:\n"
        "  F11=全屏  Tab=加速(按住，仅单机)  Esc=退出\n"
        "  --fps-log <帧数>           开发者自测：跑满 N 帧后打印实测帧率并退出\n");
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
        } else if (a == "--keypreview") {
            o.keyPreview = next("");
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

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Windows 控制台默认代码页是 GBK(936)，而源文件里的中文按 UTF-8 存储，
    // 直接输出必然乱码。切到 UTF-8 即可（Win10 1903+ 的 conhost 与 Windows Terminal 均支持）。
    SetConsoleOutputCP(CP_UTF8);
#endif
    Options opt;
    if (!parseArgs(argc, argv, opt)) { usage(); return 1; }
    if (opt.rom.empty()) { usage(); return 1; }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL 初始化失败: %s\n", SDL_GetError());
        return 1;
    }

    Emulator emu;
    if (!emu.loadROM(opt.rom)) {
        std::fprintf(stderr, "ROM 加载失败（需要 iNES/NES2.0 格式的 .nes 文件）: %s\n", opt.rom.c_str());
        SDL_Quit();
        return 1;
    }

    const std::string title = baseName(opt.rom);
    std::printf("=== fc-emulator ===\n");
    std::printf("ROM     : %s\n", title.c_str());
    std::printf("Mapper  : %s\n", emu.cartridge().mapperName());
    std::printf("镜像    : %d\n", emu.cartridge().mirrorMode());

    // ------------------------------------------------------------ 启动即读档
    // 这是「从某个进度接着联机」的关键：Esc 会直接退出整个程序，而联机只能由
    // 命令行开启，所以没有这个选项时根本没法带着内存里的档进入联机（一重启就丢）。
    // 加载失败必须硬报错退出 —— 若静默地从开机状态起跑，联机时只会看到莫名其妙的
    // 「不同步」，反而更难查。
    if (!opt.loadState.empty()) {
        std::vector<u8> buf;
        std::string why;
        if (!readFile(opt.loadState, buf)) {
            std::fprintf(stderr, "读档失败: 打不开 %s\n", opt.loadState.c_str());
            SDL_Quit();
            return 1;
        }
        if (!emu.loadState(buf.data(), buf.size(), &why)) {
            std::fprintf(stderr, "读档失败: %s —— %s\n", opt.loadState.c_str(), why.c_str());
            SDL_Quit();
            return 1;
        }
        std::printf("读档    : %s（从第 %llu 帧继续）\n",
                    opt.loadState.c_str(), (unsigned long long)emu.frameCount());
    }

    // ------------------------------------------------------------ 窗口
    // 必须在联机握手之前建好：主机等客机时窗口要立刻可见，
    // 否则看起来就像程序卡死了（旧版正是这个表现）。
    SDL_Window* win = SDL_CreateWindow(
        title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kScreenWidth * opt.scale, kScreenHeight * opt.scale,
        SDL_WINDOW_RESIZABLE | (opt.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) { std::fprintf(stderr, "创建窗口失败: %s\n", SDL_GetError()); SDL_Quit(); return 1; }

    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_RenderSetLogicalSize(ren, kScreenWidth, kScreenHeight);

    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, kScreenWidth, kScreenHeight);
    if (!tex) { std::fprintf(stderr, "创建纹理失败: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);

    // ------------------------------------------------------------ 联机
    NetSession net;
    opt.stateFile = opt.rom + ".fcstate";

    // 握手等待期间被反复调用：泵 SDL 事件（窗口因此不会「未响应」）+ 重绘等待动画，
    // 按 Esc 或关闭窗口即取消。SDL2 不带字体，所以用三个跳动的方块表示「进行中」。
    u32 lastDraw = 0;
    auto waitScreen = [&](const char* what) -> bool {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) return false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) return false;
        }
        SDL_SetWindowTitle(win, what);                 // SDL 内部会比较，重复设置无开销

        const u32 t = SDL_GetTicks();
        if (t - lastDraw < 16) return true;            // 重绘限到 ~60fps
        lastDraw = t;

        const int active = int((t / 320) % 3);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        for (int i = 0; i < 3; ++i) {
            const bool on = (i == active);
            const int  sz = on ? 20 : 12;
            const Uint8 v = on ? 235 : 70;
            SDL_SetRenderDrawColor(ren, v, v, v, 255);
            SDL_Rect r{ kScreenWidth / 2 + (i - 1) * 44 - sz / 2, kScreenHeight / 2 - sz / 2, sz, sz };
            SDL_RenderFillRect(ren, &r);
        }
        SDL_RenderPresent(ren);
        return true;
    };

    if (opt.host || opt.connect) {
        std::string err;
        bool ok = false;
        if (opt.relay && !opt.delaySet) opt.delay = kRelayDefaultDelay;   // 中继多一跳，默认给足

        if (opt.relay) {
            RelayConfig rc;
            rc.host = opt.relayIp;
            rc.port = opt.relayPort;
            rc.room = opt.room;
            const std::string waitTitle =
                "fc-emulator — 房间 " + opt.room + " 等待对端…（Esc 取消）";
            std::printf("模式    : %s（经中继 %s:%u，房间 %s），输入延迟 %d 帧\n",
                        opt.host ? "主机" : "客机",
                        opt.relayIp.c_str(), opt.relayPort, opt.room.c_str(), opt.delay);
            std::printf("          本地无需放行端口；等待对端加入同一房间（按 Esc 取消）...\n");
            std::fflush(stdout);
            ok = opt.host
                ? net.startHostViaRelay(rc, opt.delay, &err,
                                        [&] { return waitScreen(waitTitle.c_str()); })
                : net.startClientViaRelay(rc, opt.delay, &err,
                                          [&] { return waitScreen(waitTitle.c_str()); });
        } else if (opt.host) {
            std::printf("模式    : 主机，监听 0.0.0.0:%u，输入延迟 %d 帧\n", opt.hostPort, opt.delay);
            std::printf("          等待客机连接中（按 Esc 取消）...\n");
            std::fflush(stdout);
            ok = net.startHost(opt.hostPort, opt.delay, &err,
                               [&] { return waitScreen("fc-emulator — 等待客机连接中…（Esc 取消）"); });
        } else {
            std::printf("模式    : 客机，连接 %s:%u，输入延迟 %d 帧\n",
                        opt.hostIp.c_str(), opt.connectPort, opt.delay);
            ok = net.startClient(opt.hostIp, opt.connectPort, opt.delay, &err,
                                 [&] { return waitScreen("fc-emulator — 正在连接主机…（Esc 取消）"); });
        }

        if (!ok) {
            // 取消（Esc）或超时：窗口已经开好了，直接切回单机继续玩，
            // 而不是让用户白等一场再退出。
            std::fprintf(stderr, "联机失败: %s\n", err.c_str());
            if (opt.relay) {
                std::fprintf(stderr,
                             "已切回单机模式。提示: ① 中继服务器需放行 UDP %u（不是 TCP）；"
                             "② 两端 --room 必须完全一致；③ 房间号被别人占了就换一个。\n",
                             opt.relayPort);
            } else {
                std::fprintf(stderr, "已切回单机模式。提示: 主机需放行 UDP 端口；公网互联时需在主机做端口映射。\n");
            }
            net.stop();
            SDL_SetWindowTitle(win, title.c_str());
            std::printf("模式    : 单机（联机未建立）\n");
        } else {
            SDL_SetWindowTitle(win, title.c_str());
            if (net.relayed()) {
                std::printf("联机    : 已连接（经中继 %s:%u，房间 %s），同步方式 %s\n",
                            net.relayConfig().host.c_str(), net.relayConfig().port,
                            net.relayConfig().room.c_str(), net.strategy().name());
            } else {
                std::printf("联机    : 已连接（%s），同步方式 %s\n",
                            net.peer().c_str(), net.strategy().name());
            }
            std::printf("延迟    : %d 帧\n", net.inputDelay());
            std::printf("座位    : 你控制 %s；用本机键盘的 %s 键位（F2 可在单机时自定义）\n",
                        net.role() == NetSession::Role::Host ? "1P" : "2P",
                        net.role() == NetSession::Role::Host ? "P1" : "P2");
        }
    } else {
        std::printf("模式    : 单机\n");
    }

    // ------------------------------------------------------------ 音频
    SDL_AudioDeviceID adev = 0;
    SDL_AudioSpec have{};
    if (!opt.noAudio) {
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
    }

    // ------------------------------------------------------------ 键位
    KeyMap keymap;
    keymap.setDefaults();
    const std::string keyFile = opt.keyFile.empty() ? defaultKeyConfigPath() : opt.keyFile;
    if (keymap.load(keyFile)) {
        std::printf("键位    : 已从 %s 载入自定义设置\n", keyFile.c_str());
        std::printf("          P1 方向=%s/%s/%s/%s  A=%s B=%s  选择=%s 开始=%s\n",
                    keyName(keymap.get(0, ACT_UP)).c_str(), keyName(keymap.get(0, ACT_DOWN)).c_str(),
                    keyName(keymap.get(0, ACT_LEFT)).c_str(), keyName(keymap.get(0, ACT_RIGHT)).c_str(),
                    keyName(keymap.get(0, ACT_A)).c_str(), keyName(keymap.get(0, ACT_B)).c_str(),
                    keyName(keymap.get(0, ACT_SELECT)).c_str(), keyName(keymap.get(0, ACT_START)).c_str());
        std::printf("          该文件支持热加载：运行中改完保存，约 1 秒内自动生效\n");
    } else {
        std::printf("键位    : 使用默认（P1 = WASD + JK，P2 = 方向键 + Z/X）\n");
        std::printf("          按 F2 可自定义，改完自动存到 %s（该文件运行中改动会热加载）\n",
                    keyFile.c_str());
    }

    // 开发者自测：把键位设置界面渲染成一张图然后退出。
    // 内置点阵字体是手写的，最容易出现「某个字母画错/整体错位」，需要能肉眼确认。
    if (!opt.keyPreview.empty()) {
        std::vector<u32> shot(kScreenPixels, 0);
        KeyScreenView kv;
        kv.fileTag = "fc-keys.cfg";
        drawKeyConfigScreen(shot.data(), keymap, kv);
        if (writePPM(opt.keyPreview, shot.data(), kScreenWidth, kScreenHeight))
            std::printf("键位界面已导出: %s\n", opt.keyPreview.c_str());
        else
            std::fprintf(stderr, "导出键位界面失败: %s\n", opt.keyPreview.c_str());
        SDL_Quit();
        return 0;
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
    std::vector<u32> pixels(kScreenPixels, 0xFF000000u);
    std::vector<s16> samples;
    samples.reserve(8192);

    const u64 perfFreq = SDL_GetPerformanceFrequency();
    u64 nextTick = SDL_GetPerformanceCounter();

    bool running = true;
    bool fullscreen = opt.fullscreen;
    u32 fpsT0 = SDL_GetTicks();
    int fpsFrames = 0, fps = 0;
    u32 titleT0 = SDL_GetTicks();
    bool netBroken = false;
    u32 desyncFrame = 0xFFFFFFFFu;   // 首次不同步发生的帧号（用于标题栏定位）

    int  audioDrops = 0;             // 音频被丢弃的连续帧数（用于限频报警）

    // 键位配置热加载：记下「上次生效时的文件内容」，每秒比对一次。
    // 用内容比对而不是修改时间 —— mtime 在 Windows 上只有秒级精度，
    // 同一秒内改两次会漏检；配置文件只有 1KB 左右，读一次不要钱。
    std::string keyCfgText;
    u32 keyWatchT0 = SDL_GetTicks();
    { std::vector<u8> raw; if (readFile(keyFile, raw)) keyCfgText.assign(raw.begin(), raw.end()); }

    // 开发者自测：实测帧率（--fps-log），用来验证「联机不再比单机快」
    const u64 fpsLogT0 = SDL_GetPerformanceCounter();
    int fpsLogDone = 0;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT:
                    running = false;
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
                case SDL_KEYDOWN:
                    if (ev.key.keysym.sym == SDLK_F2) {
                        // 锁步联机时本机一停下来对端就会干等这一帧，所以联机中不开设置界面。
                        if (net.active()) {
                            std::fprintf(stderr,
                                         "[键位] 联机中不能打开键位设置（会让对端一直等帧）。"
                                         "请先退出联机、单机改好后再联机。\n");
                        } else {
                            // 打开前先从磁盘重读：在外部编辑器手改过配置也能立刻看到最新值
                            {
                                KeyMap fresh;
                                fresh.setDefaults();
                                if (fresh.load(keyFile)) keymap = fresh;
                            }
                            bool keyQuit = false;
                            runKeyConfigScreen(win, ren, keyFile, keymap, &keyQuit);
                            if (keyQuit) running = false;
                            // 界面里可能刚存过盘，刷新基线内容，免得下一轮热加载又提示一次
                            {
                                std::vector<u8> raw;
                                if (readFile(keyFile, raw)) keyCfgText.assign(raw.begin(), raw.end());
                            }
                            titleT0 = 0;                  // 让标题栏立刻刷新
                        }
                    }
                    else if (ev.key.keysym.sym == SDLK_ESCAPE) running = false;
                    else if (ev.key.keysym.sym == SDLK_F11) {
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    } else if (ev.key.keysym.sym == SDLK_F5) {
                        // 存档只是「把当前状态读出来写盘」，不改变模拟状态，所以联机中也安全。
                        std::vector<u8> buf;
                        emu.saveState(buf);
                        if (writeFile(opt.stateFile, buf)) {
                            std::printf("[存档] %zu 字节 -> %s\n", buf.size(), opt.stateFile.c_str());
                            if (net.active())
                                std::printf("       注意: 联机中读档被禁用（会让两端状态分叉），"
                                            "这个档请留到单机或退出联机后使用。\n");
                        }
                    } else if (ev.key.keysym.sym == SDLK_F8) {
                        if (net.active()) {
                            // 锁步（feed-forward 帧同步）要求两端在同一帧上状态逐位一致。
                            // 单方面 loadState 会立刻让本机状态偏离对端，且无法再自行收敛，
                            // 所以这里必须拦住，而不是让它去触发一次「不同步」告警。
                            std::fprintf(stderr,
                                         "[读档] 联机中不能读档：锁步要求两端逐帧状态一致，"
                                         "单方面读档会立刻不同步。\n"
                                         "       需要读档请先退出联机（Esc），单机读完档再重新联机。\n");
                        } else {
                            std::vector<u8> buf;
                            std::string why;
                            if (readFile(opt.stateFile, buf) && emu.loadState(buf.data(), buf.size(), &why))
                                std::printf("[读档] 成功（第 %llu 帧）\n",
                                            (unsigned long long)emu.frameCount());
                            else
                                std::printf("[读档] 失败：%s\n",
                                            why.empty() ? "读不到文件，先按 F5 存一个" : why.c_str());
                        }
                    }
                    break;
                default:
                    break;
            }
        }
        if (!running) break;

        // 本机物理输入：两套键位（可自定义）+ 全部已连接手柄（手柄归入「本机玩家」那一路）
        u8 padBits = 0;
        for (SDL_GameController* gc : pads) padBits |= samplePad(gc);

        u8 p1 = u8(keymap.sample(0) | padBits);     // 单机时的 1P；联机时即本机玩家输入
        u8 p2 = keymap.sample(1);                   // 仅单机时使用（联机时由对端输入填充）
        const u8 localPad = p1;

        const bool turbo = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_TAB] != 0;

        // ------------------------------------------------ 推进模拟
        bool simulated = false;
        if (net.active()) {
            // 锁步：等待远端延迟帧输入后再跑当前帧
            const u32 simFrame = net.beginFrame(localPad);
            if (net.disconnected()) {
                std::fprintf(stderr, "\n[联机] 与 %s 的连接已断开，切回单机。\n", net.peer().c_str());
                net.stop();
                netBroken = true;
            } else if (simFrame == NetSession::NO_FRAME) {
                SDL_Delay(1);
                continue;
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
                }
            }
        } else {
            emu.bus().player1().setButtons(p1);
            emu.bus().player2().setButtons(p2);
            emu.runFrame();
            simulated = true;
            if (turbo) { emu.bus().player1().setButtons(p2); emu.runFrame(); }
        }
        if (!simulated) continue;

        // ------------------------------------------------ 画面
        const u32* fb = emu.framebuffer();
        for (int i = 0; i < kScreenPixels; ++i) pixels[size_t(i)] = 0xFF000000u | fb[i];
        SDL_UpdateTexture(tex, nullptr, pixels.data(), kScreenWidth * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);

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

        // ------------------------------------------------ 节流
        fpsFrames++;
        if (SDL_GetTicks() - fpsT0 >= 1000) {
            fps = fpsFrames;
            fpsFrames = 0;
            fpsT0 = SDL_GetTicks();
        }

        // 帧节拍：单机和联机都必须节流。
        //
        // 曾经的写法是「联机时跳过节流，反正锁步等待会自行节流」—— 这是错的：
        // 输入延迟锁步只提供「上限」约束（本机不会跑到对端前面），不提供「节奏」约束。
        // 两端各自一路跑下去时，对端第 (slot - delay) 帧的输入早就躺在本地缓冲区里了，
        // waitForRemote() 立刻返回、根本不等，于是模拟就以 CPU 能跑多快跑多快 ——
        // 表现就是「联机比单机快很多」。所以联机也要按 kFrameMs 打拍子。
        //
        // 注意节拍点只推进「真正跑了一帧」的路径（NO_FRAME 的等待分支会 continue，
        // 不消耗节拍），否则等待期间会白算掉帧预算、越等越晚。
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
                        std::printf("       P1 方向=%s/%s/%s/%s  A=%s B=%s  选择=%s 开始=%s\n",
                                    keyName(keymap.get(0, ACT_UP)).c_str(),
                                    keyName(keymap.get(0, ACT_DOWN)).c_str(),
                                    keyName(keymap.get(0, ACT_LEFT)).c_str(),
                                    keyName(keymap.get(0, ACT_RIGHT)).c_str(),
                                    keyName(keymap.get(0, ACT_A)).c_str(),
                                    keyName(keymap.get(0, ACT_B)).c_str(),
                                    keyName(keymap.get(0, ACT_SELECT)).c_str(),
                                    keyName(keymap.get(0, ACT_START)).c_str());
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
                if (net.desynced()) {
                    if (desyncFrame == 0xFFFFFFFFu) std::snprintf(des, sizeof(des), "  [不同步!]");
                    else std::snprintf(des, sizeof(des), "  [不同步@帧%u]", desyncFrame);
                }
                std::snprintf(buf, sizeof(buf), "%s  |  FPS %d  |  %s %s lag %d  delay %d%s",
                              title.c_str(), fps, tag.c_str(),
                              net.role() == NetSession::Role::Host ? "(1P)" : "(2P)",
                              net.lag(), net.inputDelay(), des);
            } else {
                std::snprintf(buf, sizeof(buf), "%s  |  FPS %d  |  单机", title.c_str(), fps);
            }
            SDL_SetWindowTitle(win, buf);
        }
    }

    if (netBroken) { /* 已切回单机 */ }
    net.stop();
    for (SDL_GameController* gc : pads) SDL_GameControllerClose(gc);
    if (adev) SDL_CloseAudioDevice(adev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    std::printf("已退出。\n");
    return 0;
}
