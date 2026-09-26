// keycfg.cpp — 键位映射实现
#include "keycfg.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fc {

namespace {

struct PlayerDefault {
    SDL_Scancode k[ACT_COUNT];
};

// 默认键位：
//   P1 = WASD 方向 + J/K 双键 + 右Shift/回车（这套就是俗称的「WASD + JK」）
//   P2 = 方向键 + Z/X，与 P1 完全不重叠，方便笔记本上本地双人
const PlayerDefault kDefaults[kPlayerCount] = {
    { { SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D,
        SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN } },
    { { SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_RCTRL, SDL_SCANCODE_RALT } },
};

// 配置文件里用的动作名，必须稳定（改了就等于改了配置格式）
const char* const kActionKey[ACT_COUNT] = {
    "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "SELECT", "START"
};

void trim(std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) { s.clear(); return; }
    s = s.substr(b, e - b + 1);
}

// 自带的忽略大小写比较：-std=c++17（严格 ANSI）下 MinGW 不一定暴露 strcasecmp
bool iequals(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = char(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = char(y - 'a' + 'A');
        if (x != y) return false;
    }
    return i == a.size() && b[i] == '\0';
}

} // namespace

u8 actionButton(int act) {
    switch (act) {
        case ACT_UP:     return BTN_UP;
        case ACT_DOWN:   return BTN_DOWN;
        case ACT_LEFT:   return BTN_LEFT;
        case ACT_RIGHT:  return BTN_RIGHT;
        case ACT_A:      return BTN_A;
        case ACT_B:      return BTN_B;
        case ACT_SELECT: return BTN_SELECT;
        case ACT_START:  return BTN_START;
        default:         return 0;
    }
}

const char* actionLabel(int act) {
    return (act >= 0 && act < ACT_COUNT) ? kActionKey[act] : "?";
}

const char* actionLabelCN(int act) {
    static const char* const cn[ACT_COUNT] = {
        "上", "下", "左", "右", "A 键", "B 键", "选择", "开始"
    };
    return (act >= 0 && act < ACT_COUNT) ? cn[act] : "?";
}

void KeyMap::setDefaults() {
    for (int p = 0; p < kPlayerCount; ++p)
        for (int a = 0; a < ACT_COUNT; ++a)
            keys[p][a] = kDefaults[p].k[a];
}

SDL_Scancode KeyMap::get(int player, int act) const {
    if (player < 0 || player >= kPlayerCount || act < 0 || act >= ACT_COUNT)
        return SDL_SCANCODE_UNKNOWN;
    return keys[player][act];
}

void KeyMap::set(int player, int act, SDL_Scancode sc) {
    if (player < 0 || player >= kPlayerCount || act < 0 || act >= ACT_COUNT) return;
    keys[player][act] = sc;
}

int KeyMap::conflictIn(int player, SDL_Scancode sc, int exceptAct) const {
    if (sc == SDL_SCANCODE_UNKNOWN) return -1;
    for (int a = 0; a < ACT_COUNT; ++a) {
        if (a == exceptAct) continue;
        if (keys[player][a] == sc) return a;
    }
    return -1;
}

bool KeyMap::overlapsOtherPlayer(int player, SDL_Scancode sc) const {
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    for (int p = 0; p < kPlayerCount; ++p) {
        if (p == player) continue;
        for (int a = 0; a < ACT_COUNT; ++a)
            if (keys[p][a] == sc) return true;
    }
    return false;
}

u8 KeyMap::sample(int player) const {
    if (player < 0 || player >= kPlayerCount) return 0;
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    if (!k) return 0;
    u8 b = 0;
    for (int a = 0; a < ACT_COUNT; ++a) {
        const SDL_Scancode sc = keys[player][a];
        if (sc > SDL_SCANCODE_UNKNOWN && sc < SDL_NUM_SCANCODES && k[sc]) b |= actionButton(a);
    }
    return b;
}

std::string keyName(SDL_Scancode sc) {
    const char* n = SDL_GetScancodeName(sc);
    if (!n || !*n) return "(NONE)";
    return n;
}

