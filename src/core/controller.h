// controller.h — 标准手柄（4016/4017 移位寄存器）
#pragma once

#include "types.h"

namespace fc {

// 位定义与前端/网络协议共用：
//   bit0=A bit1=B bit2=Select bit3=Start bit4=Up bit5=Down bit6=Left bit7=Right
enum Button : u8 {
    BTN_A      = 0x01,
    BTN_B      = 0x02,
    BTN_SELECT = 0x04,
    BTN_START  = 0x08,
    BTN_UP     = 0x10,
    BTN_DOWN   = 0x20,
    BTN_LEFT   = 0x40,
    BTN_RIGHT  = 0x80,
};

class Controller {
public:
    void reset() { buttons_ = 0; shift_ = 0; strobe_ = false; }
    void setButtons(u8 b) { buttons_ = b; }
    u8   buttons() const { return buttons_; }   // 回读当前施加的按键（前端显示/自测用）

    void write(u8 v) {
        if (v & 1) {
            strobe_ = true;
            shift_ = buttons_;
        } else if (strobe_) {
            strobe_ = false;
            shift_ = buttons_;
        }
    }

    u8 read() {
        u8 v = u8(shift_ & 1);
        shift_ >>= 1;
        return u8(v | 0x40);   // 未接的位读回 1
    }

    void save(StateWriter& w) const { w.u8v(buttons_); w.u8v(shift_); w.boolv(strobe_); }
    void load(StateReader& r) { buttons_ = r.u8v(); shift_ = r.u8v(); strobe_ = r.boolv(); }

private:
    u8   buttons_ = 0;
    u8   shift_ = 0;
    bool strobe_ = false;
};

} // namespace fc
