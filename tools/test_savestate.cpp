// test_savestate.cpp — 存档/读档往返一致性自测（开发者工具，不随模拟器分发）
//
// 要验证的只有一件事：F5 存下来的档，**换一个进程**用 F8 读回来之后，
// 模拟能不能沿完全相同的轨迹继续跑 —— 也就是状态快照是否「完整」。
//
// 只测「读档不报错」是不够的：漏存一个字段（比如 PPU 的 loopy 寄存器 v/t/x、
// 或 MMC1 的移位寄存器、CPU 的中断挂起位）照样不报错，但画面/时序从此就偏了。
// 所以判据必须是「继续跑 N 帧后的状态哈希逐位相同」。
//
// 用法:
//   test_savestate <rom> save <statefile>   跑到 200 帧存档，继续跑到 300 帧打印哈希
//   test_savestate <rom> load <statefile>   先乱跑 137 帧（制造一个「不同的过去」），
//                                           再读档，然后跑到 300 帧打印哈希
// 两条命令打印的 HASH@300 若逐位相同 → 存档完整、可跨进程还原，
// 且读档是「整体覆盖」而不是「依赖当前状态」。
//
// 注意 load 分支故意先跑 137 帧垃圾输入：如果 loadState 只是部分覆盖，
// 残留的旧状态会让哈希对不上，这个测试就会失败。
#include "core/emulator.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace fc;

namespace {

constexpr u32 kSaveFrame = 200;
constexpr u32 kEndFrame  = 300;

// 固定的伪随机输入序列：两次运行必须完全一致，
// 否则比的是「输入差异」而不是「存档差异」，测不出东西。
u8 inputAt(u32 f, int player) {
    u32 x = f * 2654435761u + (player ? 0x9E3779B9u : 0u);
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return u8(x);
}

bool readAll(const char* path, std::vector<u8>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size_t(n > 0 ? n : 0));
    if (n > 0) std::fread(out.data(), 1, size_t(n), f);
    std::fclose(f);
    return n > 0;
}

bool writeAll(const char* path, const std::vector<u8>& in) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fwrite(in.data(), 1, in.size(), f);
    std::fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("用法: test_savestate <rom.nes> save|load <statefile>\n");
        return 1;
    }
    const std::string rom = argv[1], mode = argv[2], file = argv[3];
    if (mode != "save" && mode != "load") {
        std::printf("模式只能是 save 或 load\n");
        return 1;
    }

    Emulator emu;
    if (!emu.loadROM(rom)) {
        std::printf("ROM 加载失败: %s\n", rom.c_str());
        return 1;
    }

    // 正常推进一帧：两个手柄都喂确定性输入
    auto step = [&](u32 f) {
        emu.bus().player1().setButtons(inputAt(f, 0));
        emu.bus().player2().setButtons(inputAt(f, 1));
        emu.runFrame();
    };

    if (mode == "load") {
        // 先制造一个与存档时完全不同的状态：证明 loadState 是整体覆盖
        for (u32 f = 0; f < 137; ++f) {
            emu.bus().player1().setButtons(u8(inputAt(f, 0) ^ 0xFF));
            emu.bus().player2().setButtons(u8(inputAt(f, 1) ^ 0xA5));
            emu.runFrame();
        }
        std::printf("预热: 已乱跑 137 帧（状态与存档点完全不同）\n");
    } else {
        for (u32 f = 0; f < kSaveFrame; ++f) step(f);
    }

    if (mode == "save") {
        std::vector<u8> blob;
        emu.saveState(blob);
        if (!writeAll(file.c_str(), blob)) {
            std::printf("写存档失败: %s\n", file.c_str());
            return 1;
        }
        std::printf("存档: %zu 字节 -> %s（第 %u 帧）\n", blob.size(), file.c_str(), kSaveFrame);
    } else {
        std::vector<u8> blob;
        if (!readAll(file.c_str(), blob)) {
            std::printf("读存档文件失败: %s\n", file.c_str());
            return 1;
        }
        if (!emu.loadState(blob.data(), blob.size())) {
            std::printf("loadState 失败（%zu 字节）\n", blob.size());
            return 1;
        }
        std::printf("读档: %zu 字节 <- %s\n", blob.size(), file.c_str());
    }

    // 从存档点继续跑到 kEndFrame，沿途记录哈希
    for (u32 f = kSaveFrame; f < kEndFrame; ++f) {
        step(f);
        if (f == 249 || f == 274 || f == kEndFrame - 1)
            std::printf("HASH@%u = %016llX\n", f + 1, (unsigned long long)emu.stateHash());
    }
    return 0;
}
