// launcher.cpp — 图形启动器实现
//
// 界面结构：
//   顶栏（品牌 + 页签） / 内容区（库 · 联机 · 设置） / 底栏（当前选择 + 开始游戏）
//
// 两个实现上的约定：
//   1) 所有绘制都按 renderer 输出的真实像素坐标。窗口缩放时同步 ui.setViewport，
//      所以布局跟着变，而不是靠 SDL 的逻辑分辨率去拉伸。
//   2) hover / 选中这类状态走 AnimSet 平滑趋近，避免鼠标一动颜色就"啪"地跳变。
#include "launcher.h"

#include "core/cartridge.h"
#include "keycfg.h"
#include "keyscreen.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#endif

using namespace fc::ui;

namespace fc {
namespace {

// ================================================================ 路径与文件

#ifdef _WIN32
std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n);
    return w;
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}
#endif

std::string exeDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return ".";
    const std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return ".";
    return toUtf8(p.substr(0, slash));
#else
    return ".";
#endif
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '\\' || dir.back() == '/') return dir + name;
    return dir + "\\" + name;
}

std::string parentPath(const std::string& p) {
    std::string s = p;
    while (s.size() > 1 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    const size_t i = s.find_last_of("\\/");
    if (i == std::string::npos) return s;
    if (i == 2 && s.size() > 2 && s[1] == ':') return s.substr(0, 3);   // 盘符根 "C:\"
    if (i == 0) return s.substr(0, 1);
    return s.substr(0, i);
}

std::string fileNameOf(const std::string& p) {
    const size_t i = p.find_last_of("\\/");
    return (i == std::string::npos) ? p : p.substr(i + 1);
}

std::string stripExt(const std::string& p) {
    const size_t dot = p.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return p;
    return p.substr(0, dot);
}

bool fileExists(const std::string& p) {
#ifdef _WIN32
    return GetFileAttributesW(toWide(p).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0;
#endif
}

bool isDirectory(const std::string& p) {
#ifdef _WIN32
    const DWORD a = GetFileAttributesW(toWide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

long long fileSize(const std::string& p) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(toWide(p).c_str(), GetFileExInfoStandard, &d)) return 0;
    LARGE_INTEGER li;
    li.LowPart  = d.nFileSizeLow;
    li.HighPart = d.nFileSizeHigh;
    return li.QuadPart;
#else
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return 0;
    return st.st_size;
#endif
}

std::string humanSize(long long n) {
    char b[64];
    if (n >= 1024LL * 1024)      std::snprintf(b, sizeof(b), "%.1f MB", double(n) / 1048576.0);
    else if (n >= 1024)          std::snprintf(b, sizeof(b), "%lld KB", (long long)(n / 1024));
    else                         std::snprintf(b, sizeof(b), "%lld B", n);
    return b;
}

bool isNesFile(const std::string& name) {
    if (name.size() < 4) return false;
    std::string e = name.substr(name.size() - 4);
    for (char& c : e) c = char(std::tolower((unsigned char)c));
    return e == ".nes";
}

struct DirEntry {
    std::string name;
    bool        isDir = false;
    long long   size  = 0;
};

std::vector<DirEntry> listDir(const std::string& path) {
    std::vector<DirEntry> out;
#ifdef _WIN32
    std::wstring pattern = toWide(path);
    if (pattern.empty()) return out;
    if (pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;

        DirEntry e;
        e.name  = toUtf8(name);
        e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!e.isDir) {
            LARGE_INTEGER li;
            li.LowPart  = fd.nFileSizeLow;
            li.HighPart = fd.nFileSizeHigh;
            e.size = li.QuadPart;
        }
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(path.c_str());
    if (!d) return out;
    while (struct dirent* de = readdir(d)) {
        const std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        DirEntry e;
        e.name  = name;
        e.isDir = isDirectory(joinPath(path, name));
        if (!e.isDir) e.size = fileSize(joinPath(path, name));
        out.push_back(std::move(e));
    }
    closedir(d);
#endif
    // 目录在前，同类按名字排序（忽略大小写），跟资源管理器习惯一致
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        std::string la = a.name, lb = b.name;
        for (char& c : la) c = char(std::tolower((unsigned char)c));
        for (char& c : lb) c = char(std::tolower((unsigned char)c));
        return la < lb;
    });
    return out;
}

// ================================================================ ROM 概要

const char* mapperLabel(int m) {
    switch (m) {
        case 0:  return "NROM";
        case 1:  return "MMC1";
        case 2:  return "UxROM";
        case 3:  return "CNROM";
        case 58: return "BMC-68in1";
        default: return "不支持";
    }
}

const char* mirrorLabel(int m) {
    switch (m) {
        case MIRROR_HORIZONTAL: return "水平";
        case MIRROR_VERTICAL:   return "垂直";
        case MIRROR_FOUR:       return "四屏";
        default:                return "单屏";
    }
}

struct RomInfo {
    bool        valid    = false;   // iNES 头合法
    int         mapper   = 0;
    std::string mapperName;
    int         prgKb    = 0;
    int         chrKb    = 0;
    int         mirror   = 0;
    bool        battery  = false;
    long long   bytes    = 0;
    bool        hasState = false;
    std::string statePath;
};

// 只读 16 字节 iNES 头。不用 Cartridge 真加载 —— 列表里每个 ROM 都建一次
// Mapper 太浪费，而头里的信息已经够把「这个 ROM 是什么」讲清楚。
RomInfo inspectRom(const std::string& path) {
    RomInfo r;
    r.bytes     = fileSize(path);
    r.statePath = path + ".fcstate";
    r.hasState  = fileExists(r.statePath);
    r.mapperName = mapperLabel(0);

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return r;
    u8 h[16]{};
    const size_t got = std::fread(h, 1, 16, f);
    std::fclose(f);
    if (got != 16) return r;
    if (h[0] != 'N' || h[1] != 'E' || h[2] != 'S' || h[3] != 0x1A) return r;

    r.valid      = true;
    r.mapper     = (h[6] >> 4) | (h[7] & 0xF0);
    r.prgKb      = h[4] * 16;
    r.chrKb      = h[5] * 8;
    r.battery    = (h[6] & 0x02) != 0;
    r.mirror     = (h[6] & 0x08) ? MIRROR_FOUR
                                 : ((h[6] & 0x01) ? MIRROR_VERTICAL : MIRROR_HORIZONTAL);
    r.mapperName = mapperLabel(r.mapper);
    return r;
}

// ================================================================ 交互输入

// 输入快照与控件动画都来自 ui 层，这里取个别名即可，各界面共用同一套
using Frame = Input;

// ================================================================ 文本输入框

struct TextField {
    std::string text;
    size_t      cursor  = 0;
    bool        focused = false;
    int         maxLen  = 64;
    bool        numeric = false;
    double      caretT  = 0;
};

void textFieldInput(TextField& tf, const SDL_Event& e) {
    if (!tf.focused) return;

    if (e.type == SDL_TEXTINPUT) {
        const std::string in = e.text.text;
        for (size_t i = 0; i < in.size();) {
            // 逐字符过滤：数字框只收 ASCII 数字，其余一律丢弃
            size_t len = 1;
            while (i + len < in.size() && (u8(in[i + len]) & 0xC0) == 0x80) ++len;
            const std::string ch = in.substr(i, len);
            i += len;
            if (tf.numeric && (ch.size() != 1 || !std::isdigit((unsigned char)ch[0]))) continue;
            if (tf.text.size() + ch.size() > size_t(tf.maxLen)) break;
            tf.text.insert(tf.cursor, ch);
            tf.cursor += ch.size();
        }
        return;
    }

    if (e.type != SDL_KEYDOWN) return;
    const SDL_Keycode k = e.key.keysym.sym;
    const bool ctrl = (e.key.keysym.mod & KMOD_CTRL) != 0;

    switch (k) {
        case SDLK_BACKSPACE:
            if (tf.cursor > 0) {
                size_t p = tf.cursor - 1;
                while (p > 0 && (u8(tf.text[p]) & 0xC0) == 0x80) --p;
                tf.text.erase(p, tf.cursor - p);
                tf.cursor = p;
            }
            break;
        case SDLK_DELETE:
            if (tf.cursor < tf.text.size()) {
                size_t p = tf.cursor + 1;
                while (p < tf.text.size() && (u8(tf.text[p]) & 0xC0) == 0x80) ++p;
                tf.text.erase(tf.cursor, p - tf.cursor);
            }
            break;
        case SDLK_LEFT:
            if (tf.cursor > 0) {
                size_t p = tf.cursor - 1;
                while (p > 0 && (u8(tf.text[p]) & 0xC0) == 0x80) --p;
                tf.cursor = p;
            }
            break;
        case SDLK_RIGHT:
            if (tf.cursor < tf.text.size()) {
                size_t p = tf.cursor + 1;
                while (p < tf.text.size() && (u8(tf.text[p]) & 0xC0) == 0x80) ++p;
                tf.cursor = p;
            }
            break;
        case SDLK_HOME: tf.cursor = 0; break;
        case SDLK_END:  tf.cursor = tf.text.size(); break;
        case SDLK_v:
            if (ctrl) {
                if (char* clip = SDL_GetClipboardText()) {
                    std::string s = clip;
                    SDL_free(clip);
                    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                                          s.back() == ' '  || s.back() == '\t')) s.pop_back();
                    if (tf.numeric) {
                        std::string only;
                        for (char c : s) if (std::isdigit((unsigned char)c)) only += c;
                        s = only;
                    }
                    if (tf.text.size() + s.size() <= size_t(tf.maxLen)) {
                        tf.text.insert(tf.cursor, s);
                        tf.cursor += s.size();
                    }
                }
            }
            break;
        default:
            break;
    }
}

