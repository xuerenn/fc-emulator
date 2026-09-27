// headless.cpp — 无图形验证程序：跑 N 帧、打印状态、导出 PPM 截图、可选联机自测
// 用于在没有显示器/不需要 SDL 的环境下验证 CPU / PPU / APU / Mapper / 网络同步全链路。
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

#include "core/emulator.h"
#include "core/fs_utf8.h"
#include "net/net.h"

#ifdef _WIN32
#  include <windows.h>      // SetConsoleOutputCP：修 Windows 控制台中文乱码
#endif

using namespace fc;

namespace {

bool writePPM(const char* path, const u32* fb, int w, int h) {
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

bool readFile(const std::string& path, std::vector<u8>& out) {
    std::FILE* f = fc::fopenUtf8(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size_t(n > 0 ? n : 0));
    if (n > 0) std::fread(out.data(), 1, size_t(n), f);
    std::fclose(f);
    return n > 0;
}

void usage() {
    std::printf(
        "用法: fc-headless <rom.nes> [帧数] [输出.ppm] [选项]\n"
        "  --pad <掩码>                每帧固定按下的按键位掩码（十六进制，如 0x08=Start）\n"
        "  --pad-at <帧>:<掩码>        从指定帧起改用该按键掩码，可重复多次以模拟按键序列\n"
        "  --net-host <端口>           作为主机联机（等待客机），本机固定坐 1P\n"
        "  --net-connect <ip[:端口]>   作为客机联机，本机固定坐 2P\n"
        "                              （联机时 --pad 代表本机玩家的输入，对端输入自动填另一路）\n"
        "  --relay <ip[:端口]>         改用公网中继（双方都不需要公网 IP），需与 --room 搭配\n"
        "  --room <房间号>             中继房间号，两端必须一致（默认 7777）\n"
        "  --delay <帧数>              输入延迟帧数，默认 3（中继模式默认 6）\n"
        "  --hash-at <帧号>            在指定网络帧打印状态哈希，可重复多次；\n"
        "                              联机时两端都带上，用同一帧的哈希对齐比对\n"
        "  --hash-every <n>            每 n 帧打印一次状态哈希（联机时两端都带上，\n"
        "                              日志逐行 diff 即可定位从哪一帧开始分叉）\n"
        "  --dump-ppu                  导出 PPU 显存（nametable/属性表/调色板/瓦片图案）\n"
        "  --trace-ppu                 追踪 $2006/$2007，显示游戏实际写了哪些显存格子\n"
        "\n"
        "联机自测专用：\n"
        "  --perturb-at <帧>:<地址>:<值>  到该网络帧时往总线写一个字节，可重复多次。\n"
        "                              只在其中一端指定，就能人为制造状态分叉，\n"
        "                              用来确认「不同步检测」本身没坏（会报出首帧与两端哈希）。\n");
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);   // 源文件中文是 UTF-8，控制台默认 GBK 会乱码
    // 路径在程序内部一律按 UTF-8 走，而 Windows 的 argv 是系统 ANSI 代码页（中文版
    // 为 GBK）：先转一次，中文路径的 ROM / 存档才能和 fopen 那边对齐。
    std::vector<std::string> argvUtf8;
    argvUtf8.reserve(size_t(argc));
    for (int i = 0; i < argc; ++i) argvUtf8.push_back(fc::ansiToUtf8(argv[i] ? argv[i] : ""));
    for (int i = 0; i < argc; ++i) argv[i] = const_cast<char*>(argvUtf8[size_t(i)].c_str());
#endif
    if (argc < 2) { usage(); return 1; }

    const std::string rom = argv[1];
    int frames = 60;
    const char* outPath = nullptr;
    u8 padMask = 0;
    bool netHost = false, netConnect = false;
    u16 hostPort = 7777, connPort = 7777;
    std::string connIp = "127.0.0.1";
    int delay = 3;
    bool delaySet = false;
    bool relay = false;
    std::string relayIp = "127.0.0.1";
    u16 relayPort = 7777;
    std::string room = "7777";
    bool dumpPPU = false;
    bool tracePPU = false;
    std::string loadState;                       // 启动时载入的存档（空=不载入）
    std::vector<u32> hashAt;                     // 需要在哪些网络帧打印状态哈希
    int hashEvery = 0;                           // 每 N 帧打印一次哈希（0=关闭）
    // 联机自测用的「人造分叉」：到某帧时往总线写一个字节（只在一端指定才会分叉）
    struct Perturb { u32 frame; u16 addr; u8 val; };
    std::vector<Perturb> perturb;
    std::vector<std::pair<int, u8>> padSeq;      // (起始帧, 按键掩码)，按帧号生效

    int positional = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        // 只在下一个参数不是选项时才消费它：这样 `--net-host --relay 1.2.3.4:7777`
        // 里的端口可以省略，而不会被 "--relay" 顶掉。
        auto argOr = [&](const char* def) -> std::string {
            if (i + 1 < argc && argv[i + 1][0] != '-') return std::string(argv[++i]);
            return std::string(def);
        };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--pad")         padMask = (u8)std::strtol(argOr("0").c_str(), nullptr, 0);
        else if (a == "--pad-at") {
            const std::string v = argOr("0:0");
            const size_t c = v.find(':');
            if (c != std::string::npos)
                padSeq.push_back({ std::atoi(v.substr(0, c).c_str()),
                                   (u8)std::strtol(v.substr(c + 1).c_str(), nullptr, 0) });
        }
        else if (a == "--net-host")    { netHost = true; hostPort = u16(std::atoi(argOr("7777").c_str())); }
        else if (a == "--net-connect") {
            netConnect = true;
            const std::string v = argOr("127.0.0.1");
            const size_t c = v.find(':');
            if (c != std::string::npos) { connIp = v.substr(0, c); connPort = u16(std::atoi(v.substr(c + 1).c_str())); }
            else if (!v.empty()) connIp = v;
        }
        else if (a == "--relay") {
            relay = true;
            const std::string v = argOr("");
            const size_t c = v.find(':');
            if (c != std::string::npos) {
                relayIp = v.substr(0, c);
                relayPort = u16(std::atoi(v.substr(c + 1).c_str()));
            } else if (!v.empty()) {
                relayIp = v;
            }
            if (relayIp.empty()) relayIp = "127.0.0.1";
        }
        else if (a == "--room")        { room = argOr("7777"); if (room.empty()) room = "7777"; }
        else if (a == "--delay")       { delay = std::atoi(argOr("3").c_str()); delaySet = true; }
        else if (a == "--hash-at")     hashAt.push_back(u32(std::atoi(argOr("0").c_str())));
        else if (a == "--hash-every")  hashEvery = std::atoi(argOr("0").c_str());
        else if (a == "--perturb-at") {
            const std::string v = argOr("0:0:0");
            long parts[3] = { 0, 0, 0 };
            size_t start = 0;
            for (int k = 0; k < 3; ++k) {
                const size_t c = v.find(':', start);
                const std::string t = (c == std::string::npos)
                                        ? v.substr(start) : v.substr(start, c - start);
                if (!t.empty()) parts[k] = std::strtol(t.c_str(), nullptr, 0);
                if (c == std::string::npos) break;
                start = c + 1;
            }
            perturb.push_back(Perturb{ u32(parts[0]), u16(parts[1]), u8(parts[2]) });
        }
        else if (a == "--dump-ppu")    dumpPPU = true;
        else if (a == "--trace-ppu")   tracePPU = true;
        else if (a == "--load-state")  loadState = argOr("");
        else if (!a.empty() && a[0] != '-') {
            if (positional == 0) { frames = std::atoi(a.c_str()); positional++; }
            else if (positional == 1) { outPath = argv[i]; positional++; }
        }
    }

