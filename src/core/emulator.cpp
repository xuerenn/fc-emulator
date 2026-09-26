// emulator.cpp — 顶层实现
#include "emulator.h"

#include <cstdio>

namespace fc {

namespace {

// 对整个 ROM 文件（含 iNES 头）做 FNV-1a 64。用它当「这是哪块卡带」的身份指纹：
// 只要两端不是同一个文件，指纹就不同，联机第一次哈希比对就会命中。
u64 hashRomFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;

    u64 h = 1469598103934665603ull;
    u8 buf[8192];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; ++i) {
            h ^= buf[i];
            h *= 1099511628211ull;
        }
    }
    std::fclose(f);
    return h;
}

constexpr u32 kStateMagic = 0x46434E32u;   // 'FCN2'（含 ROM 指纹，旧版 FCN1 已不兼容）

} // namespace

Emulator::Emulator() {
    bus_.connectCartridge(&cart_);
    bus_.ppu().connectCartridge(&cart_);
}

bool Emulator::loadROM(const std::string& path) {
    if (!cart_.loadFromFile(path)) return false;
    romHash_ = hashRomFile(path);
    reset();
    return true;
}

void Emulator::reset() {
    cart_.reset();
    bus_.reset();
    bus_.connectCartridge(&cart_);
    bus_.ppu().connectCartridge(&cart_);
    frameCount_ = 0;
}

void Emulator::runFrame() {
    PPU& ppu = bus_.ppu();
    CPU6502& cpu = bus_.cpu();
    APU& apu = bus_.apu();

    ppu.clearFrameComplete();

    // 上限保护：防止卡带异常导致死循环
    u32 guard = 0;
    const u32 kGuardMax = 200000;

    while (!ppu.frameComplete()) {
        if (cpu.instructionComplete()) {
            if (ppu.nmiRequested()) {
                ppu.clearNmi();
                cpu.nmi();
            } else if (apu.irqPending() || cart_.irqPending()) {
                cpu.irq();
                apu.clearIrq();
                cart_.clearIrq();
            }
        }

        cpu.clock();
        ppu.tick();
        ppu.tick();
        ppu.tick();
        apu.tick();
        cart_.tickCpu();

        if (++guard > kGuardMax) break;
    }

    frameCount_++;
}

void Emulator::saveState(std::vector<u8>& out) const {
    StateWriter w;
    w.u32v(kStateMagic);
    w.u64v(romHash_);             // 卡带指纹：让「两端 ROM 不一致」也能被哈希抓到
    w.u64v(frameCount_);
    bus_.save(w);
    cart_.save(w);
    out = w.data();
}

bool Emulator::loadState(const u8* data, size_t n, std::string* why) {
    auto fail = [&](const char* r) { if (why) *why = r; return false; };
    if (!data || n < 20) return fail("档太短或为空");

    StateReader r(data, n);
    const u32 magic = r.u32v();
    if (magic != kStateMagic) {
        return fail(magic == 0x46434E31u
                        ? "这是旧版（FCN1）的档，该格式已停止支持，请重新存一次"
                        : "不是本模拟器的存档");
    }

    const u64 stRomHash = r.u64v();
    if (stRomHash != romHash_) return fail("这个档是用另一个 ROM 存的（卡带指纹不匹配）");

    // 注意：romHash_ 本身不随档恢复 —— 它必须始终反映「本机当前加载的 ROM」，
    // 否则用 A 的档配 B 的 ROM 时，两端哈希会一起变成档里的值，把 ROM 差异掩盖掉。
    frameCount_ = r.u64v();
    bus_.load(r);
    cart_.load(r);
    return true;
}

u64 Emulator::stateHash() const {
    std::vector<u8> buf;
    saveState(buf);
    u64 h = 1469598103934665603ull;         // FNV-1a 64
    for (u8 b : buf) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace fc
