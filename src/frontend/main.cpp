// main.cpp — SDL2 前端：启动器 / 窗口 / 渲染 / 音频 / 输入 / 主循环 / 联机接入
//
// 界面架构：
//   没有给 ROM 参数 → 先进图形启动器，选好 ROM 与联机设置再开始；
//   给了 ROM 参数   → 直接进游戏（脚本、批处理、老用法不受影响）。
//   游戏里 Esc / F1 唤出暂停菜单；联机时菜单只是叠加层，模拟不会停 ——
//   锁步一旦停住，对端会一直等这一帧直到超时。
//   暂停菜单里的「返回游戏列表」会退回启动器换卡带：见下面的「回合循环」。
//   它先自动存一次档，再收掉本回合的联机 / 音频 / 纹理，重新走一遍挑卡带流程。
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
#include "core/fs_utf8.h"
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
    std::vector<fc::UiClick> uiClicks;  // --ui-click <x>,<y>：截图时按顺序注入的合成点击
    std::vector<std::pair<std::string, std::string>> cfgSets;  // --cfg-set <键>=<值>：仅本次运行覆盖配置
    float menuClickX = -1.0f, menuClickY = -1.0f;  // --menu-click：进游戏后往暂停菜单里注入一次点击
    bool  autoStart  = false;           // --auto-start：启动器不等点击直接开跑
    std::string saveStateOut;           // --save-state：配合 --fps-log，退出前把当前状态写成存档
    std::string syncState;              // --sync-state：联机中在这个帧号上自动读档并同步给对方
    int   syncAt = 120;                 // --sync-at：那个帧号
    int   resetCartAt = 0;              // --reset-cart-at：联机中在第 N 轮自动「重启卡带」并同步
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
        "  Esc / F1   暂停菜单（继续 / 存档 / 读档 / 键位 / 全屏 / 缩放 / 重启卡带 / 换卡带 / 退出）\n"
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
        "  存档写到 <rom>.fcstate。单机：F5 存、F8 读；联机：F5 照旧可用，\n"
        "  按 F8（或菜单里的「读取存档」）会自动把这份档传给对端，双方一起装上。\n"
        "  --load-state <文件>        启动时先载入存档，再进入游戏/联机\n"
        "  想「从某个进度接着联机」：主机 --load-state 之后开房，客机连上来以后\n"
        "  在暂停菜单里按一次「读取存档」，两端就会落到同一份档上，不必手工拷文件。\n"
        "  两端 ROM 必须完全一致；档与 ROM 不匹配会被当场拒绝（这是故意的）。\n"
        "\n"
        "开发者自测:\n"
        "  --shot <界面> <文件.ppm>   离屏渲染界面并存图后退出\n"
        "                             界面: launcher | netplay | settings | keys | pause\n"
        "  --ui-click <x>,<y>         注入合成点击，可重复给多次、按顺序各占一帧：\n"
        "                             配合 --shot 时喂给离屏预览；不给 --shot 时喂给第一次\n"
        "                             进启动器的实跑循环（用来复现「点一下再点一下」的缺陷）\n"
        "  --cfg-set <键>=<值>        仅本次运行覆盖启动器配置，不写回文件；可重复给多次\n"
        "                             （键名同 fc-launcher.cfg，如 net=1、delay=8、browseDir=C:\\roms）\n"
        "  --menu-click <x>,<y>       进游戏后自动打开暂停菜单，并在第 4 帧往该点注入一次点击\n"
        "                             （复现「只有点暂停菜单某一项才发生」的缺陷）\n"
        "  --auto-start               配合 --menu-click：回到列表后的启动器不等点击直接开跑，\n"
        "                             一条命令跑通「游戏 → 菜单 → 返回列表 → 再进游戏」\n"
        "  --fps-log <帧数>           跑到第 N 个模拟帧为止，打印实测帧率与状态哈希后退出\n"
        "                             （以模拟器自己的帧号为准，所以联机两端给同一个 N\n"
        "                               就能停在同一个绝对帧上，哈希可直接逐位比对）\n"
        "  --save-state <文件>        配合 --fps-log：退出前把当前状态存成 .fcstate\n"
        "                             （用来做联机联调的素材档）\n"
        "  --sync-state <文件>        联机中在第 --sync-at 帧自动「读档并同步给对方」，\n"
        "                             用来无人值守地验证存档同步\n"
        "  --sync-at <帧数>           触发 --sync-state 的帧号，默认 120\n"
        "  --reset-cart-at <轮次>     联机中到第 N 轮自动「重启卡带」并同步给对方，\n"
        "                             用来无人值守地验证联机重启（0=不触发）\n");
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
        } else if (a == "--ui-click") {
            const std::string v = next("");
            const size_t comma = v.find(',');
            if (comma == std::string::npos) {
                std::fprintf(stderr, "错误: --ui-click 需要 <x>,<y> 形式\n");
                return false;
            }
            fc::UiClick c;
            c.x = float(std::atof(v.substr(0, comma).c_str()));
            c.y = float(std::atof(v.substr(comma + 1).c_str()));
            o.uiClicks.push_back(c);
        } else if (a == "--menu-click") {
            const std::string v = next("");
            const size_t comma = v.find(',');
            if (comma == std::string::npos) {
                std::fprintf(stderr, "错误: --menu-click 需要 <x>,<y> 形式\n");
                return false;
            }
            o.menuClickX = float(std::atof(v.substr(0, comma).c_str()));
            o.menuClickY = float(std::atof(v.substr(comma + 1).c_str()));
        } else if (a == "--auto-start") {
            o.autoStart = true;
        } else if (a == "--cfg-set") {
            const std::string v = next("");
            const size_t eq = v.find('=');
            if (eq == std::string::npos) {
                std::fprintf(stderr, "错误: --cfg-set 需要 <键>=<值> 形式\n");
                return false;
            }
            o.cfgSets.emplace_back(v.substr(0, eq), v.substr(eq + 1));
        } else if (a == "--fps-log") {
            o.fpsLog = std::atoi(next("0").c_str());
        } else if (a == "--save-state") {
            o.saveStateOut = next("");
        } else if (a == "--sync-state") {
            o.syncState = next("");
        } else if (a == "--sync-at") {
            o.syncAt = std::atoi(next("120").c_str());
        } else if (a == "--reset-cart-at") {
            o.resetCartAt = std::atoi(next("120").c_str());
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
    std::FILE* f = fc::fopenUtf8(path, "wb");
    if (!f) return false;
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return true;
}