    if (relay && !netHost && !netConnect) {
        std::fprintf(stderr, "错误: 使用 --relay 时必须指定 --net-host（坐 1P）或 --net-connect（坐 2P）\n");
        return 1;
    }

    Emulator emu;
    if (!emu.loadROM(rom)) {
        std::fprintf(stderr, "ROM 加载失败: %s\n", rom.c_str());
        return 2;
    }

    std::printf("ROM      : %s\n", emu.cartridge().title().c_str());
    std::printf("Mapper   : %s\n", emu.cartridge().mapperName());
    std::printf("镜像模式 : %d\n", emu.cartridge().mirrorMode());
    std::printf("ROM 指纹 : %016llX\n", (unsigned long long)emu.romHash());
    if (!loadState.empty()) {
        std::vector<u8> buf;
        std::string why;
        if (!readFile(loadState, buf)) {
            std::fprintf(stderr, "读档失败: 打不开 %s\n", loadState.c_str());
            return 3;
        }
        if (!emu.loadState(buf.data(), buf.size(), &why)) {
            std::fprintf(stderr, "读档失败: %s —— %s\n", loadState.c_str(), why.c_str());
            return 3;
        }
        std::printf("读档     : %s（从第 %llu 帧继续）\n",
                    loadState.c_str(), (unsigned long long)emu.frameCount());
    }
    std::printf("目标帧数 : %d\n", frames);
    if (tracePPU) emu.bus().ppu().debugTrace(true, 400000);
    std::fflush(stdout);