// 输入框的绘制。文本超宽时用裁剪矩形硬切（而不是加省略号）——
// 输入框里加省略号会让光标位置和显示内容对不上，反而更难用。
void textField(Ui& u, const Frame& in, TextField& tf, const SDL_Rect& r,
               const std::string& placeholder, bool numeric) {
    tf.numeric = numeric;
    const bool hot = in.inside(r);
    if (in.pressed) {
        tf.focused = hot;
        if (hot) tf.caretT = 0.0;
    }

    const RGBA bg     = tf.focused ? mix(theme::surface, theme::accent, 0.07f) : theme::surface;
    const RGBA border = tf.focused ? theme::accent
                                   : (hot ? theme::borderHi : theme::border);
    u.round(r, radius::Input, bg);
    u.roundOutline(r, radius::Input, tf.focused ? 2 : 1, border);

    const bool empty = tf.text.empty();
    const int  tx = r.x + 12;
    const int  ty = r.y + (r.h - u.lineHeight(Font::Mono)) / 2;

    SDL_RenderSetClipRect(u.ren(), &r);
    u.text(Font::Mono, empty ? placeholder : tf.text, tx, ty,
           empty ? theme::textFaint : theme::text);
    if (tf.focused && std::fmod(tf.caretT, 1.06) < 0.58) {
        const int cx = tx + u.measure(Font::Mono, tf.text.substr(0, tf.cursor));
        u.fill(cx + 1, r.y + 9, 2, r.h - 18, theme::accent);
    }
    SDL_RenderSetClipRect(u.ren(), nullptr);
}

// ================================================================ 通用控件

enum class BtnStyle { Normal, Primary, Danger, Ghost, Subtle };

bool button(Ui& u, const Frame& in, double dt, const std::string& id, const SDL_Rect& r,
            const std::string& label, BtnStyle style = BtnStyle::Normal,
            bool enabled = true, float fontSize = 0.0f) {
    (void)fontSize;
    const bool hot = enabled && in.inside(r);
    const float k  = animStep("btn:" + id, hot, dt);

    RGBA bg, fg, border = rgba(0, 0, 0, 0);
    switch (style) {
        case BtnStyle::Primary:
            bg = mix(theme::accent, mix(theme::accent, rgba(255, 255, 255), 0.16f), k);
            fg = theme::accentInk;
            break;
        case BtnStyle::Danger:
            bg     = mix(fade(theme::err, 0.13f), fade(theme::err, 0.26f), k);
            fg     = theme::err;
            border = fade(theme::err, 0.55f);
            break;
        case BtnStyle::Ghost:
            bg     = mix(rgba(0, 0, 0, 0), fade(theme::text, 0.07f), k);
            fg     = mix(theme::textDim, theme::text, k);
            border = mix(theme::border, theme::borderHi, k);
            break;
        case BtnStyle::Subtle:
            bg = mix(theme::surfaceHi, theme::hover, k);
            fg = mix(theme::textDim, theme::text, k);
            break;
        default:
            bg     = mix(theme::surfaceHi, theme::hover, k);
            fg     = theme::text;
            border = mix(theme::border, theme::borderHi, k);
            break;
    }
    if (!enabled) {
        bg     = theme::surface;
        fg     = theme::textFaint;
        border = theme::line;
    }

    if (style == BtnStyle::Primary && enabled) u.shadow(r, radius::Button, 10, 180);
    if (bg.a > 0) u.round(r, radius::Button, bg);
    if (border.a > 0) u.roundOutline(r, radius::Button, 1, border);

    u.textCenter(Font::Body, label, r, fg);
    return hot && in.pressed;
}

// 一行开关项，返回是否被点击。
// 传入的 r 至少要 50px 高 —— 双行文字（主标题 + 说明）刚够，给矮了说明会溢出去。
bool switchRow(Ui& u, const Frame& in, double dt, const std::string& id, const SDL_Rect& r,
               const std::string& label, const std::string& desc, bool on) {
    const bool hot = in.inside(r);
    const float k  = animStep("sw:" + id, hot, dt);
    if (k > 0.01f) u.round(r, radius::Button, fade(theme::text, 0.04f * k));

    if (desc.empty()) {
        u.textVCenter(Font::Body, label, r.x + 4, r, theme::text);
    } else {
        u.text(Font::Body, label, r.x + 4, r.y + 5, theme::text);
        u.text(Font::Small, desc, r.x + 4, r.y + 6 + u.lineHeight(Font::Body), theme::textFaint);
    }

    // 右侧胶囊开关
    const int sw = 46, sh = 26;
    const SDL_Rect track{ r.x + r.w - sw - 4, r.y + (r.h - sh) / 2, sw, sh };
    const float tk = animStep("swk:" + id, on, dt);
    u.round(track, sh / 2, mix(theme::border, theme::accent, tk));
    const int knobX = track.x + 3 + int(float(sw - sh) * tk);
    u.round(SDL_Rect{ knobX, track.y + 3, sh - 6, sh - 6 }, (sh - 6) / 2, rgba(245, 249, 255));

    return hot && in.pressed;
}

// 卡片外壳：阴影 + 圆角 + 描边 + 顶部内高光
SDL_Rect cardShell(Ui& u, const SDL_Rect& r, bool elevated = true) {
    if (elevated) u.shadow(r, radius::Card, 12, 110);
    u.round(r, radius::Card, theme::surfaceHi);
    u.roundOutline(r, radius::Card, 1, theme::border);
    u.hline(r.x + radius::Card, r.y, r.w - radius::Card * 2, rgba(255, 255, 255, 12));
    return r;
}

// 卡片标题行
int cardTitle(Ui& u, const SDL_Rect& card, const std::string& title, const std::string& hint = "") {
    u.text(Font::Title, title, card.x + 20, card.y + 16, theme::text);
    if (!hint.empty())
        u.text(Font::Small, hint, card.x + 20, card.y + 44, theme::textFaint);
    return card.y + (hint.empty() ? 54 : 68);
}

// 页签胶囊（顶栏 / 分段选择共用）。返回被点中的段索引，没点中返回 -1。
// 动画 key 里带上坐标，避免不同位置但同名的段互相干扰动画值。
int pillTab(Ui& u, const Frame& in, double dt, const SDL_Rect& r,
            const std::vector<std::string>& labels, int active) {
    u.round(r, radius::Pill, theme::surface);
    u.roundOutline(r, radius::Pill, 1, theme::border);

    const int n  = int(labels.size());
    const int tw = r.w / n;
    int hit = -1;
    const std::string pos = std::to_string(r.x) + "," + std::to_string(r.y) + ",";
    for (int i = 0; i < n; ++i) {
        const SDL_Rect t{ r.x + i * tw + 4, r.y + 4, tw - 8, r.h - 8 };
        const bool hot = in.inside(t);
        const float k  = animStep("tab:" + pos + labels[size_t(i)], hot, dt);
        if (i == active) {
            u.shadow(t, radius::Pill, 7, 150);
            u.round(t, radius::Pill, theme::accent);
            u.textCenter(Font::Body, labels[size_t(i)], t, theme::accentInk);
        } else {
            if (k > 0.01f) u.round(t, radius::Pill, fade(theme::text, 0.07f * k));
            u.textCenter(Font::Body, labels[size_t(i)], t, mix(theme::textDim, theme::text, k));
        }
        if (hot && in.pressed) hit = i;
    }
    return hit;
}

