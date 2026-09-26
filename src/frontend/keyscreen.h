// keyscreen.h — 键位设置界面（启动器里可进，单机游戏中按 F2 也可进）
//
// 与旧版的区别：不再往 256x240 的 ARGB 缓冲里软合成点阵画面，而是直接用
// ui 基础层画到 renderer。好处是字体是矢量平滑的、配色与启动器一致，
// 而且是按窗口真实分辨率布局。
#pragma once

#include <SDL.h>

#include <string>

#include "core/types.h"
#include "keycfg.h"
#include "ui.h"

namespace fc {

// 界面「当前状态」。把它从事件循环里拆出来，是为了让同一套绘制逻辑
// 既能给交互用，也能给离屏截图自测用。
struct KeyScreenView {
    int  col     = 0;                 // 选中的玩家列 0/1
    int  row     = 0;                 // 选中的动作行
    bool binding = false;             // 是否正在等待按键
    bool blink   = false;             // 「按下任意键…」的闪烁相位
    std::string msg;                  // 底部状态行；为空则显示默认提示
    bool msgWarn = false;
    std::string fileTag;              // 配置文件短名
};

// 绘制整屏（运行与截图共用同一条路径）
void drawKeyScreen(ui::Ui& ui, const KeyMap& km, const KeyScreenView& v);

// 阻塞式运行键位设置界面，直到用户按 Esc 返回。
//   configPath : 退出时若有改动就写回这个文件
//   km         : 就地修改
//   quit       : 用户直接关窗口时置 true，调用方应退出程序
// 返回 true 表示键位有改动。
//
// 注意：锁步联机时不要调用 —— 本函数会让本机停止推进帧，对端会一直等这一帧。
bool runKeyConfigScreen(SDL_Window* win, SDL_Renderer* ren, ui::Ui& ui,
                        const std::string& configPath, KeyMap& km, bool* quit);

} // namespace fc