    // ------------------------------------------------------------ 联机接入
    NetSession net;
    if (netHost || netConnect) {
        std::string err;
        if (relay && !delaySet) delay = kRelayDefaultDelay;   // 中继多一跳，默认给足
        bool ok;
        if (relay) {
            RelayConfig rc;
            rc.host = relayIp;
            rc.port = relayPort;
            rc.room = room;
            ok = netHost ? net.startHostViaRelay(rc, delay, &err)
                         : net.startClientViaRelay(rc, delay, &err);
        } else {
            ok = netHost ? net.startHost(hostPort, delay, &err)
                         : net.startClient(connIp, connPort, delay, &err);
        }
        if (!ok) { std::fprintf(stderr, "联机失败: %s\n", err.c_str()); return 3; }
        if (net.relayed()) {
            std::printf("联机     : %s <- 中继 %s:%u（房间 %s），延迟 %d 帧，本机坐 %s\n",
                        netRoleName(net.role()), net.relayConfig().host.c_str(),
                        net.relayConfig().port, net.relayConfig().room.c_str(),
                        net.inputDelay(),
                        net.role() == NetSession::Role::Host ? "1P" : "2P");
        } else {
            std::printf("联机     : %s <- %s，延迟 %d 帧，本机坐 %s\n",
                        netRoleName(net.role()), net.peer().c_str(), net.inputDelay(),
                        net.role() == NetSession::Role::Host ? "1P" : "2P");
        }
        std::fflush(stdout);
    }

    // 按帧号求解当前应施加的按键（未配置序列时就是固定的 --pad 值）
    auto currentPad = [&](int frame) -> u8 {
        u8 m = padMask;
        for (const auto& k : padSeq) if (frame >= k.first) m = k.second;
        return m;
    };

    int simulated = 0;
    int guard = 0;
    bool peerLeft = false;
    auto wantHash = [&](u32 f) {
        if (hashEvery > 0 && f % u32(hashEvery) == 0) return true;
        return std::find(hashAt.begin(), hashAt.end(), f) != hashAt.end();
    };

    while (simulated < frames && guard < frames * 600 + 60000) {
        ++guard;
        const u8 pad = currentPad(simulated);
        if (net.active()) {
            const u32 f = net.beginFrame(pad);
            if (net.disconnected()) {
                peerLeft = true;
                break;
            }
            if (f == NetSession::NO_FRAME) continue;
            // 座位固定：host 坐 1P、client 坐 2P（与 GUI 前端保持一致）
            const u8 mine   = net.localInput(f);
            const u8 theirs = net.remoteInput(f);
            if (net.role() == NetSession::Role::Host) {
                emu.bus().player1().setButtons(mine);
                emu.bus().player2().setButtons(theirs);
            } else {
                emu.bus().player1().setButtons(theirs);
                emu.bus().player2().setButtons(mine);
            }
            // 自测钩子：只在指定端生效，制造出确定的状态分叉
            for (const Perturb& pt : perturb)
                if (pt.frame == f) emu.bus().write(pt.addr, pt.val);
            emu.runFrame();
            simulated++;
            if (f % 120 == 0) net.sendHash(f, emu.stateHash());
            // 两端都在同一帧打印，就能拿日志逐帧对齐比对
            if (wantHash(f))
                std::printf("哈希@帧 %u : %016llX\n", f, (unsigned long long)emu.stateHash());
        } else {
            emu.bus().player1().setButtons(pad);
            emu.runFrame();
            simulated++;
            // 单机的第 k 次 runFrame 对应联机的网络帧 k-1，编号方式保持一致
            const u32 f = u32(simulated - 1);
            if (wantHash(f))
                std::printf("哈希@帧 %u : %016llX\n", f, (unsigned long long)emu.stateHash());
        }
    }

    // stop() 会重置联机状态，先把要汇报的信息取出来
    const bool   netWasActive = net.active();
    const bool   netDesync    = net.desynced();
    const u32    netDesyncCnt = net.desyncCount();
    NetSession::DesyncInfo netFirst;
    const bool   netHasFirst  = net.takeDesyncEvent(netFirst);
    const bool   netRelayed   = net.relayed();
    const std::string netRoom = net.relayConfig().room;
    net.stop();
    if (tracePPU) emu.bus().ppu().debugTrace(false);        // 停表，保留日志
    if (peerLeft)
        std::fprintf(stderr, "对端已离开，本机提前结束（已模拟 %d/%d 帧）\n", simulated, frames);