// 选择卡片（用于「单机 / 主机 / 客机」这类大选项）
bool choiceCard(Ui& u, const Frame& in, double dt, const std::string& id, const SDL_Rect& r,
                const std::string& title, const std::string& desc, bool on, int iconKind) {
    const bool hot = in.inside(r);
    const float k  = animStep("cc:" + id, hot, dt);
    const float onK = animStep("cco:" + id, on, dt);

    const RGBA bg     = mix(mix(theme::surface, theme::hover, k), fade(theme::accent, 0.13f), onK);
    const RGBA border = mix(mix(theme::border, theme::borderHi, k), theme::accent, onK);

    u.round(r, radius::Panel, bg);
    u.roundOutline(r, radius::Panel, onK > 0.5f ? 2 : 1, border);

    // 左上角图标
    const int ix = r.x + 18, iy = r.y + 20;
    const RGBA ic = mix(theme::textDim, theme::accent, onK);
    switch (iconKind) {
        case 0:   // 单人：头 + 肩
            u.ring(ix + 9, iy + 7, 5, 2, ic);
            u.ring(ix + 9, iy + 24, 10, 2, ic, 200, 340);
            break;
        case 1:   // 主机：屏幕 + 底座
            u.roundOutline(SDL_Rect{ ix, iy, 20, 14 }, 3, 2, ic);
            u.fill(ix + 6, iy + 17, 8, 2, ic);
            break;
        default:  // 客机：插头 / 链接
            u.ring(ix + 6, iy + 9, 6, 2, ic);
            u.ring(ix + 14, iy + 15, 6, 2, ic);
            break;
    }

    u.text(Font::Body, title, r.x + 18, r.y + 50, on ? theme::text : mix(theme::text, theme::textDim, 0.3f));
    u.text(Font::Small, desc, r.x + 18, r.y + 74, theme::textFaint);

    // 选中标记
    if (onK > 0.01f) {
        const SDL_Rect dot{ r.x + r.w - 30, r.y + 18, 16, 16 };
        u.round(dot, 8, fade(theme::accent, onK));
        u.thickLine(dot.x + 4, dot.y + 8, dot.x + 7, dot.y + 11, 2, fade(theme::accentInk, onK));
        u.thickLine(dot.x + 7, dot.y + 11, dot.x + 12, dot.y + 5, 2, fade(theme::accentInk, onK));
    }
    return hot && in.pressed;
}

// 小标签
void chip(Ui& u, int x, int y, const std::string& text, const RGBA& fg, const RGBA& bg) {
    const int tw = u.measure(Font::Small, text);
    const SDL_Rect r{ x, y, tw + 16, 22 };
    u.round(r, radius::Chip, bg);
    u.textCenter(Font::Small, text, r, fg);
}

// 按可用宽度折行（按字符断，对中英混排够用）
std::vector<std::string> wrapText(Ui& u, Font role, const std::string& text, int maxW) {
    std::vector<std::string> lines;
    std::string cur;
    for (size_t i = 0; i < text.size();) {
        size_t len = 1;
        while (i + len < text.size() && (u8(text[i + len]) & 0xC0) == 0x80) ++len;
        const std::string ch = text.substr(i, len);
        i += len;
        const std::string next = cur + ch;
        if (!cur.empty() && u.measure(role, next) > maxW) {
            lines.push_back(cur);
            cur = ch;
        } else {
            cur = next;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// 卡带图标（详情页大图标 / 列表小图标共用）
void cartridgeIcon(Ui& u, const SDL_Rect& r, const RGBA& body, const RGBA& ink) {
    const int rad = std::max(3, r.w / 8);
    u.round(r, rad, body);
    // 上方的标签条
    u.round(SDL_Rect{ r.x + r.w / 6, r.y + r.h / 8, r.w * 2 / 3, r.h / 4 }, rad / 2, ink);

    // 下缘的金手指
    const int n = 6;
    const int step = r.w / (n + 1);
    for (int i = 0; i < n; ++i) {
        u.fill(r.x + step * (i + 1) - 1, r.y + r.h - r.h / 5,
               2, r.h / 6, fade(ink, 0.75f));
    }
}

// ================================================================ 启动器状态

enum class Tab { Library, Netplay, Settings };

struct State {
    Tab tab = Tab::Library;

    std::vector<DirEntry> entries;
    std::string browseDir;
    int   sel      = -1;      // 列表选中下标
    float scroll   = 0;
    float scrollTo = 0;

    std::string infoFor;      // info 对应哪个文件，避免重复解析
    RomInfo     info;

    TextField fHostIp, fHostPort, fConnectPort, fRelayIp, fRelayPort, fRoom;

    std::string toast;
    double      toastUntil = 0;
    bool        toastWarn  = false;

    double lastClickT = 0;
    float  lastClickY = -999;

    bool draggingDelay = false;   // 输入延迟滑杆是否正在拖动
    float settingsScroll = 0;     // 设置页滚动位置（内容比一屏高时才用得上）
};

void toast(State& st, const std::string& msg, bool warn = false) {
    st.toast      = msg;
    st.toastWarn  = warn;
    st.toastUntil = double(SDL_GetTicks()) + 2600.0;
}

} // namespace

// ================================================================ LaunchConfig

std::string LaunchConfig::defaultPath() {
#ifdef _WIN32
    return exeDir() + "\\fc-launcher.cfg";
#else
    return exeDir() + "/fc-launcher.cfg";
#endif
}

namespace {

void writeKV(std::FILE* f, const char* k, const std::string& v) {
    std::fprintf(f, "%s=%s\n", k, v.c_str());
}

bool readKV(std::FILE* f, std::string& k, std::string& v) {
    char line[1024];
    if (!std::fgets(line, sizeof(line), f)) return false;
    std::string s = line;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (s.empty() || s[0] == '#') { k.clear(); return true; }   // 空行/注释：让调用方继续
    const size_t eq = s.find('=');
    if (eq == std::string::npos) { k.clear(); return true; }
    k = s.substr(0, eq);
    v = s.substr(eq + 1);
    return true;
}

} // namespace

bool LaunchConfig::save(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "# fc-emulator 启动器配置（程序自动维护，可手改）\n");
    writeKV(f, "rom",         rom);
    writeKV(f, "browseDir",   browseDir);
    writeKV(f, "net",         std::to_string(int(net)));
    writeKV(f, "relay",       relay ? "1" : "0");
    writeKV(f, "hostIp",      hostIp);
    writeKV(f, "hostPort",    std::to_string(hostPort));
    writeKV(f, "connectPort", std::to_string(connectPort));
    writeKV(f, "relayIp",     relayIp);
    writeKV(f, "relayPort",   std::to_string(relayPort));
    writeKV(f, "room",        room);
    writeKV(f, "delay",       std::to_string(delay));
    writeKV(f, "loadState",   loadState);
    writeKV(f, "scale",       std::to_string(scale));
    writeKV(f, "fullscreen",  fullscreen ? "1" : "0");
    writeKV(f, "audio",       audio ? "1" : "0");
    writeKV(f, "keyFile",     keyFile);
    std::fclose(f);
    return true;
}

bool LaunchConfig::load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string k, v;
    while (readKV(f, k, v)) {
        if (k.empty()) continue;
        if      (k == "rom")         rom = v;
        else if (k == "browseDir")   browseDir = v;
        else if (k == "net")         net = Net(std::atoi(v.c_str()));
        else if (k == "relay")       relay = (v == "1");
        else if (k == "hostIp")      hostIp = v;
        else if (k == "hostPort")    hostPort = std::atoi(v.c_str());
        else if (k == "connectPort") connectPort = std::atoi(v.c_str());
        else if (k == "relayIp")     relayIp = v;
        else if (k == "relayPort")   relayPort = std::atoi(v.c_str());
        else if (k == "room")        room = v;
        else if (k == "delay")       delay = std::atoi(v.c_str());
        else if (k == "loadState")   loadState = v;
        else if (k == "scale")       scale = std::atoi(v.c_str());
        else if (k == "fullscreen")  fullscreen = (v == "1");
        else if (k == "audio")       audio = (v == "1");
        else if (k == "keyFile")     keyFile = v;
    }
    std::fclose(f);
    if (hostPort <= 0 || hostPort > 65535)       hostPort = 7777;
    if (connectPort <= 0 || connectPort > 65535) connectPort = 7777;
    if (relayPort <= 0 || relayPort > 65535)     relayPort = 7777;
    if (delay < 0) delay = 0;
    if (delay > 30) delay = 30;
    if (room.empty()) room = "7777";
    return true;
}

bool LaunchConfig::validate(std::string* why) const {
    if (rom.empty()) { if (why) *why = "还没有选择 ROM"; return false; }
    if (!fileExists(rom)) { if (why) *why = "ROM 文件不存在：\n" + rom; return false; }
    if (!isNesFile(rom)) { if (why) *why = "只支持 .nes 文件"; return false; }
    if (net != Net::Solo) {
        if (relay) {
            if (relayIp.empty()) { if (why) *why = "中继地址不能为空"; return false; }
            if (room.empty()) { if (why) *why = "房间号不能为空"; return false; }
        } else if (net == Net::Client && hostIp.empty()) {
            if (why) *why = "主机地址不能为空";
            return false;
        }
    }
    return true;
}

