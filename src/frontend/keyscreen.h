// keyscreen.h — 游戏内键位设置界面（F2）
#pragma once

#include <SDL.h>

#include <string>

#include "core/types.h"
#include "keycfg.h"

namespace fc {

// 界面「当前状态」。拆出来是为了让画面合成可以从事件循环里独立出来，
// 这样既能给 F2 用，也能被 --keypreview 直接渲染成图片自测（点阵字体最容易画错）。
struct KeyScreenView {
    int  col = 0;                 // 选中的玩家列 0/1
    int  row = 0;                 // 选中的动作行
    bool binding = false;         // 是否正在等待按键
    bool blink = false;           // 「PRESS...」的高亮相位（由调用方按时间翻转）
    std::string msg;              // 底部状态行；为空则显示默认提示
    bool msgWarn = false;
    std::string fileTag = "";     // 配置文件短名
};

// 把键位设置界面合成到 ARGB 缓冲（256x240）
void drawKeyConfigScreen(u32* buf, const KeyMap& km, const KeyScreenView& v);

// 阻塞式运行键位设置界面，直到用户按 Esc 返回游戏。
//   configPath : 退出时若有改动就写回这个文件
//   km         : 就地修改
//   quit       : 用户直接关窗口时置 true，调用方应退出游戏
// 返回 true 表示键位有改动。
//
// 注意：锁步联机时不要调用——本函数会让本机停止推进帧，
// 对端会一直等这一帧直到超时。调用方要先判断是否在联机中。
bool runKeyConfigScreen(SDL_Window* win, SDL_Renderer* ren,
                        const std::string& configPath, KeyMap& km, bool* quit);

} // namespace fc
