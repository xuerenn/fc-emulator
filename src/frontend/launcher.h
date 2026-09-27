// launcher.h — 图形启动器：把原先只能靠命令行参数表达的东西搬进界面
//
// 定位：没有给 ROM 参数启动时进入启动器；给了 ROM 参数则直接进游戏（脚本/老用法不受影响）。
// 启动器只负责「产出一份 LaunchConfig」，真正的启动流程仍复用 main.cpp 里原有那套。
#pragma once

#include <SDL.h>

#include <string>
#include <vector>

#include "ui.h"

namespace fc {

// ---------------------------------------------------------------- 启动配置
struct LaunchConfig {
    std::string rom;            // 已选 ROM 的**实际文件路径**（空 = 未选）
    std::string browseDir;      // 文件浏览器当前目录（下次打开时恢复）

    // 选的是 zip 时：rom 指向解压出来的缓存文件，这里记住它来自哪个压缩包的哪一条。
    // 一是为了在界面上说明来源，二是缓存被清掉时能自动重新解一次。
    std::string romZip;         // 来源压缩包路径（空 = rom 本身就是最终文件）
    std::string romZipEntry;    // 压缩包内条目名（含包内目录前缀）

    enum class Net { Solo, Host, Client };
    Net  net   = Net::Solo;
    bool relay = false;         // 是否经中继服务器

    std::string hostIp      = "127.0.0.1";   // 客机要连的主机地址
    int         hostPort    = 7777;          // 主机监听端口
    int         connectPort = 7777;          // 客机连接端口
    std::string relayIp     = "127.0.0.1";
    int         relayPort   = 7777;
    std::string room        = "7777";
    int         delay       = 3;             // 输入延迟帧数

    std::string loadState;                   // 启动时读档（空 = 从开机状态起跑）

    int  scale      = 0;                     // 0 = 自动（取窗口能容纳的最大整数倍）
    bool fullscreen = false;
    bool audio      = true;
    // 离开当前进度前（暂停菜单里的「重启卡带」「换一张卡带」）要不要自动存一次档。
    // 开着更不容易丢进度，但不是所有人都想要一堆意外生成的 .fcstate —— 所以交给用户定。
    bool autoSave   = true;

    std::string keyFile;                     // 空 = 用默认位置

    // 上次的选择存在这里，下次打开启动器时恢复
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    static std::string defaultPath();

    // 按名字写单个字段（认得的键见 load 里的那串比较）；返回 false = 键名不认识。
    bool setField(const std::string& key, const std::string& value);
    // 把数值字段收进合法区间（端口、delay 等），load / setField 之后调用
    void clamp();

    // 能否开始游戏；不能时 why 给出人话原因
    bool validate(std::string* why) const;
};

// ---------------------------------------------------------------- 合成点击（开发者自测）
// 一个点 = 依次注入一次「鼠标按下」。正常路径下做不了的事（点某一行、再点某一行），
// 靠它就能无人值守地复现：有些缺陷只在「点下去之后」才发生。
struct UiClick { float x = -1.0f, y = -1.0f; };

// ---------------------------------------------------------------- 启动器
// 调用方塞给启动器的一点额外信息，都是给「从游戏里返回列表」和开发者自测用的。
struct LauncherHandoff {
    std::string toast;                 // 挂在启动器上的一句话（空 = 不显示）
    bool        toastWarn  = false;    // 用警示色还是普通色
    bool        autoStart  = false;    // 开发者自测：不等点击，几帧后直接开跑
    // 开发者自测：在启动器**实跑**的这一轮里按帧依次注入的合成点击。
    // 和 --shot 的离屏预览是同一套点，但这里走的是真实交互循环 ——
    // 「点一行 → 再点一行」这种多步操作只有在这里才测得准。
    std::vector<UiClick> clicks;
};

// 阻塞运行主界面，直到用户点「开始游戏」或关闭窗口。
//   返回 true  = 开始游戏，state 已按用户选择更新（调用方负责 save）
//   返回 false = 用户要求退出程序
bool runLauncher(SDL_Window* win, SDL_Renderer* ren, ui::Ui& ui,
                 LaunchConfig& state, const std::string& keyConfigPath,
                 const LauncherHandoff& handoff = {});

// 离屏截图用：把启动器的某一页画成一帧（tab: 0=游戏库 1=联机 2=设置）。
// 正常使用走 runLauncher，这个只为 --shot 视觉自测服务。
//
// clicks 见上面的 UiClick。这里的实现是「一帧画完算数」，所以整串点会在同一次调用里
// 依次消费；要测多步操作（点一下、等界面换了、再点一下）请走 runLauncher 那条路。
void drawLauncherPreview(ui::Ui& ui, const LaunchConfig& cfg, int tab,
                         const std::vector<UiClick>& clicks = {});

} // namespace fc