// ================================================================ 主界面

namespace {

struct View {
    Ui*      u  = nullptr;
    State*   st = nullptr;
    Frame    in;
    double   dt = 0.016;
    LaunchConfig* cfg = nullptr;
    std::string keyConfigPath;

    Ui& U() const { return *u; }
    const Frame& F() const { return in; }
};

constexpr int kTopBarH    = 72;
constexpr int kBottomBarH = 84;
constexpr int kPad        = 24;

// ---------------------------------------------------------------- 顶栏
void drawTopBar(View& v) {
    Ui& u = v.U();
    const int W = u.width();

    // 品牌
    cartridgeIcon(u, SDL_Rect{ kPad, 22, 26, 30 }, theme::accent, rgba(10, 20, 32));
    u.text(Font::Title, "fc-emulator", kPad + 38, 20, theme::text);
    u.text(Font::Small, "FC/NES 模拟器 · 锁步联机", kPad + 38, 46, theme::textFaint);

    // 页签
    const std::vector<std::string> labels{ "游戏库", "联机", "设置" };
    const SDL_Rect tabs{ W - kPad - 330, 18, 330, 40 };
    const int hit = pillTab(u, v.F(), v.dt, tabs, labels, int(v.st->tab));
    if (hit >= 0) v.st->tab = Tab(hit);

    u.hline(kPad, kTopBarH - 8, W - kPad * 2, theme::line);
}

// ---------------------------------------------------------------- 底栏
void drawBottomBar(View& v, bool* wantStart) {
    Ui& u = v.U();
    const int W = u.width(), H = u.height();
    const int y = H - kBottomBarH;

    u.hline(0, y, W, theme::line);
    u.fill(SDL_Rect{ 0, y + 1, W, kBottomBarH - 1 }, rgba(12, 16, 22));

    LaunchConfig& cfg = *v.cfg;

    // 左侧：当前选择摘要
    const std::string romName = cfg.rom.empty() ? "未选择 ROM" : fileNameOf(cfg.rom);
    u.textVCenter(Font::Body, u.clip(Font::Body, romName, W - 480), kPad, SDL_Rect{ 0, y + 16, 0, 22 },
                  cfg.rom.empty() ? theme::textFaint : theme::text);

    std::string mode = "单机";
    if (cfg.net == LaunchConfig::Net::Host)        mode = cfg.relay ? "主机 · 经中继" : "主机 · 直连";
    else if (cfg.net == LaunchConfig::Net::Client) mode = cfg.relay ? "客机 · 经中继" : "客机 · 直连";
    std::string sub = "模式：" + mode;
    if (!cfg.loadState.empty()) sub += " · 启动时读档";
    u.text(Font::Small, sub, kPad, y + 44, theme::textFaint);

    // 右侧：主按钮
    std::string why;
    const bool ok = cfg.validate(&why);
    const SDL_Rect btn{ W - kPad - 220, y + 22, 220, 46 };
    if (button(u, v.F(), v.dt, "start", btn, "开始游戏", BtnStyle::Primary, ok)) *wantStart = true;

    if (!ok)
        u.textRight(Font::Small, why, W - kPad - 240, y + 38, theme::warn);
}

} // namespace

// ---------------------------------------------------------------- 库页

