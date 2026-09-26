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

// ---------------------------------------------------------------- 启动器
// 阻塞运行主界面，直到用户点「开始游戏」或关闭窗口。
//   返回 true  = 开始游戏，state 已按用户选择更新（调用方负责 save）
//   返回 false = 用户要求退出程序
bool runLauncher(SDL_Window* win, SDL_Renderer* ren, ui::Ui& ui,
                 LaunchConfig& state, const std::string& keyConfigPath);

// 离屏截图用：把启动器的某一页画成一帧（tab: 0=游戏库 1=联机 2=设置）。
// 正常使用走 runLauncher，这个只为 --shot 视觉自测服务。
//
// clicks 里的每个点按顺序注入一次「鼠标按下」，各自独占一帧。
// 这是为交互类 bug 准备的：有些缺陷只在「点某一行之后」才发生（例如点击会改变列表长度、
// 而迭代仍按旧长度继续），单帧渲染路径下照着截图看不出来，却能靠合成点击稳定复现。
// 之所以要**一串**而不是一个点：进入子目录这类问题必须「点一下、等界面换了、再点一下」，
// 只看单次点击是测不到的（见 --ui-click 可重复给值）。
struct UiClick { float x = -1.0f, y = -1.0f; };

void drawLauncherPreview(ui::Ui& ui, const LaunchConfig& cfg, int tab,
                         const std::vector<UiClick>& clicks = {});

} // namespace fc