bool KeyMap::save(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    std::fprintf(f,
        "# fc-emulator 键位配置\n"
        "#\n"
        "# 用法：游戏里按 F2 打开键位设置界面，改完退出会自动写回本文件；\n"
        "#       也可以直接编辑下面几行，保存后约 1 秒内游戏自动热加载生效（联机中也生效）。\n"
        "#\n"
        "# 格式： <玩家>.<动作>=<键名>\n"
        "#   玩家 : P1 / P2\n"
        "#   动作 : UP DOWN LEFT RIGHT A B SELECT START\n"
        "#   键名 : SDL 扫描码名，可写成大写或小写，例如\n"
        "#          W A S D J K RETURN RSHIFT SPACE UP DOWN LEFT RIGHT\n"
        "#          KP_1 KP_2 LCTRL RCTRL LALT RALT COMMA PERIOD SLASH\n"
        "#   行的末尾注释里还带了该键的扫描码数字，仅作参考。\n"
        "#\n"
        "# 默认值： P1 = W S A D + J K（即 WASD + JK）\n"
        "#          P2 = 方向键 + Z X\n"
        "\n");

    for (int p = 0; p < kPlayerCount; ++p) {
        std::fprintf(f, "[P%d]\n", p + 1);
        for (int a = 0; a < ACT_COUNT; ++a) {
            const SDL_Scancode sc = keys[p][a];
            std::fprintf(f, "P%d.%s=%s   # scancode %d\n", p + 1, kActionKey[a],
                         keyName(sc).c_str(), int(sc));
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    return true;
}

bool KeyMap::load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;               // 首次运行没有配置文件，保持默认值即可

    char line[512];
    int applied = 0;
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        const size_t hash = s.find('#');
        const std::string tail = (hash == std::string::npos) ? std::string() : s.substr(hash);
        if (hash != std::string::npos) s = s.substr(0, hash);
        trim(s);
        if (s.empty()) continue;

        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;

        std::string lhs = s.substr(0, eq);
        std::string rhs = s.substr(eq + 1);
        trim(lhs);
        trim(rhs);

        // lhs 形如 "P1.UP"
        if (lhs.size() < 4 || (lhs[0] != 'P' && lhs[0] != 'p')) continue;
        const int player = (lhs[1] == '1') ? 0 : (lhs[1] == '2') ? 1 : -1;
        if (player < 0 || lhs[2] != '.') continue;

        const std::string actName = lhs.substr(3);
        int act = -1;
        for (int a = 0; a < ACT_COUNT; ++a) {
            if (iequals(actName, kActionKey[a])) { act = a; break; }
        }
        if (act < 0) continue;

        SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
        if (!rhs.empty()) sc = SDL_GetScancodeFromName(rhs.c_str());

        // 键名认不出来时，退回解析行尾注释里的扫描码数字。
        // 这样即使某台机器上的 SDL 键名表有差异，键位也不会悄悄丢回默认值。
        if (sc == SDL_SCANCODE_UNKNOWN && !tail.empty()) {
            const size_t tag = tail.find("scancode");
            if (tag != std::string::npos) {
                const long v = std::strtol(tail.c_str() + tag + 8, nullptr, 10);
                if (v > 0 && v < SDL_NUM_SCANCODES) sc = SDL_Scancode(v);
            }
        }

        if (sc != SDL_SCANCODE_UNKNOWN) { keys[player][act] = sc; ++applied; }
        else std::fprintf(stderr, "[键位] 无法识别 %s 的键名 \"%s\"，该项保持默认。\n",
                          lhs.c_str(), rhs.c_str());
    }
    std::fclose(f);
    return applied > 0;
}

std::string defaultKeyConfigPath() {
    char* base = SDL_GetBasePath();      // exe 所在目录，以分隔符结尾
    if (base) {
        std::string p(base);
        SDL_free(base);
        return p + "fc-keys.cfg";
    }
    return "fc-keys.cfg";
}

} // namespace fc