namespace {

void refreshEntries(State& st) {
    st.entries = listDir(st.browseDir);
    // 启动器只关心目录和 .nes。build 目录这类地方会混进 .png/.o/.fcstate，
    // 全列出来会把真正要找的 ROM 淹掉，所以这里直接过滤掉。
    st.entries.erase(std::remove_if(st.entries.begin(), st.entries.end(),
                                    [](const DirEntry& e) {
                                        return !e.isDir && !isNesFile(e.name);
                                    }),
                     st.entries.end());
    st.sel      = -1;
    st.scroll   = st.scrollTo = 0;
    st.info     = RomInfo{};
    st.infoFor.clear();
}

// 换目录 / 重新扫描后调用：把当前选中的 ROM 重新定位出来，省得用户再找一遍
void reloadDir(State& st, const LaunchConfig& cfg) {
    refreshEntries(st);
    const std::string target = cfg.rom.empty() ? std::string() : fileNameOf(cfg.rom);
    if (target.empty()) return;
    for (size_t i = 0; i < st.entries.size(); ++i) {
        if (!st.entries[i].isDir && st.entries[i].name == target) {
            st.sel = int(i);
            break;
        }
    }
}

void ensureInfo(State& st) {
    if (st.sel < 0 || size_t(st.sel) >= st.entries.size()) return;
    const DirEntry& e = st.entries[size_t(st.sel)];
    if (e.isDir) return;
    const std::string full = joinPath(st.browseDir, e.name);
    if (st.infoFor == full) return;
    st.info    = inspectRom(full);
    st.infoFor = full;
}

void fitScroll(View& v, int listH, int itemH) {
    State& st = *v.st;
    const int total = int(st.entries.size()) * itemH;
    const int maxScroll = std::max(0, total - listH + 8);
    st.scrollTo = std::max(0.0f, std::min(st.scrollTo, float(maxScroll)));
    st.scroll = Ui::approach(st.scroll, st.scrollTo, 18.0f, v.dt);
}

void drawLibrary(View& v, bool* wantStart) {
    Ui& u = v.U();
    State& st = *v.st;
    LaunchConfig& cfg = *v.cfg;
    const int W = u.width(), H = u.height();
    const int top = kTopBarH, bottom = H - kBottomBarH;

    const int listW = std::max(360, std::min(480, int(float(W) * 0.36f)));
    const SDL_Rect listCard{ kPad, top + 8, listW, bottom - top - 24 };
    const SDL_Rect detailCard{ kPad + listW + 18, top + 8,
                               W - kPad * 2 - listW - 18, bottom - top - 24 };

    // ---------------- 左：文件浏览器 ----------------
    cardShell(u, listCard, false);
    u.round(SDL_Rect{ listCard.x + 1, listCard.y + 1, listCard.w - 2, listCard.h - 2 },
            radius::Card, theme::surface);

    // 路径行
    const SDL_Rect pathRow{ listCard.x + 14, listCard.y + 14, listCard.w - 28, 34 };
    {
        const SDL_Rect upBtn{ pathRow.x, pathRow.y, 34, 34 };
        if (button(u, v.F(), v.dt, "up", upBtn, "↑", BtnStyle::Subtle)) {
            const std::string p = parentPath(st.browseDir);
            if (!p.empty() && p != st.browseDir) {
                st.browseDir = p;
                reloadDir(st, cfg);
            }
        }
        u.textVCenter(Font::Small, u.clip(Font::Small, st.browseDir, pathRow.w - 46),
                      pathRow.x + 42, pathRow, theme::textDim);
    }

    // 列表
    const int itemH = 58;
    const SDL_Rect listArea{ listCard.x + 8, pathRow.y + pathRow.h + 10,
                             listCard.w - 16, listCard.h - (pathRow.y + pathRow.h + 10 - listCard.y) - 14 };
    fitScroll(v, listArea.h, itemH);

    SDL_RenderSetClipRect(u.ren(), &listArea);
    const int first = std::max(0, int(st.scroll / float(itemH)));
    const int last  = std::min(int(st.entries.size()), first + listArea.h / itemH + 2);

    if (st.entries.empty()) {
        u.textCenter(Font::Small, "这个目录里没有可显示的内容",
                     SDL_Rect{ listArea.x, listArea.y + listArea.h / 2 - 20, listArea.w, 20 },
                     theme::textFaint);
        u.textCenter(Font::Small, "把 .nes 文件拖进窗口也可以直接载入",
                     SDL_Rect{ listArea.x, listArea.y + listArea.h / 2 + 2, listArea.w, 20 },
                     theme::textFaint);
    }

    for (int i = first; i < last; ++i) {
        const DirEntry& e = st.entries[size_t(i)];
        const SDL_Rect r{ listArea.x + 2, listArea.y + int(float(i) * itemH - st.scroll) + 2,
                          listArea.w - 4, itemH - 4 };
        const bool on  = (i == st.sel);
        const bool hot = v.F().inside(r);
        const float k  = animStep("row:" + st.browseDir + ":" + e.name, hot, v.dt);
        const float onK = animStep("rowsel:" + st.browseDir + ":" + e.name, on, v.dt);

        RGBA bg = mix(rgba(0, 0, 0, 0), fade(theme::text, 0.05f), k);
        bg = mix(bg, fade(theme::accent, 0.16f), onK);
        if (bg.a > 2) u.round(r, radius::Button, bg);
        if (onK > 0.02f) u.roundOutline(r, radius::Button, 1, fade(theme::accent, 0.6f * onK));

        // 图标
        if (e.isDir) {
            const SDL_Rect fr{ r.x + 14, r.y + 18, 22, 16 };
            u.round(SDL_Rect{ fr.x, fr.y - 5, 10, 7 }, 2, theme::warn);
            u.round(fr, 4, theme::warn);
        } else {
            const bool nes = isNesFile(e.name);
            cartridgeIcon(u, SDL_Rect{ r.x + 14, r.y + 13, 24, 30 },
                          nes ? theme::accent : theme::border,
                          nes ? rgba(10, 20, 32) : theme::surface);
        }

        const int tx = r.x + 52;
        const int maxW = r.w - 52 - 12;
        u.text(Font::Body, u.clip(Font::Body, e.isDir ? e.name : stripExt(e.name), maxW),
               tx, r.y + 10, on ? theme::text : mix(theme::textDim, theme::text, k));

        std::string sub = e.isDir ? "文件夹" : humanSize(e.size);
        if (!e.isDir && !isNesFile(e.name)) sub += " · 非 .nes";
        if (!e.isDir && isNesFile(e.name) && fileExists(joinPath(st.browseDir, e.name) + ".fcstate"))
            sub += " · 有存档";
        u.text(Font::Small, u.clip(Font::Small, sub, maxW), tx, r.y + 32,
               on ? theme::textDim : theme::textFaint);

        // 交互
        if (hot && v.F().pressed) {
            const double now = double(SDL_GetTicks());
            const bool dbl = (now - st.lastClickT < 420.0) && std::fabs(v.F().my - st.lastClickY) < 6.0f;
            st.lastClickT = now;
            st.lastClickY = v.F().my;

            if (e.isDir) {
                st.browseDir = joinPath(st.browseDir, e.name);
                reloadDir(st, cfg);
            } else if (isNesFile(e.name)) {
                st.sel = i;
                v.cfg->rom = joinPath(st.browseDir, e.name);
                ensureInfo(st);
                if (dbl) *wantStart = true;
            } else {
                toast(st, "只能载入 .nes 文件", true);
            }
        }
    }
    SDL_RenderSetClipRect(u.ren(), nullptr);

    // 滚动条
    if (int(st.entries.size()) * itemH > listArea.h) {
        const float visible = float(listArea.h) / float(int(st.entries.size()) * itemH);
        const float total   = float(int(st.entries.size()) * itemH);
        const int barH = std::max(28, int(float(listArea.h) * visible));
        const int barY = listArea.y + int((float(listArea.h) - barH) * (st.scroll / std::max(1.0f, total - listArea.h)));
        u.round(SDL_Rect{ listArea.x + listArea.w - 6, barY, 4, barH }, 2, theme::borderHi);
    }

    // ---------------- 右：详情 ----------------
    cardShell(u, detailCard, true);

    const bool hasRom = !cfg.rom.empty() && fileExists(cfg.rom);

    if (!hasRom) {
        u.textCenter(Font::Title, "选择左侧的 ROM 开始",
                     SDL_Rect{ detailCard.x, detailCard.y + detailCard.h / 2 - 60, detailCard.w, 30 },
                     theme::textDim);
        u.textCenter(Font::Small, "支持 iNES / NES2.0 格式的 .nes 文件",
                     SDL_Rect{ detailCard.x, detailCard.y + detailCard.h / 2 - 24, detailCard.w, 20 },
                     theme::textFaint);
        u.textCenter(Font::Small, "Mapper 支持：NROM · MMC1 · UxROM · CNROM · BMC-68in1",
                     SDL_Rect{ detailCard.x, detailCard.y + detailCard.h / 2 + 4, detailCard.w, 20 },
                     theme::textFaint);
        return;
    }

    ensureInfo(st);
    const RomInfo& info = st.info;
    const int cx = detailCard.x + 28;
    int y = detailCard.y + 28;

    cartridgeIcon(u, SDL_Rect{ cx, y, 62, 76 }, theme::accent, rgba(10, 20, 32));

    const int ttx = cx + 82;
    const int tmaxW = detailCard.w - 28 - 82 - 20;
    u.text(Font::Title, u.clip(Font::Title, stripExt(fileNameOf(cfg.rom)), tmaxW), ttx, y + 6, theme::text);
    u.text(Font::Small, "iNES / NES2.0", ttx, y + 36, theme::textFaint);

    // 状态标签
    if (!info.valid)          chip(u, ttx, y + 58, "文件头异常", theme::err, fade(theme::err, 0.16f));
    else if (info.mapperName == std::string("不支持"))
                              chip(u, ttx, y + 58, "Mapper 不支持", theme::err, fade(theme::err, 0.16f));
    else                      chip(u, ttx, y + 58, "可运行", theme::ok, fade(theme::ok, 0.16f));
    if (info.hasState)        chip(u, ttx + 82, y + 58, "有存档", theme::warn, fade(theme::warn, 0.16f));

    y += 108;
    u.hline(cx, y, detailCard.w - 56, theme::line);
    y += 18;

    // 属性表
    struct Row { const char* k; std::string v; };
    std::vector<Row> rows;
    rows.push_back({ "Mapper",  std::string(info.mapperName) + " (" + std::to_string(info.mapper) + ")" });
    rows.push_back({ "PRG ROM", std::to_string(info.prgKb) + " KB" });
    rows.push_back({ "CHR ROM", info.chrKb > 0 ? std::to_string(info.chrKb) + " KB" : "使用 CHR RAM" });
    rows.push_back({ "镜像方式", mirrorLabel(info.mirror) });
    rows.push_back({ "文件大小", humanSize(info.bytes) });
    rows.push_back({ "电池存档", info.battery ? "有" : "无" });

    for (const Row& r : rows) {
        u.text(Font::Small, r.k, cx, y, theme::textFaint);
        u.text(Font::Body, r.v, cx + 110, y - 2, theme::text);
        y += 30;
    }

    // 存档开关
    y += 6;
    u.hline(cx, y, detailCard.w - 56, theme::line);
    y += 14;

    if (info.hasState) {
        const bool on = (cfg.loadState == info.statePath);
        const SDL_Rect rowRect{ cx - 8, y, detailCard.w - 56 + 16, 56 };
        if (switchRow(u, v.F(), v.dt, "loadstate:" + info.statePath, rowRect,
                      "启动时载入存档", fileNameOf(info.statePath), on)) {
            cfg.loadState = on ? std::string() : info.statePath;
            toast(st, on ? "已取消读档，将从开机状态起跑" : "启动时会从这个存档继续");
        }
    } else {
        u.text(Font::Small, "尚无存档", cx, y, theme::textFaint);
        u.text(Font::Small, "游戏中按 F5 存档，文件名与 ROM 同名（.fcstate）",
               cx, y + 22, theme::textFaint);
    }

    // 路径
    u.text(Font::Small, u.clip(Font::Small, cfg.rom, detailCard.w - 56),
           cx, detailCard.y + detailCard.h - 34, theme::textFaint);
}

} // namespace

// ---------------------------------------------------------------- 联机页

