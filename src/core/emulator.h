// emulator.h — 顶层：把 CPU / PPU / APU / 卡带 组装成一台可运行的 FC
#pragma once

#include "types.h"
#include "bus.h"

#include <string>

namespace fc {

class Emulator {
public:
    Emulator();

    bool loadROM(const std::string& path);
    void reset();

    // 精确推进一帧（到 VBlank 起点）。联机锁步以「帧」为同步单位。
    void runFrame();

    const u32* framebuffer() const { return bus_.ppu().framebuffer(); }
    Bus&       bus()       { return bus_; }
    const Bus& bus() const { return bus_; }
    Cartridge& cartridge() { return cart_; }
    u64        frameCount() const { return frameCount_; }
    void       setFrameCount(u64 n) { frameCount_ = n; }

    // ROM 文件指纹（FNV-1a 64，覆盖含 iNES 头在内的整个文件）。
    // 它会被写进快照，因而参与状态哈希 —— 两端 ROM 必须完全一致这件事
    // 就由「每 120 帧的哈希比对」自动兜住了：一旦不同，第 0 帧就会报警。
    // 不这么做的话，ROM 差异若落在从未被执行的字节上，状态哈希会长期相同。
    u64        romHash() const { return romHash_; }

    // ------------------------------------------------ 状态快照（回滚/校验）
    void saveState(std::vector<u8>& out) const;
    // 失败时 why 会带上可读原因（档损坏 / 格式过旧 / 与当前 ROM 不匹配）
    bool loadState(const u8* data, size_t n, std::string* why = nullptr);
    u64  stateHash() const;

private:
    Bus       bus_;
    Cartridge cart_;
    u64       frameCount_ = 0;
    u64       romHash_ = 0;
};

} // namespace fc