bool readFile(const std::string& path, std::vector<u8>& data) {
    std::FILE* f = fc::fopenUtf8(path, "rb");
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
    std::FILE* f = fc::fopenUtf8(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// 写 PPM（无依赖的图片格式，项目里的 tools/ppm2png.py 可转 PNG）
bool writePPM(const std::string& path, const u32* fb, int w, int h) {
    std::FILE* f = fc::fopenUtf8(path, "wb");
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

// 联机存档同步的进度浮层。两端都会看到它，所以文案要两种角色都读得通 ——
// 光弹在发送方那边的话，另一头的人只会看到游戏莫名其妙卡住不动。
void drawStateSyncOverlay(ui::Ui& ui, const NetSession& net) {
    const int W = ui.width(), H = ui.height();
    ui.fill(SDL_Rect{ 0, 0, W, H }, ui::rgba(4, 6, 10, 205));

    const int pw = 468, ph = 186;
    const SDL_Rect panel{ (W - pw) / 2, (H - ph) / 2, pw, ph };
    ui.shadow(panel, ui::radius::Card, 22, 210);
    ui.round(panel, ui::radius::Card, ui::rgba(20, 26, 35, 252));
    ui.roundOutline(panel, ui::radius::Card, 1, ui::theme::borderHi);
    ui.hline(panel.x + ui::radius::Card, panel.y, panel.w - ui::radius::Card * 2,
             ui::rgba(255, 255, 255, 18));

    ui.text(ui::Font::Title, "联机存档同步", panel.x + 24, panel.y + 20, ui::theme::text);
    ui.text(ui::Font::Small,
            net.stateSending() ? "正在把本机存档发给对方…" : "正在接收对方的存档…",
            panel.x + 24, panel.y + 54, ui::theme::textDim);

    const int pct = net.statePercent();
    const SDL_Rect track{ panel.x + 24, panel.y + 92, pw - 48, 10 };
    ui.round(track, 5, ui::theme::surface);
    const int fw = track.w * pct / 100;
    if (fw > 0) ui.round(SDL_Rect{ track.x, track.y, fw, track.h }, 5, ui::theme::accent);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d%%", pct);
    ui.text(ui::Font::Body, buf, panel.x + 24, panel.y + 112, ui::theme::accent);
    ui.textRight(ui::Font::Small, "同步期间两端都会暂停，完成后自动继续",
                 panel.x + pw - 24, panel.y + 120, ui::theme::textFaint);
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Windows 控制台默认代码页是 GBK(936)，而源文件里的中文按 UTF-8 存储，
    // 直接输出必然乱码。切到 UTF-8 即可（Win10 1903+ 的 conhost 与 Windows Terminal 均支持）。
    SetConsoleOutputCP(CP_UTF8);
#endif
#ifdef _WIN32
    // 路径在程序内部一律按 UTF-8 走（启动器、配置文件里存的都是 UTF-8），而 Windows
    // 传进来的 argv 是系统 ANSI 代码页（中文版为 GBK）。这里统一转一次，中文路径才
    // 不会一边是 GBK、一边是 UTF-8 地对不上。
    std::vector<std::string> argvUtf8;
    argvUtf8.reserve(size_t(argc));
    for (int i = 0; i < argc; ++i) argvUtf8.push_back(fc::ansiToUtf8(argv[i] ? argv[i] : ""));
    for (int i = 0; i < argc; ++i) argv[i] = const_cast<char*>(argvUtf8[size_t(i)].c_str());
#endif
    Options opt;
    if (!parseArgs(argc, argv, opt)) { usage(); return 1; }

    // --cfg-set 的键名先在这里验一遍。放到后面（拿到 cfg 再验）的话，
    // 报错时 SDL 窗口、渲染器、字体都已经起来了，要么带一堆清理代码退出，
    // 要么像原来那样 `return false` —— 那等于用退出码 0 报告「参数写错了」。
    // 用一份临时配置探测，代价是一个结构体的构造。
    {
        LaunchConfig probe;
        for (const auto& kv : opt.cfgSets) {
            if (!probe.setField(kv.first, kv.second)) {
                std::fprintf(stderr, "错误: --cfg-set 不认识这个键: %s\n", kv.first.c_str());
                return 1;
            }
        }
    }

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

    // --cfg-set 放在最后：它只服务于「离屏截图自测某个界面状态」这种场景
    // （例如滑杆只在联机分支才画，得先把 net 顶成 1 才截得到），
    // 所以让它压过前面所有来源，效果最直观。只改内存，不写回配置文件。
    for (const auto& kv : opt.cfgSets) cfg.setField(kv.first, kv.second);
    cfg.clamp();

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
                // 联机态也要能出图：置灰项（读档 / 键位 / 返回列表）+「联机中不可用」
                // 的排布和单机那张完全不同，只看单机那张判断不了对错。
                // 切法：--cfg-set net=1（中继态再补 relay=1）。
                if (cfg.net != LaunchConfig::Net::Solo) {
                    pi.netActive  = true;
                    pi.netRelayed = cfg.relay;
                    pi.netTag     = cfg.relay ? ("中继 · 房间 " + cfg.room)
                                              : std::string(netRoleName(
                                                    cfg.net == LaunchConfig::Net::Host
                                                        ? NetSession::Role::Host
                                                        : NetSession::Role::Client));
                    pi.delay = cfg.delay;
                    pi.lag   = 4;
                }
                ui::Input in;
                in.mx = -100; in.my = -100;
                pauseMenuFrame(ui, pi, in, 0.0);
            } else {
                int tab = 0;
                if (opt.shot == "netplay")  tab = 1;
                if (opt.shot == "settings") tab = 2;
                drawLauncherPreview(ui, cfg, tab, opt.uiClicks);
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

    // ------------------------------------------------------------ 回合循环
    // 暂停菜单里的「返回游戏列表」要把程序退回启动器。启动器 / 载入 ROM / 主循环
    // 三者共享 win、ren、ui、cfg、emu 一大摊上下文，硬拆成函数反而更难读；
    // 就地套一层外层循环，每绕一圈就是「挑卡带 → 跑游戏」一个完整回合，
    // 回列表这件事就退化成把 toLibrary 置位、收尾后重来一轮。
    //
    // 下面这几个句柄声明在外层，是为了让任何一条跳转路径都收得干净 ——
    // 尤其音频设备：漏关一个不会报错，只会让你听到两层声音叠在一起。
    NetSession net;
    SDL_AudioDeviceID adev = 0;
    SDL_AudioSpec have{};
    SDL_Texture* tex = nullptr;
    std::vector<SDL_GameController*> pads;

    bool firstRound = true;      // 首轮：带 --rom 时直接开跑，老用法不变
    bool toLibrary  = false;     // 暂停菜单里点了「返回游戏列表」
    LauncherHandoff handoff;     // 上一局捎给启动器的话（「已自动存档」之类）

    // 开发者自测（--menu-click）：**跨回合**只注入一次。
    // 必须放在回合外 —— 放里面的话每回合都会重新点一次，第二回合立马又被弹回列表，
    // 于是「回到列表之后还能不能再进游戏」这件事就永远测不到了（实测会绕成死循环）。
    int  menuClickFrame = 0;
    bool menuClickFired = false;

    // 存档同步的状态要在「回合内」用（每回合一个 net 会话），但触发标记得跨回合保留，
    // 不然下一回合会莫名其妙又同步一次。
    bool syncFired   = false;
    int  syncFrame   = 0;
    bool resetFired  = false;       // --reset-cart-at 只触发一次
    int  resetFrame  = 0;
    bool stateSyncing = false;      // 上一帧 net 是否在收发存档（用来捕捉「刚刚结束」）

    for (;;) {
        const bool fromLauncher = (!firstRound || opt.rom.empty());
        if (fromLauncher) {
            std::printf("=== fc-emulator 启动器 ===\n");
            handoff.autoStart = opt.autoStart;
            // 合成点击只在「第一次」进启动器时给：回合循环里反复给的话，第二回合又会照着
            // 同一串点再走一遍（例如把用户刚换回来的卡带再点走），整条回路就绕成了死循环。
            if (firstRound) handoff.clicks = opt.uiClicks;
            if (!runLauncher(win, ren, ui, cfg, keyFile, handoff)) break;   // 用户要退出程序
            handoff = LauncherHandoff{};   // 话已经带到了，别糊到下一回合的启动器上
            // 启动器可能改过窗口尺寸，重新同步一次视口
            int ow = 0, oh = 0;
            SDL_GetRendererOutputSize(ren, &ow, &oh);
            ui.setViewport(ow, oh);
        }
        firstRound = false;
        toLibrary  = false;

    // ------------------------------------------------------------ 载入 ROM
    Emulator emu;
    if (!emu.loadROM(cfg.rom)) {
        std::fprintf(stderr, "ROM 加载失败（需要 iNES/NES2.0 格式的 .nes 文件）: %s\n", cfg.rom.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "fc-emulator",
                                 ("ROM 加载失败：\n" + cfg.rom).c_str(), win);
        // 从启动器挑出来的卡带读不动（多半是文件被删了/被换了），退回列表重挑一个就好，
        // 没必要把整个程序带走。此时还没分配任何本回合资源，直接重来最省事。
        if (fromLauncher) continue;
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
    // 读档是一次性的，用完就划掉：回到游戏列表后可能换一张卡带，
    // 留着旧档路径只会拿别人的档去读一个新 ROM（读出来必然牛头不对马嘴）。
    cfg.loadState.clear();

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
    // net 在外层循环里反复复用：几个 start*() 内部都会 resetStreamState()，
    // stop() 之后可以原地再开一局，不必重建对象。
    opt.stateFile = cfg.rom + ".fcstate";

    if (cfg.net != LaunchConfig::Net::Solo) {
        std::string err;
        bool ok = false;
        // 带上卡带指纹：握手完成后两端互发校验，选了不同的卡带当场报错，
        // 不会拖到开局后冒出一句没头没脑的「检测到状态不同步」。
        net.setLocalRomHash(emu.romHash());
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
            // 注意：这里**不要**把 cfg.net 改回 Solo。以前它会改，后果是从启动器
            // 「返回游戏列表」再进来时（启动器退出会落盘），用户配好的主机/客机/中继
            // 设置会被无声地降级成单机 —— 下次开房还得重配一遍。
            // 内存里"这一局跑单机"这件事，用下面的 net.active() 判断就够了。
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
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            if (SDL_GameController* gc = SDL_GameControllerOpen(i)) {
                pads.push_back(gc);
                std::printf("手柄    : %s\n", SDL_GameControllerName(gc));
            }
        }
    }

    // ------------------------------------------------------------ 主循环
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
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

    // 联机中「把本机当下的状态同步给对方」时附带的说明：完成后提示语里的动词。
    // 为空 = 普通读档；非空则提示语按这个动作来写（例如「重启卡带」）。
    std::string syncNote;

    // 联机中读档 = 「把这份档发给对方，双方一起装上」；单机 = 本地装上就完事。
    // 菜单项和 F8 都走这里，免得两条路行为不一致。
    auto loadOrSyncState = [&](const char* src) {
        auto toast = [&](const char* t, bool warn, double ms) {
            menuToast      = t;
            menuToastWarn  = warn;
            menuToastUntil = double(SDL_GetTicks()) + ms;
        };

        std::vector<u8> buf;
        if (!readFile(opt.stateFile, buf)) {
            std::printf("[读档] 打不开 %s（先按 F5 存一个）\n", opt.stateFile.c_str());
            toast("没有可用的存档", true, 2400.0);
            return;
        }
        if (!net.active()) {
            std::string why;
            if (emu.loadState(buf.data(), buf.size(), &why)) {
                std::printf("[读档] 成功（第 %llu 帧）\n", (unsigned long long)emu.frameCount());
                toast("已读取存档", false, 2200.0);
            } else {
                std::printf("[读档] 失败：%s\n", why.empty() ? "格式不对" : why.c_str());
                toast(why.empty() ? "读档失败" : "读档失败（档与当前 ROM 不匹配）", true, 2600.0);
            }
            return;
        }

        // 联机：先自检一遍。装不上的档发过去只会把两端一起搞坏 ——
        // 对方装上了、本机没装上，那不叫同步，叫分裂。
        std::string why;
        if (!emu.checkState(buf.data(), buf.size(), &why)) {
            std::printf("[读档] 联机同步前自检失败：%s\n", why.c_str());
            toast("这份档与当前 ROM 不匹配，没法同步", true, 3000.0);
            return;
        }
        net.beginStateSync(buf);
        if (net.stateSyncing()) {
            std::printf("[读档] 联机中（%s）：%zu 字节开始同步给对方\n", src, buf.size());
            std::fflush(stdout);
            toast("正在把存档同步给对方…", false, 4000.0);
        } else {
            toast("同步没能启动", true, 2600.0);
        }
    };

    // 离开当前进度前的自动存档（暂停菜单里「重启卡带 / 换一张卡带」共用）。
    // 存档 = 把当前状态读出来写盘，不改变模拟状态，所以永远安全。
    // 开关交给用户：不是所有人都想要一堆意外生成的 .fcstate。
    auto autoSaveBeforeLeave = [&](const char* where) -> bool {
        if (!cfg.autoSave) return false;
        std::vector<u8> buf;
        emu.saveState(buf);
        if (writeFile(opt.stateFile, buf)) {
            std::printf("[存档] %s前已自动存档: %zu 字节 -> %s\n", where, buf.size(),
                        opt.stateFile.c_str());
            return true;
        }
        std::fprintf(stderr, "[存档] %s前自动存档失败: %s\n", where, opt.stateFile.c_str());
        return false;
    };

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

    // ------------------------------------------------ 重启卡带（单机 / 联机）
    // 等价于按主机的 Reset：多合一卡带（68 in 1 之类）开机先给自己的菜单，
    // Reset 就又回到那个菜单 —— 想玩同一张卡带里的另一个游戏走这条。
    // 联机里不能自己一个人跳，所以做法是「先造出快照、再退回去」：
    //   ① 存下当前状态  ② reset 出「刚开机」的快照  ③ 立刻把当前状态装回来
    // 于是本机在整段传输期间都停在原地，等对方收齐后由装档流程把两端一起送过去。
    // 这样同步万一失败，两端本来就都没动过，不会留下一次不可逆的不同步。
    // 菜单项与开发者钩子 --reset-cart-at 共用，免得两条路行为不一致。
    auto resetCart = [&](const char* src) {
        if (!net.active()) {
            emu.reset();
            setMenu(false);
            std::printf("[卡带] 已重启，回到卡带自带的菜单\n");
            return;
        }
        if (net.stateSyncing()) {
            menuToast      = "正在同步中，请稍后再试";
            menuToastWarn  = true;
            menuToastUntil = double(SDL_GetTicks()) + 2400.0;
            return;
        }
        std::vector<u8> rollback;
        emu.saveState(rollback);
        emu.reset();
        std::vector<u8> fresh;
        emu.saveState(fresh);
        std::string rbWhy;
        if (!emu.loadState(rollback.data(), rollback.size(), &rbWhy)) {
            std::fprintf(stderr, "[卡带] 回退到原状态失败：%s\n", rbWhy.c_str());
            menuToast      = "重启卡带失败（本机状态回退不了）";
            menuToastWarn  = true;
            menuToastUntil = double(SDL_GetTicks()) + 3000.0;
            return;
        }
        syncNote = "重启卡带";
        net.beginStateSync(fresh);
        if (net.stateSyncing()) {
            setMenu(false);
            std::printf("[卡带] 联机中重启（%s）：把「刚开机」这份状态同步给对方，"
                        "两端一起回到卡带菜单\n", src);
            std::fflush(stdout);
            menuToast      = "正在同步重启卡带…";
            menuToastWarn  = false;
            menuToastUntil = double(SDL_GetTicks()) + 4000.0;
        } else {
            syncNote.clear();
            menuToast      = "同步没能启动，卡带未重启";
            menuToastWarn  = true;
            menuToastUntil = double(SDL_GetTicks()) + 3000.0;
        }
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
                        // 联机里不再是「不能读档」，而是「读档 = 把这份档同步给对方」。
                        // 理由见 net.h 顶部那张时序图：两端必须是同一份档才行。
                        loadOrSyncState("F8");
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

        // ------------------------------------------------ 开发者自测：往暂停菜单里注入点击
        // 菜单项几乎都只在「点下去那一帧」产生动作，正常路径下没法无人值守地验证。
        // 这里按帧号来：先替用户把菜单按开，隔两帧再补一次合成按下。
        // 拉开这几帧是必要的 —— 只有等界面真的画出来、行矩形算好，点击才可能命中。
        if (opt.menuClickX >= 0.0f && !menuClickFired) {
            if (++menuClickFrame == 1) {
                setMenu(true);
            } else if (menuClickFrame >= 4) {
                in.pressed = true;
                in.down    = true;
                in.mx      = opt.menuClickX;
                in.my      = opt.menuClickY;
                menuClickFired = true;
            }
        }

        // ------------------------------------------------ 开发者自测：联机中自动读档同步
        // 存档同步这条路径没法靠手点验证（要两台机器同时操作），所以留个按帧触发的口子。
        if (!opt.syncState.empty() && !syncFired && net.active() && ++syncFrame >= opt.syncAt) {
            syncFired = true;
            std::printf("[自测] 第 %d 帧触发联机存档同步: %s\n", syncFrame, opt.syncState.c_str());
            std::fflush(stdout);
            std::vector<u8> sbuf;
            if (readFile(opt.syncState, sbuf)) {
                std::string swhy;
                if (!emu.checkState(sbuf.data(), sbuf.size(), &swhy)) {
                    std::fprintf(stderr, "[自测] 这份档本机都装不上（%s），同步取消\n", swhy.c_str());
                } else {
                    net.beginStateSync(sbuf);
                }
            } else {
                std::fprintf(stderr, "[自测] 读不到 %s\n", opt.syncState.c_str());
            }
        }

        // ------------------------------------------------ 开发者自测：联机中自动重启卡带
        // 「联机里两端一起回到卡带菜单」这条路径同样没法靠手点验证，留个按轮次触发的口子。
        if (opt.resetCartAt > 0 && !resetFired && net.active() && ++resetFrame >= opt.resetCartAt) {
            resetFired = true;
            std::printf("[自测] 第 %d 轮触发联机重启卡带\n", resetFrame);
            std::fflush(stdout);
            resetCart("自测");
        }

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
        // 收发存档期间两端都必须停：谁先跑起来，谁的状态就和对方不一样了。
        bool simulated = false;
        bool waiting   = false;
        if (!paused && !net.stateSyncing()) {
            if (net.active()) {
                // 锁步：等待远端延迟帧输入后再跑当前帧
                const u32 simFrame = net.beginFrame(localPad);
                if (net.disconnected()) {
                    // 对端断开也分两种原因：普通退出（本机切回单机接着玩），
                    // 和「对方去换卡带了」（本机也回启动器，各自选好再重新连）。
                    const bool peerToLib = net.peerWentToLibrary();
                    std::fprintf(stderr, "\n[联机] 与 %s 的连接已断开%s\n",
                                 net.peer().c_str(),
                                 peerToLib ? "（对方去换卡带了）" : "");
                    std::fflush(stderr);
                    net.stop();
                    if (peerToLib) {
                        handoff.toast     = "对方去换卡带了；各自选好卡带后重新开始联机";
                        handoff.toastWarn = false;
                        toLibrary = true;     // 跟着回启动器，别让人家对着单机画面发呆
                        running   = false;
                    } else {
                        netBroken = true;
                    }
                } else if (simFrame == NetSession::NO_FRAME) {
                    // 对端还没到这一帧：本机先等，但界面照常刷新（菜单也还能点）。
                    // 注意同步期间不算「等对端」—— 那是我们自己停了，得照常走下面的节拍。
                    waiting = !net.stateSyncing();
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

        // ------------------------------------------------ 联机中读档：推进存档同步
        // 收发期间不推进模拟，只推进这套握手。界面照常刷新，好让用户看见进度。
        {
            const bool wasSyncing = stateSyncing;
            if (net.stateSyncing()) {
                std::vector<u8> applyBuf;
                if (net.pumpStateSync(applyBuf)) {
                    std::string why;
                    if (emu.loadState(applyBuf.data(), applyBuf.size(), &why)) {
                        std::printf("[同步] 已装上同步过来的存档（第 %llu 帧）\n",
                                    (unsigned long long)emu.frameCount());
                        if (syncNote.empty()) {
                            menuToast = net.stateSending() ? "存档已同步给对方，继续游戏"
                                                           : "已载入对方同步的存档";
                        } else if (net.stateSending()) {
                            menuToast = "已" + syncNote + "，并同步给对方";
                        } else {
                            menuToast = "对方" + syncNote + "，已同步过来";
                        }
                        menuToastWarn  = false;
                    } else {
                        std::printf("[同步] 同步过来的存档装不上：%s\n", why.c_str());
                        menuToast      = "同步过来的存档装不上";
                        menuToastWarn  = true;
                    }
                    menuToastUntil = double(SDL_GetTicks()) + 3000.0;
                    net.endStateSync();
                    syncNote.clear();
                }
            } else if (wasSyncing) {
                // 刚刚结束（成功的那条路上面已经处理过，stateError 为空）
                if (!net.stateError().empty()) {
                    menuToast      = syncNote.empty()
                                        ? ("存档同步失败：" + net.stateError())
                                        : (syncNote + "同步失败：" + net.stateError()
                                           + "；两端维持原状");
                    menuToastWarn  = true;
                    menuToastUntil = double(SDL_GetTicks()) + 4200.0;
                }
                syncNote.clear();
            }
            stateSyncing = net.stateSyncing();
        }

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
                pi.autoSave   = cfg.autoSave;
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
                    case PauseAction::LoadState:
                        loadOrSyncState("菜单");
                        break;
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
                    case PauseAction::ResetCart:
                        // 换进度前先存档（受设置页「离开时自动存档」控制，不影响模拟状态）
                        autoSaveBeforeLeave("重启卡带");
                        resetCart("菜单");
                        break;
                    case PauseAction::ToLibrary: {
                        // 换卡带 = 放弃这一局的现场。
                        const bool saved = autoSaveBeforeLeave("换卡带");
                        if (net.active()) {
                            // 联机换卡带：没有「两端一起换」的协议（对端机器上未必有这份
                            // ROM），所以干脆干净断开 —— 对端收到「我去换卡带了」的原因后
                            // 也会跟着回启动器，两边各自挑好卡带再重新开始联机。
                            // 注意不要拨「启动时载入存档」：那会让本机开局带档、对方不带，
                            // 重连后必然不同步。存档已写盘，下次单机玩可以自己 F8 读。
                            net.stop(true);
                            handoff.toast     = saved
                                ? "已断开联机并自动存档；回到列表换卡带，选好后重新开始联机"
                                : "已断开联机；回到列表换卡带，选好后重新开始联机";
                            handoff.toastWarn = false;
                        } else if (saved) {
                            // 顺手把「启动时载入存档」拨到刚存下的这个档上：回到列表后如果又选了
                            // 同一张卡带，期望显然是接着刚才那一局玩，而不是从开机画面重新来。
                            // 换别的卡带时 pickRom 会自己把这个开关清掉，所以不会串档。
                            cfg.loadState     = opt.stateFile;
                            handoff.toast     = "已自动存档，再选同一张卡带可接着玩";
                            handoff.toastWarn = false;
                        } else {
                            handoff.toast     = cfg.autoSave ? "自动存档失败，已直接返回列表"
                                                             : "已返回列表（没自动存档）";
                            handoff.toastWarn = cfg.autoSave;
                        }
                        toLibrary = true;     // 本回合到此为止，收尾后重新回启动器
                        running   = false;
                        break;
                    }
                    case PauseAction::Quit:
                        running = false;
                        break;
                    default:
                        break;
                }
            }
            if (net.stateSyncing()) drawStateSyncOverlay(ui, net);
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
        // 终止条件用「模拟器自己的帧号」，而不是本机跑了多少帧：
        // 联机回归要拿两端的状态哈希逐位比对，只有停在同一个绝对帧上才有可比性。
        // （存档同步会让帧号回退到档里的那一刻，两端在同步前各跑了多少帧是不相等的，
        //   按本机计数退出必然差个一两帧。）
        if (opt.fpsLog > 0 && simulated) {
            ++fpsLogDone;
            if (emu.frameCount() < u64(opt.fpsLog)) continue;
            const double sec = double(SDL_GetPerformanceCounter() - fpsLogT0) / double(perfFreq);
            std::printf("实测帧率 : %d 帧 / %.3f 秒 = %.2f FPS（%s）\n", fpsLogDone, sec,
                        sec > 0 ? double(fpsLogDone) / sec : 0.0,
                        net.active() ? "联机" : "单机");
            if (!opt.saveStateOut.empty()) {
                std::vector<u8> sbuf;
                emu.saveState(sbuf);
                if (writeFile(opt.saveStateOut, sbuf))
                    std::printf("[自测] 已写出存档: %zu 字节 -> %s\n", sbuf.size(),
                                opt.saveStateOut.c_str());
                else
                    std::fprintf(stderr, "[自测] 写存档失败: %s\n", opt.saveStateOut.c_str());
            }
            // 收尾状态哈希：联机回归就靠比对这个数 —— 两端一致才说明锁步真的没错位。
            std::printf("状态哈希 : %016llX（第 %llu 帧，%s）\n",
                        (unsigned long long)emu.stateHash(),
                        (unsigned long long)emu.frameCount(),
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

        // ------------------------------------------------ 本回合收尾
        // 不管是从「退出游戏」还是「返回游戏列表」出来的，这些都要放干净：
        // 下一圈会重新载入 ROM、重开音频设备、重建纹理，留着就是两套同时活着。
        net.stop();
        for (SDL_GameController* gc : pads) SDL_GameControllerClose(gc);
        pads.clear();
        if (adev) { SDL_CloseAudioDevice(adev); adev = 0; }
        if (tex)  { SDL_DestroyTexture(tex);   tex  = nullptr; }

        if (!toLibrary) break;

        // 回列表前先收掉全屏：启动器是给普通窗口排的版，全屏下拉开的观感很怪；
        // 而且不撤标志的话，下一圈里 fullscreen 这个内存状态会和窗口实际状态对不上。
        SDL_SetWindowFullscreen(win, 0);
        std::printf("返回游戏列表。\n");
    }

    ui.shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    std::printf("已退出。\n");
    return 0;
}
