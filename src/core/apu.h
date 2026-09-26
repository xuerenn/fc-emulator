// apu.h — 2A03 APU 音频
#pragma once

#include "types.h"

namespace fc {

class Bus;

// 逐 CPU 周期推进的 APU。
// 输出侧：按目标采样率把混音结果推入 pending_ 缓冲，前端每帧取走并交给 SDL 播放。
class APU {
public:
    APU();

    void connectBus(Bus* bus) { bus_ = bus; }
    void reset();

    void tick();                       // 每个 CPU 周期调用一次

    u8   readStatus();
    void writeRegister(u16 addr, u8 value);

    bool irqPending() const { return frameIrq_; }
    void clearIrq() { frameIrq_ = false; }

    void setSampleRate(int hz) { sampleRate_ = hz > 0 ? hz : 44100; }
    int  sampleRate() const { return sampleRate_; }

    // 取走自上次调用以来产生的全部采样（int16 单声道）
    void takeSamples(std::vector<s16>& out);

    void save(StateWriter& w) const;
    void load(StateReader& r);

private:
    struct Pulse {
        bool enabled = false;
        u8   duty = 0, dutyPos = 0;
        u16  timer = 0, period = 0;
        u8   length = 0, lengthHalt = 0;
        bool constVol = false;
        u8   volParam = 0, volume = 0;
        u8   envDivider = 0, envDecay = 0;
        bool envStart = false;
        u8   sweepShift = 0, sweepPeriod = 0, sweepDiv = 0;
        bool sweepNeg = false, sweepEnable = false, sweepReload = false;
        u8   out = 0;
    };
    struct Triangle {
        bool enabled = false;
        u16  timer = 0, period = 0;
        u8   length = 0, lengthHalt = 0;
        u8   linear = 0, linearReload = 0;
        u8   seqPos = 0;
        u8   out = 0;
    };
    struct Noise {
        bool enabled = false;
        u16  timer = 0, period = 0;
        u8   length = 0, lengthHalt = 0;
        bool constVol = false;
        u8   volParam = 0, volume = 0;
        u8   envDivider = 0, envDecay = 0;
        bool envStart = false;
        u16  lfsr = 1;
        bool mode = false;
        u8   out = 0;
    };
    struct Dmc {
        bool enabled = false, irqEnable = false, loop = false;
        u16  timer = 0, period = 0;
        u16  addr = 0, addrStart = 0;
        u16  sampleLength = 0, sampleLeft = 0;
        u8   shift = 0, bitsLeft = 0;
        u8   level = 0;
        bool bufferFull = false;
        u8   buffer = 0;
        bool silence = true;
    };

    void tickPulse(Pulse& p, int channel);
    void tickTriangle();
    void tickNoise();
    void tickDmc();

    void quarterFrame();
    void halfFrame();
    void sweepClock(Pulse& p, int channel);

    void pushSample();
    float mix() const;

    Pulse    pulse1_, pulse2_;
    Triangle tri_;
    Noise    noise_;
    Dmc      dmc_;

    int  frameCounter_ = 0;
    bool mode5_ = false;
    bool irqInhibit_ = false;
    bool frameIrq_ = false;

    int  sampleRate_ = 44100;
    int  cycleAccum_ = 0;
    std::vector<s16> pending_;

    Bus* bus_ = nullptr;
};

} // namespace fc