namespace {

void drawNetplay(View& v) {
    Ui& u = v.U();
    State& st = *v.st;
    LaunchConfig& cfg = *v.cfg;
    const int W = u.width();
    int top = kTopBarH + 8;

    const int colW = std::min(860, W - kPad * 2);
    const int x0 = (W - colW) / 2;

    // ---- 卡片 1：对局模式
    SDL_Rect c1{ x0, top, colW, 176 };
    cardShell(u, c1, true);
    cardTitle(u, c1, "对局模式", "本机一个人玩，还是和另一台机器对着玩");

    const int cw = (colW - 40 - 2 * 12) / 3;
    struct ModeOpt { const char* title; const char* desc; LaunchConfig::Net m; int icon; };
    const ModeOpt opts[3] = {
        { "单机",     "自己玩 1P / 2P",       LaunchConfig::Net::Solo,   0 },
        { "作为主机", "开房等对方连进来",     LaunchConfig::Net::Host,   1 },
        { "连接主机", "连到对方开的房间",     LaunchConfig::Net::Client, 2 },
    };
    for (int i = 0; i < 3; ++i) {
        const SDL_Rect r{ c1.x + 20 + i * (cw + 12), c1.y + 62, cw, 96 };
        if (choiceCard(u, v.F(), v.dt, std::string("mode") + char('0' + i), r,
                       opts[i].title, opts[i].desc, cfg.net == opts[i].m, opts[i].icon)) {
            cfg.net = opts[i].m;
            resetAnimSteps();
        }
    }
    top = c1.y + c1.h + 14;

    // ---- 卡片 2：连接方式与地址
    SDL_Rect c2{ x0, top, colW, cfg.net == LaunchConfig::Net::Solo ? 96 : 214 };
    cardShell(u, c2, true);
    cardTitle(u, c2, "连接方式", cfg.net == LaunchConfig::Net::Solo ? "单机模式无需设置" : "");

    if (cfg.net == LaunchConfig::Net::Solo) {
        u.text(Font::Small, "选择「作为主机」或「连接主机」后，这里会出现地址与端口设置。",
               c2.x + 20, c2.y + 58, theme::textFaint);
    } else {
        // 直连 / 中继 两个 pill
        const SDL_Rect seg{ c2.x + 20, c2.y + 58, 300, 36 };
        const std::vector<std::string> segs{ "直连", "中继" };
        const int segHit = pillTab(u, v.F(), v.dt, seg, segs, cfg.relay ? 1 : 0);
        if (segHit >= 0) cfg.relay = (segHit == 1);
        chip(u, seg.x + seg.w + 14, c2.y + 65,
             cfg.relay ? "双方都不需要公网 IP" : "主机需要有公网 IP 或做端口映射",
             theme::textFaint, theme::surface);

        // 输入行
        const int iy = c2.y + 108;
        const int ih = 40;
        int ix = c2.x + 20;

        if (!cfg.relay) {
            if (cfg.net == LaunchConfig::Net::Client) {
                u.text(Font::Small, "主机地址", ix, iy - 20, theme::textFaint);
                const SDL_Rect r{ ix, iy, 300, ih };
                textField(u, v.F(), st.fHostIp, r, "192.168.1.100", false);
                ix += 316;

                u.text(Font::Small, "端口", ix, iy - 20, theme::textFaint);
                const SDL_Rect r2{ ix, iy, 120, ih };
                textField(u, v.F(), st.fConnectPort, r2, "7777", true);
            } else {
                u.text(Font::Small, "本机监听端口", ix, iy - 20, theme::textFaint);
                const SDL_Rect r{ ix, iy, 160, ih };
                textField(u, v.F(), st.fHostPort, r, "7777", true);
                ix += 176;
                u.textVCenter(Font::Small, "对方在你的「主机地址」里填本机 IP", ix, SDL_Rect{ 0, iy, 0, ih },
                              theme::textFaint);
            }
        } else {
            u.text(Font::Small, "中继服务器地址", ix, iy - 20, theme::textFaint);
            const SDL_Rect r{ ix, iy, 300, ih };
            textField(u, v.F(), st.fRelayIp, r, "relay.example.com", false);
            ix += 316;

            u.text(Font::Small, "中继端口", ix, iy - 20, theme::textFaint);
            textField(u, v.F(), st.fRelayPort, SDL_Rect{ ix, iy, 100, ih }, "7777", true);
            ix += 116;

            u.text(Font::Small, "房间号", ix, iy - 20, theme::textFaint);
            textField(u, v.F(), st.fRoom, SDL_Rect{ ix, iy, 120, ih }, "7777", true);
        }

        // 同步到 cfg
        auto syncInt = [](const TextField& tf, int def) {
            if (tf.text.empty()) return def;
            const int v2 = std::atoi(tf.text.c_str());
            return (v2 <= 0 || v2 > 65535) ? def : v2;
        };
        cfg.hostIp      = st.fHostIp.text.empty() ? "127.0.0.1" : st.fHostIp.text;
        cfg.hostPort    = syncInt(st.fHostPort, 7777);
        cfg.connectPort = syncInt(st.fConnectPort, 7777);
        cfg.relayIp     = st.fRelayIp.text.empty() ? "127.0.0.1" : st.fRelayIp.text;
        cfg.relayPort   = syncInt(st.fRelayPort, 7777);
        cfg.room        = st.fRoom.text.empty() ? "7777" : st.fRoom.text;
    }
    top = c2.y + c2.h + 14;

    // ---- 卡片 3：同步参数
    SDL_Rect c3{ x0, top, colW, cfg.net == LaunchConfig::Net::Solo ? 96 : 132 };
    cardShell(u, c3, true);
    cardTitle(u, c3, "同步参数");

    if (cfg.net == LaunchConfig::Net::Solo) {
        u.text(Font::Small, "输入延迟只在联机时生效，单机可以不管。", c3.x + 20, c3.y + 58, theme::textFaint);
    } else {
        u.text(Font::Small, "输入延迟", c3.x + 20, c3.y + 56, theme::textFaint);
        u.text(Font::Body, std::to_string(cfg.delay) + " 帧", c3.x + 20, c3.y + 78, theme::text);

        const SDL_Rect track{ c3.x + 110, c3.y + 88, colW - 260, 6 };
        u.round(track, 3, theme::line);
        SDL_Rect fill = track;
        fill.w = int(float(track.w) * float(cfg.delay) / 30.0f);
        u.round(fill, 3, theme::accent);

        // 拖动：按下时命中就接管，直到松开为止（这样拖出滑杆范围也不会断）
        const SDL_Rect hit{ track.x - 12, track.y - 16, track.w + 24, 40 };
        if (v.F().pressed && v.F().inside(hit)) st.draggingDelay = true;
        if (!v.F().down) st.draggingDelay = false;
        if (st.draggingDelay && v.F().down) {
            const float t = float(v.F().mx - float(track.x)) / float(std::max(1, track.w));
            cfg.delay = std::max(0, std::min(30, int(t * 30.0f + 0.5f)));
        }
        const int hx = track.x + int(float(track.w) * float(cfg.delay) / 30.0f) - 9;
        u.shadow(SDL_Rect{ hx, track.y - 6, 18, 18 }, 9, 6, 160);
        u.round(SDL_Rect{ hx, track.y - 6, 18, 18 }, 9, theme::text);

        u.text(Font::Small, "越大越抗网络抖动，但操作越迟钝。局域网友好值 2–4，公网 6–10。",
               c3.x + 20, c3.y + 104, theme::textFaint);
    }
    top = c3.y + c3.h + 14;

    // ---- 提示条
    std::string tip;
    RGBA tipColor = theme::textFaint;
    if (cfg.net == LaunchConfig::Net::Host && !cfg.relay) {
        tip = "直连做主机会在 UDP " + std::to_string(cfg.hostPort) +
              " 上等对方。Windows 首次运行会弹防火墙授权，需要允许「专用网络」。公网互联还要在路由器上把这个端口映射到本机。";
        tipColor = theme::warn;
    } else if (cfg.net == LaunchConfig::Net::Client && !cfg.relay) {
        tip = "客机不占端口，只要能主动连上主机即可。主机没放行端口时，症状是「正在连接…」一直转圈。";
        tipColor = theme::warn;
    } else if (cfg.relay) {
        tip = "中继模式下本地不需要放行任何端口。两端必须填完全一样的「中继地址 + 端口 + 房间号」，"
              "否则会各自等在自己房间里。";
        tipColor = theme::warn;
    } else {
        tip = "单机模式下 F2 可以改键位，F5/F8 存档读档，Tab 按住加速。";
    }
    if (!tip.empty()) {
        const auto lines = wrapText(u, Font::Small, tip, colW - 76);
        const int h = 26 + int(lines.size()) * 22;
        const SDL_Rect box{ x0, top, colW, h };
        u.round(box, radius::Panel, fade(tipColor, 0.09f));
        u.roundOutline(box, radius::Panel, 1, fade(tipColor, 0.32f));
        // 灯泡图标
        u.ring(box.x + 26, box.y + 24, 8, 2, tipColor);
        u.fill(box.x + 22, box.y + 33, 9, 2, tipColor);
        int ly = box.y + 14;
        for (const std::string& l : lines) {
            u.text(Font::Small, l, box.x + 48, ly, tipColor);
            ly += 22;
        }
    }
}

} // namespace

// ---------------------------------------------------------------- 设置页