    // ------------------------------------------------------------ 统计
    const u32* fb = emu.framebuffer();
    std::vector<u32> colors;
    for (int i = 0; i < kScreenPixels; ++i) {
        const u32 c = fb[i] & 0x00FFFFFFu;
        bool seen = false;
        for (u32 v : colors) if (v == c) { seen = true; break; }
        if (!seen && colors.size() < 64) colors.push_back(c);
    }

    std::printf("已模拟   : %d 帧\n", simulated);
    if (netWasActive) {
        if (netRelayed)
            std::printf("联机方式 : 中继（房间 %s）\n", netRoom.c_str());
        else
            std::printf("联机方式 : 直连\n");
        // 哈希比对结论：每 120 帧交换一次状态哈希，任何一帧对不上都会记录
        if (!netDesync) {
            std::printf("状态同步 : 一致（哈希比对无差异）\n");
        } else if (netHasFirst) {
            std::printf("状态同步 : 检测到不同步(!!)  首帧 %u  本机 %016llX  对端 %016llX  累计 %u 次\n",
                        netFirst.frame, (unsigned long long)netFirst.local,
                        (unsigned long long)netFirst.remote, netDesyncCnt);
        } else {
            std::printf("状态同步 : 检测到不同步(!!)  累计 %u 次\n", netDesyncCnt);
        }
    }
    std::printf("CPU 周期 : %llu\n", (unsigned long long)emu.bus().cpu().totalCycles());
    std::printf("PPU 位置 : scanline=%d dot=%d\n", emu.bus().ppu().scanline(), emu.bus().ppu().dot());
    std::printf("CPU PC   : $%04X\n", emu.bus().cpu().pc());
    // 末帧实际施加到两个手柄的按键：联机时应为「本机玩家 → 自己的座位」
    std::printf("P1 按钮  : 0x%02X\n", emu.bus().player1().buttons());
    std::printf("P2 按钮  : 0x%02X\n", emu.bus().player2().buttons());
    std::printf("画面颜色 : %zu 种\n", colors.size());
    for (size_t i = 0; i < colors.size() && i < 8; ++i)
        std::printf("   #%02zu -> $%06X\n", i, colors[i]);

    // 状态快照 存/读 往返一致性（回滚能力的核心前提）
    std::vector<u8> snap;
    emu.saveState(snap);
    const u64 h1 = emu.stateHash();
    emu.runFrame();
    const u64 h2 = emu.stateHash();
    const bool restored = emu.loadState(snap.data(), snap.size());
    const u64 h3 = emu.stateHash();
    std::printf("快照字节 : %zu\n", snap.size());
    std::printf("回滚校验 : 前进后 %s，回滚后 %s\n",
                (h1 != h2 ? "哈希已变化(OK)" : "哈希未变(!!)"),
                (restored && h1 == h3 ? "完全还原(OK)" : "还原失败(!!)"));
    std::printf("最终哈希 : %016llX\n", (unsigned long long)h3);

