// launcher.h — 图形启动器：把原先只能靠命令行参数表达的东西搬进界面
//
// 定位：没有给 ROM 参数启动时进入启动器；给了 ROM 参数则直接进游戏（脚本/老用法不受影响）。
// 启动器只负责「产出一份 LaunchConfig」，真正的启动流程仍复用 main.cpp 里原有那套。
#pragma once

#include <SDL.h>

#include <string>

#include "ui.h"

namespace fc {

// ---------------------------------------------------------------- 启动配置
struct LaunchConfig {
    std::string rom;            // 已选 ROM 的路径（空 = 未选）
    std::string browseDir;      // 文件浏览器当前目录（下次打开时恢复）

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

    std::string keyFile;                     // 空 = 用默认位置

    // 上次的选择存在这里，下次打开启动器时恢复
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    static std::string defaultPath();

    // 能否开始游戏；不能时 why 给出人话原因
    bool validate(std::string* why) const;
};

// ---------------------------------------------------------------- 启动器
// 阻塞运行主界面，直到用户点「开始游戏」或关闭窗口。
//   返回 true  = 开始游戏，state 已按用户选择更新（调用方负责 save）
//   返回 false = 用户要求退出程序
bool runLauncher(SDL_Window* win, SDL_Renderer* ren, ui::Ui& ui,
                 LaunchConfig& state, const std::string& keyConfigPath);

// 离屏截图用：把启动器的某一页画成一帧（tab: 0=游戏库 1=联机 2=设置）。
// 正常使用走 runLauncher，这个只为 --shot 视觉自测服务。
void drawLauncherPreview(ui::Ui& ui, const LaunchConfig& cfg, int tab);

} // namespace fc