namespace {

void drawSettings(View& v, SDL_Window* win, SDL_Renderer* ren, const std::string& keyConfigPath,
                  bool* wantKeyEditor) {
    Ui& u = v.U();
    State& st = *v.st;
    LaunchConfig& cfg = *v.cfg;
    const int W = u.width(), H = u.height();
    const int top = kTopBarH + 8;
    const int viewH = std::max(120, H - kBottomBarH - top - 8);

    const int colW = std::min(860, W - kPad * 2);
    const int x0 = (W - colW) / 2;
    const int gap = 12;

    // 先把总高算出来：窗口矮的时候（比如拉到最小尺寸）内容会超出一屏，
    // 那就得能滚 —— 卡在底栏后面看不见是最尴尬的。
    const int hDisplay = 168, hAudio = 100, hKeys = 110, hAbout = 104;
    const int contentH = hDisplay + gap + hAudio + gap + hKeys + gap + hAbout;
    const float maxScroll = std::max(0.0f, float(contentH - viewH + 8));
    st.settingsScroll = std::max(0.0f, std::min(st.settingsScroll, maxScroll));

    const SDL_Rect clip{ 0, top, W, viewH };
    SDL_RenderSetClipRect(ren, &clip);

    int y = top - int(st.settingsScroll);

    // ---- 显示
    {
        const SDL_Rect c{ x0, y, colW, hDisplay };
        cardShell(u, c, true);
        u.text(Font::Title, "显示", c.x + 20, c.y + 14, theme::text);

        u.text(Font::Small, "画面缩放", c.x + 20, c.y + 52, theme::textFaint);
        const std::vector<std::string> scales{ "自动", "1x", "2x", "3x", "4x" };
        const SDL_Rect seg{ c.x + 20, c.y + 74, 360, 34 };
        const int scaleHit = pillTab(u, v.F(), v.dt, seg, scales, cfg.scale == 0 ? 0 : cfg.scale);
        if (scaleHit >= 0) cfg.scale = scaleHit;
        u.text(Font::Small, cfg.scale == 0 ? "按窗口自动取最大整数倍，像素不会糊"
                                           : "固定倍数，画面居中，四周留黑边",
               seg.x + seg.w + 14, c.y + 82, theme::textFaint);

        const SDL_Rect row{ c.x + 12, c.y + 114, colW - 24, 46 };
        if (switchRow(u, v.F(), v.dt, "fs", row, "全屏启动", "下次启动时直接进全屏", cfg.fullscreen))
            cfg.fullscreen = !cfg.fullscreen;
        y = c.y + c.h + gap;
    }

    // ---- 音频
    {
        const SDL_Rect c{ x0, y, colW, hAudio };
        cardShell(u, c, true);
        u.text(Font::Title, "音频", c.x + 20, c.y + 14, theme::text);
        const SDL_Rect row{ c.x + 12, c.y + 46, colW - 24, 46 };
        if (switchRow(u, v.F(), v.dt, "au", row, "启用音频", "关闭后静音运行，模拟速度不受影响", cfg.audio))
            cfg.audio = !cfg.audio;
        y = c.y + c.h + gap;
    }

    // ---- 按键
    {
        const SDL_Rect c{ x0, y, colW, hKeys };
        cardShell(u, c, true);
        u.text(Font::Title, "按键", c.x + 20, c.y + 14, theme::text);

        u.text(Font::Small, "P1 默认方向 WASD、A=J、B=K；P2 默认方向键、A=Z、B=X。",
               c.x + 20, c.y + 48, theme::textFaint);
        u.text(Font::Mono, u.clip(Font::Mono, keyConfigPath, colW - 220),
               c.x + 20, c.y + 72, theme::textFaint);

        const SDL_Rect btn{ c.x + colW - 176, c.y + 60, 156, 40 };
        if (button(u, v.F(), v.dt, "keys", btn, "自定义键位", BtnStyle::Normal)) *wantKeyEditor = true;
        y = c.y + c.h + gap;
    }

    // ---- 关于
    {
        const SDL_Rect c{ x0, y, colW, hAbout };
        cardShell(u, c, true);
        u.text(Font::Title, "关于", c.x + 20, c.y + 14, theme::text);

        SDL_version lk{};
        SDL_GetVersion(&lk);
        char sdl[64];
        std::snprintf(sdl, sizeof(sdl), "SDL %d.%d.%d / SDL2_ttf 2.24.0",
                      lk.major, lk.minor, lk.patch);

        u.text(Font::Small, "渲染字体", c.x + 20, c.y + 52, theme::textFaint);
        u.text(Font::Mono, u.clip(Font::Mono, u.fontPath(), colW - 170),
               c.x + 110, c.y + 48, theme::textDim);
        u.text(Font::Small, "运行时", c.x + 20, c.y + 78, theme::textFaint);
        u.text(Font::Mono, sdl, c.x + 110, c.y + 74, theme::textDim);
        (void)win;
    }

    SDL_RenderSetClipRect(ren, nullptr);

    // ---- 滚动条（只有超高时才出现）
    if (maxScroll > 0.5f) {
        const float visible = float(viewH) / float(contentH);
        const int barH = std::max(30, int(float(viewH) * visible));
        const int barY = top + int((float(viewH) - barH) * (st.settingsScroll / maxScroll));
        u.round(SDL_Rect{ W - 10, barY, 4, barH }, 2, theme::borderHi);
    }
}

} // namespace

// ================================================================ 入口