    // ------------------------------------------------------------ 显存导出
    if (dumpPPU) {
        const PPU& ppu = emu.bus().ppu();
        const u8* nt = ppu.debugNametable();
        const int ntSize = ppu.debugNametableSize();
        std::printf("--- PPUCTRL=$%02X PPUMASK=$%02X 镜像=%d ---\n",
                    ppu.debugCtrl(), ppu.debugMask(), emu.cartridge().mirrorMode());

        for (int tbl = 0; tbl < 2; ++tbl) {
            const int base = tbl * 0x400;
            if (base + 0x400 > ntSize) break;
            std::printf("=== Nametable %d: 瓦片编号 (每行 32 列) ===\n", tbl);
            std::printf("     %s\n", "0123456789ABCDEF0123456789ABCDEF");
            for (int r = 0; r < 30; ++r) {
                std::printf("%2d | ", r);
                for (int c = 0; c < 32; ++c) std::printf("%02X", nt[base + r * 32 + c]);
                std::printf("\n");
            }
            std::printf("=== Nametable %d: 属性表 + 第30/31行 ===\n", tbl);
            for (int r = 30; r < 32; ++r) {
                std::printf("%2d | ", r);
                for (int c = 0; c < 32; ++c) std::printf("%02X", nt[base + r * 32 + c]);
                std::printf("\n");
            }
        }
        // 直接打印若干瓦片在「当前分页生效后」的实际图案，用于判断 CHR 是否读错 bank
        const int bgTable = (ppu.debugCtrl() & 0x10) ? 0x1000 : 0x0000;
        const u8 probe[] = {0x00, 0x01, 0x55, 0xFC, 0xFD, 0xFE, 0xFF, 0x06, 0x08, 0x12, 0x17};
        std::printf("=== 瓦片图案（BG 图案表 $%04X，经 Mapper 分页后） ===\n", bgTable);
        for (u8 t : probe) {
            const u16 a = u16(bgTable + t * 16);
            std::printf("  瓦片 $%02X:", t);
            for (int y = 0; y < 8; ++y) {
                const u8 lo = emu.cartridge().readCHR(u16(a + y));
                const u8 hi = emu.cartridge().readCHR(u16(a + y + 8));
                std::printf(y == 0 ? " " : "\n         ");
                for (int x = 0; x < 8; ++x) {
                    const int bit = 7 - x;
                    const int px = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                    std::printf("%c", px == 0 ? '.' : (px == 1 ? '+' : (px == 2 ? '*' : '#')));
                }
            }
            std::printf("\n");
        }
        std::fflush(stdout);

        std::printf("=== 调色板 ===\n");
        for (int p = 0; p < 8; ++p) {
            std::printf("  BG%02d: ", p);
            for (int k = 0; k < 4; ++k) std::printf("%02X ", ppu.debugPalette()[p * 4 + k]);
            std::printf("   SPR%02d: ", p);
            for (int k = 0; k < 4; ++k) std::printf("%02X ", ppu.debugPalette()[0x10 + p * 4 + k]);
            std::printf("\n");
        }
        std::fflush(stdout);
    }

    // ------------------------------------------------------------ 显存写入追踪分析
    if (tracePPU) {
        const auto& log = emu.bus().ppu().debugTraceLog();
        size_t dataWrites = 0, addrSets = 0, paletteWrites = 0;
        size_t wroteC = 0, wroteD = 0;
        static bool wroteA[0x400];
        static bool wroteB[0x400];
        for (int i = 0; i < 0x400; ++i) { wroteA[i] = false; wroteB[i] = false; }
        for (const auto& e : log) {
            if (!e.isData) { ++addrSets; continue; }
            ++dataWrites;
            if (e.addr >= 0x2000 && e.addr < 0x2400)      wroteA[e.addr & 0x3FF] = true;
            else if (e.addr >= 0x2400 && e.addr < 0x2800) wroteB[e.addr & 0x3FF] = true;
            else if (e.addr >= 0x2800 && e.addr < 0x2C00) ++wroteC;
            else if (e.addr >= 0x2C00 && e.addr < 0x3000) ++wroteD;
            else if (e.addr >= 0x3F00)                    ++paletteWrites;
        }
        int cntA = 0, cntB = 0;
        for (int i = 0; i < 0x400; ++i) { if (wroteA[i]) ++cntA; if (wroteB[i]) ++cntB; }

        std::printf("=== PPU 写入追踪 ===\n");
        std::printf("日志 %zu 条（数据写 %zu / 地址设置 %zu / 调色板 %zu）\n",
                    log.size(), dataWrites, addrSets, paletteWrites);
        std::printf("--- $2000-$23FF 哪些格子被写过（#=写过 .=没写）---\n");
        for (int r = 0; r < 32; ++r) {
            std::printf("%2d |", r);
            for (int c = 0; c < 32; ++c) std::printf("%c", wroteA[r * 32 + c] ? '#' : '.');
            std::printf("\n");
        }
        std::printf("--- 覆盖统计：$2000区 %d/1024   $2400区 %d/1024   $2800区 %zu   $2C00区 %zu ---\n",
                    cntA, cntB, wroteC, wroteD);
        std::printf("--- 最早 40 条 ---\n");
        for (size_t i = 0; i < log.size() && i < 40; ++i)
            std::printf("  %s $%04X <- $%02X\n", log[i].isData ? "DATA" : "ADDR", log[i].addr, log[i].val);
        std::fflush(stdout);
    }

    if (outPath) {
        if (writePPM(outPath, fb, kScreenWidth, kScreenHeight))
            std::printf("截图     : %s\n", outPath);
        else
            std::fprintf(stderr, "写截图失败: %s\n", outPath);
    }
    return 0;
}
