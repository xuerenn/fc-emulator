#include "fs_utf8.h"

#include <cwchar>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX      // 别让 windows.h 带出 min/max 宏，污染 std::min/std::max
#  endif
#  include <windows.h>
#endif

namespace fc {

#ifdef _WIN32
namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n);
    return w;
}

// mode 只会是 "rb" / "wb" / "r+b" 这类 ASCII，逐字节撑宽即可，不必过代码页
std::wstring widenAscii(const char* s) {
    std::wstring w;
    for (; *s; ++s) w.push_back(wchar_t((unsigned char)*s));
    return w;
}

} // namespace
#endif

std::string ansiToUtf8(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return s;
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), int(s.size()), nullptr, 0);
    if (n <= 0) return s;                 // 转不动就原样返回，别把参数弄丢
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), int(s.size()), &w[0], n);
    const int m = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), n, nullptr, 0, nullptr, nullptr);
    if (m <= 0) return s;
    std::string out(size_t(m), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), n, &out[0], m, nullptr, nullptr);
    return out;
#else
    return s;
#endif
}

std::FILE* fopenUtf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    const std::wstring wpath = utf8ToWide(path);
    if (wpath.empty()) return nullptr;
    return ::_wfopen(wpath.c_str(), widenAscii(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

int removeUtf8(const std::string& path) {
#ifdef _WIN32
    const std::wstring wpath = utf8ToWide(path);
    if (wpath.empty()) return -1;
    return ::_wremove(wpath.c_str());
#else
    return std::remove(path.c_str());
#endif
}

int renameUtf8(const std::string& from, const std::string& to) {
#ifdef _WIN32
    const std::wstring wfrom = utf8ToWide(from);
    const std::wstring wto   = utf8ToWide(to);
    if (wfrom.empty() || wto.empty()) return -1;
    return ::_wrename(wfrom.c_str(), wto.c_str());
#else
    return std::rename(from.c_str(), to.c_str());
#endif
}

} // namespace fc