bool runLauncher(SDL_Window* win, SDL_Renderer* ren, Ui& ui,
                 LaunchConfig& cfg, const std::string& keyConfigPath) {
    if (!ui.fontsOk()) {
        std::fprintf(stderr, "启动器无法运行：%s\n", ui.diag().c_str());
        return false;
    }

    State st;
    st.fHostIp.text      = cfg.hostIp;
    st.fHostIp.cursor    = st.fHostIp.text.size();
    st.fHostPort.text    = std::to_string(cfg.hostPort);
    st.fHostPort.cursor  = st.fHostPort.text.size();
    st.fHostPort.numeric = true;
    st.fConnectPort.text    = std::to_string(cfg.connectPort);
    st.fConnectPort.cursor  = st.fConnectPort.text.size();
    st.fConnectPort.numeric = true;
    st.fRelayIp.text     = cfg.relayIp;
    st.fRelayIp.cursor   = st.fRelayIp.text.size();
    st.fRelayPort.text    = std::to_string(cfg.relayPort);
    st.fRelayPort.cursor  = st.fRelayPort.text.size();
    st.fRelayPort.numeric = true;
    st.fRoom.text        = cfg.room;
    st.fRoom.cursor      = st.fRoom.text.size();

    // 初始目录：优先上次的；它不存在（换机器/删了）就退到 exe 目录。
    // ROM 通常就跟 exe 放一起，这个默认值命中率最高。
    if (cfg.browseDir.empty() || !isDirectory(cfg.browseDir)) cfg.browseDir = exeDir();
    st.browseDir  = cfg.browseDir;
    cfg.browseDir = st.browseDir;
    reloadDir(st, cfg);

    bool running = true;
    bool wantStart = false;
    bool wantKeyEditor = false;
    bool wantQuit = false;

    SDL_StartTextInput();

    u64 prev = SDL_GetPerformanceCounter();
    const u64 freq = SDL_GetPerformanceFrequency();

    while (running && !wantStart && !wantQuit) {
        // ---------------- 计时
        const u64 now = SDL_GetPerformanceCounter();
        double dt = double(now - prev) / double(freq);
        prev = now;
        if (dt > 0.1) dt = 0.1;      // 窗口被拖动/断点后不要跳变
        ui.tick(dt);

        // ---------------- 输入快照
        Frame in;
        int mw = 0, mh = 0;
        Uint32 mstate = SDL_GetMouseState(&mw, &mh);
        int ow = 0, oh = 0, ww = 0, wh = 0;
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        SDL_GetWindowSize(win, &ww, &wh);
        // 高 DPI 下渲染坐标与窗口坐标不是 1:1，转一下鼠标位置
        const float dpiK = (ww > 0) ? float(ow) / float(ww) : 1.0f;
        in.mx = float(mw) * dpiK;
        in.my = float(mh) * dpiK;
        in.down = (mstate & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;

        // ---------------- 事件
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT:
                    wantQuit = true;
                    break;

                case SDL_WINDOWEVENT:
                    if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                        e.window.event == SDL_WINDOWEVENT_RESIZED) {
                        int nw = 0, nh = 0;
                        SDL_GetRendererOutputSize(ren, &nw, &nh);
                        ui.setViewport(nw, nh);
                        resetAnimSteps();
                    }
                    break;

                case SDL_MOUSEMOTION:
                    in.mx = float(e.motion.x) * dpiK;
                    in.my = float(e.motion.y) * dpiK;
                    break;

                case SDL_MOUSEBUTTONDOWN:
                    if (e.button.button == SDL_BUTTON_LEFT) {
                        in.pressed = true;
                        in.mx = float(e.button.x) * dpiK;
                        in.my = float(e.button.y) * dpiK;
                    }
                    break;

                case SDL_MOUSEWHEEL:
                    in.wheel += float(e.wheel.y);
                    break;

                case SDL_TEXTINPUT:
                    textFieldInput(st.fHostIp, e);
                    textFieldInput(st.fHostPort, e);
                    textFieldInput(st.fConnectPort, e);
                    textFieldInput(st.fRelayIp, e);
                    textFieldInput(st.fRelayPort, e);
                    textFieldInput(st.fRoom, e);
                    break;

                case SDL_DROPFILE: {
                    const std::string dropped = e.drop.file ? e.drop.file : "";
                    if (e.drop.file) SDL_free(e.drop.file);
                    if (!dropped.empty()) {
                        if (isDirectory(dropped)) {
                            st.browseDir  = dropped;
                            cfg.browseDir = dropped;
                            reloadDir(st, cfg);
                        } else if (isNesFile(dropped)) {
                            cfg.rom       = dropped;
                            st.browseDir  = parentPath(dropped);
                            cfg.browseDir = st.browseDir;
                            reloadDir(st, cfg);
                            const std::string nm = fileNameOf(dropped);
                            for (size_t i = 0; i < st.entries.size(); ++i)
                                if (!st.entries[i].isDir && st.entries[i].name == nm) { st.sel = int(i); break; }
                            st.tab = Tab::Library;
                            toast(st, "已载入 " + stripExt(nm));
                        } else {
                            toast(st, "拖入的不是 .nes 文件", true);
                        }
                    }
                    break;
                }

                case SDL_KEYDOWN: {
                    const SDL_Keycode k = e.key.keysym.sym;
                    const bool ctrl = (e.key.keysym.mod & KMOD_CTRL) != 0;
                    // 文本框优先吃键盘
                    bool handled = false;
                    TextField* tfs[6] = { &st.fHostIp, &st.fHostPort, &st.fConnectPort,
                                          &st.fRelayIp, &st.fRelayPort, &st.fRoom };
                    for (TextField* tf : tfs) {
                        if (tf->focused) {
                            if (k == SDLK_ESCAPE) { tf->focused = false; handled = true; break; }
                            if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_TAB) {
                                tf->focused = false; handled = true; break;
                            }
                            textFieldInput(*tf, e);
                            handled = true;
                            break;
                        }
                    }
                    if (handled) break;

                    switch (k) {
                        case SDLK_ESCAPE:
                            wantQuit = true;
                            break;
                        case SDLK_RETURN: case SDLK_KP_ENTER: {
                            std::string w;
                            if (cfg.validate(&w)) wantStart = true;
                            else                  toast(st, w, true);
                            break;
                        }
                        case SDLK_TAB:
                            st.tab = Tab((int(st.tab) + 1) % 3);
                            break;
                        // Ctrl+1/2/3 与 F5/F6/F7 都能直达页签（后者给不看修饰键的人用）
                        case SDLK_1: case SDLK_F5:
                            if (k == SDLK_F5 || ctrl) st.tab = Tab::Library;
                            break;
                        case SDLK_2: case SDLK_F6:
                            if (k == SDLK_F6 || ctrl) st.tab = Tab::Netplay;
                            break;
                        case SDLK_3: case SDLK_F7:
                            if (k == SDLK_F7 || ctrl) st.tab = Tab::Settings;
                            break;
                        case SDLK_UP:
                            if (!st.entries.empty()) {
                                st.sel = st.sel <= 0 ? 0 : st.sel - 1;
                                st.scrollTo = std::max(0.0f, st.scrollTo - 58.0f);
                            }
                            break;
                        case SDLK_DOWN:
                            if (!st.entries.empty()) {
                                st.sel = std::min(int(st.entries.size()) - 1,
                                                  st.sel < 0 ? 0 : st.sel + 1);
                                st.scrollTo += 58.0f;
                            }
                            break;
                        default:
                            break;
                    }
                    break;
                }

                default:
                    break;
            }
        }

        // ---------------- 更新
        {
            const float wheel = in.wheel;
            if (wheel != 0.0f) {
                if (st.tab == Tab::Library) {
                    st.scrollTo -= wheel * 78.0f;
                    if (st.scrollTo < 0) st.scrollTo = 0;
                } else if (st.tab == Tab::Settings) {
                    st.settingsScroll -= wheel * 78.0f;
                    if (st.settingsScroll < 0) st.settingsScroll = 0;
                }
            }
            // 光标闪烁推进
            st.fHostIp.caretT += dt;
            st.fHostPort.caretT += dt;
            st.fConnectPort.caretT += dt;
            st.fRelayIp.caretT += dt;
            st.fRelayPort.caretT += dt;
            st.fRoom.caretT += dt;
        }

        // ---------------- 绘制
        ui.clear(theme::bg);
        ui.gradientV(SDL_Rect{ 0, ui.height() - 280, ui.width(), 280 },
                     rgba(8, 11, 16, 0), theme::bgGlow);

        View v;
        v.u = &ui;
        v.st = &st;
        v.in = in;
        v.dt = dt;
        v.cfg = &cfg;
        v.keyConfigPath = keyConfigPath;

        drawTopBar(v);

        switch (st.tab) {
            case Tab::Library:  drawLibrary(v, &wantStart); break;
            case Tab::Netplay:  drawNetplay(v); break;
            case Tab::Settings: drawSettings(v, win, ren, keyConfigPath, &wantKeyEditor); break;
        }

        drawBottomBar(v, &wantStart);

        // toast
        if (!st.toast.empty() && double(SDL_GetTicks()) < st.toastUntil) {
            const int tw = ui.measure(Font::Body, st.toast);
            const SDL_Rect box{ (ui.width() - tw) / 2 - 22, ui.height() - kBottomBarH - 62, tw + 44, 42 };
            ui.shadow(box, radius::Button, 12, 170);
            ui.round(box, radius::Button, st.toastWarn ? mix(theme::surfaceHi, theme::warn, 0.18f)
                                                       : theme::surfaceHi);
            ui.roundOutline(box, radius::Button, 1, st.toastWarn ? fade(theme::warn, 0.6f) : theme::border);
            ui.textCenter(Font::Body, st.toast, box, theme::text);
        }

        SDL_RenderPresent(ren);

        // 键位界面是阻塞式的：它自己接管 renderer 与事件循环，跑完再回到这里。
        // 放在主循环内处理而不是递归调用一次 runLauncher，可以省掉整套状态重建。
        if (wantKeyEditor) {
            wantKeyEditor = false;
            KeyMap km;
            km.setDefaults();
            km.load(keyConfigPath);
            bool kq = false;
            runKeyConfigScreen(win, ren, ui, keyConfigPath, km, &kq);
            resetAnimSteps();
            prev = SDL_GetPerformanceCounter();   // 别把在键位界面停留的时间算进下一帧 dt
            SDL_StartTextInput();
            if (kq) wantQuit = true;
        }

        SDL_Delay(1);
    }

    SDL_StopTextInput();

    if (!wantStart) return false;

    // 界面上的临时状态写回配置并落盘
    cfg.browseDir = st.browseDir;
    if (cfg.net == LaunchConfig::Net::Solo) cfg.loadState.clear();

    if (cfg.save(LaunchConfig::defaultPath()))
        std::printf("启动器配置已保存: %s\n", LaunchConfig::defaultPath().c_str());

    return true;
}

// ================================================================ 离屏预览
// 只给 --shot 用：造一份临时状态，画一帧静态画面。
// 有了它，「圆角对不对、字有没有溢出、对齐歪没歪」这类问题就不必每次都跑起来肉眼盯。
void drawLauncherPreview(ui::Ui& ui, const LaunchConfig& cfg, int tab) {
    State st;
    st.tab = Tab(std::max(0, std::min(2, tab)));
    st.browseDir = cfg.browseDir.empty() ? exeDir() : cfg.browseDir;
    if (!isDirectory(st.browseDir)) st.browseDir = exeDir();

    LaunchConfig copy = cfg;   // 绘制过程会写 cfg（文本框回写等），用副本免得污染调用方
    reloadDir(st, copy);

    // 挑一个真实存在的 .nes 进详情栏，截图才有内容可看
    for (size_t i = 0; i < st.entries.size(); ++i) {
        if (!st.entries[i].isDir && isNesFile(st.entries[i].name)) {
            st.sel   = int(i);
            copy.rom = joinPath(st.browseDir, st.entries[i].name);
            break;
        }
    }
    ensureInfo(st);

    View v;
    v.u   = &ui;
    v.st  = &st;
    v.dt  = 0.016;
    v.cfg = &copy;
    v.keyConfigPath = defaultKeyConfigPath();
    v.in.mx = -1000.0f;        // 鼠标放到屏幕外，避免冒出莫名其妙的 hover 态
    v.in.my = -1000.0f;

    ui.clear(theme::bg);
    ui.gradientV(SDL_Rect{ 0, ui.height() - 280, ui.width(), 280 },
                 rgba(8, 11, 16, 0), theme::bgGlow);

    drawTopBar(v);
    bool dummy = false;
    switch (st.tab) {
        case Tab::Library:  drawLibrary(v, &dummy); break;
        case Tab::Netplay:  drawNetplay(v); break;
        case Tab::Settings: drawSettings(v, nullptr, ui.ren(), v.keyConfigPath, &dummy); break;
    }
    drawBottomBar(v, &dummy);
}

} // namespace fc
