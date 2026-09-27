// zipfile.cpp — 最小 ZIP 读取器实现（见 zipfile.h 的设计说明）
#include "zipfile.h"

#include "core/fs_utf8.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef FC_HAVE_ZLIB
#  include <zlib.h>
#endif

#ifdef _WIN32
#  include <windows.h>
#endif

namespace fc {
namespace {

constexpr uint32_t SIG_LFH    = 0x04034b50;   // local file header
constexpr uint32_t SIG_CD     = 0x02014b50;   // central directory header
constexpr uint32_t SIG_EOCD   = 0x06054b50;   // end of central directory
constexpr uint32_t SIG_EOCD64 = 0x06064b50;   // zip64 end of central directory
constexpr uint32_t SIG_LOC64  = 0x07064b50;   // zip64 EOCD locator

constexpr uint64_t kZip64Mark = 0xFFFFFFFFull;   // 需要去 ZIP64 扩展字段里找真值
constexpr uint16_t kZip64Mark16 = 0xFFFF;

// 单个 ROM 解压上限：FC 卡带最大也就几 MB。给到 64MB 已经很宽松，
// 同时挡住「条目头被人为改坏 → 分配几百 GB」这类恶意/损坏输入。
constexpr uint64_t kMaxRomBytes = 64ull * 1024 * 1024;

uint16_t rd16(const unsigned char* p) { return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8)); }
uint32_t rd32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t rd64(const unsigned char* p) { return uint64_t(rd32(p)) | (uint64_t(rd32(p + 4)) << 32); }

// Windows 的 long 只有 32 位，fseek 撑不住 >2GB 的偏移；统一走 64 位版本
int seek64(std::FILE* f, uint64_t off) {
#ifdef _WIN32
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

bool readAt(std::FILE* f, uint64_t off, void* buf, size_t n) {
    if (seek64(f, off) != 0) return false;
    return std::fread(buf, 1, n, f) == n;
}

uint64_t fileBytes(const std::string& p) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, p.c_str(), int(p.size()), nullptr, 0);
    if (n <= 0) return 0;
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p.c_str(), int(p.size()), &w[0], n);
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &d)) return 0;
    LARGE_INTEGER li;
    li.LowPart  = d.nFileSizeLow;
    li.HighPart = d.nFileSizeHigh;
    return uint64_t(li.QuadPart);
#else
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return 0;
    std::fseeko(f, 0, SEEK_END);
    const uint64_t n = uint64_t(std::ftello(f));
    std::fclose(f);
    return n;
#endif
}

// 大致判断是不是合法 UTF-8。有些打包工具不给条目名打 UTF-8 标志位、
// 但名字确实是 UTF-8，直接按 ANSI 转会把中文名转坏，所以先探一下。
bool looksUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        size_t extra = 0;
        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return false;
        if (i + extra >= s.size()) return false;
        for (size_t k = 1; k <= extra; ++k)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        i += extra + 1;
    }
    return true;
}

std::string ansiToUtf8(const std::string& raw) {
#ifdef _WIN32
    if (raw.empty()) return raw;
    // 未标 UTF-8 的条目名按系统 ANSI 代码页解释（中文 Windows 上是 GBK）。
    // 即使转失败也原样返回 —— 扩展名是 ASCII，判断 .nes 不受影响。
    const int n = MultiByteToWideChar(CP_ACP, 0, raw.c_str(), int(raw.size()), nullptr, 0);
    if (n <= 0) return raw;
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, raw.c_str(), int(raw.size()), &w[0], n);
    const int m = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), n, nullptr, 0, nullptr, nullptr);
    if (m <= 0) return raw;
    std::string s(size_t(m), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), n, &s[0], m, nullptr, nullptr);
    return s;
#else
    return raw;
#endif
}

std::string nameToUtf8(const std::string& raw, bool utf8Flag) {
    if (utf8Flag || looksUtf8(raw)) return raw;
    return ansiToUtf8(raw);
}

std::string lowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// 包内路径里我们只关心「最后一段」，且要排掉 macOS 打包时塞进来的元数据
bool looksLikeNes(const std::string& name) {
    if (name.empty()) return false;
    if (name.find("__MACOSX/") != std::string::npos) return false;
    const size_t slash = name.find_last_of("/\\");
    const std::string base = (slash == std::string::npos) ? name : name.substr(slash + 1);
    if (base.size() <= 4 || base[0] == '.') return false;
    const std::string tail = lowerAscii(base.substr(base.size() - 4));
    return tail == ".nes";
}

} // namespace

// ================================================================ 读中央目录

ZipInfo zipOpen(const std::string& zipPath) {
    ZipInfo zi;
    const uint64_t total = fileBytes(zipPath);
    if (total < 22) {
        zi.err = "文件太小，不是有效的 zip";
        return zi;
    }

    std::FILE* f = fc::fopenUtf8(zipPath, "rb");
    if (!f) {
        zi.err = "打不开压缩包";
        return zi;
    }

    // ---- 1) 从尾部回扫 EOCD（其后面最多还有 64KB 注释）
    const uint64_t tailLen = (total < 65557ull) ? total : 65557ull;
    // 注意别写成 tail(size_t(tailLen))：那会被当成函数声明（most vexing parse），
    // 而 tail{size_t(tailLen)} 又会被当成「一个元素的 initializer_list」。
    const size_t   tailN = size_t(tailLen);
    std::vector<unsigned char> tail(tailN);
    if (!readAt(f, total - tailLen, tail.data(), tail.size())) {
        std::fclose(f);
        zi.err = "读取压缩包尾部失败";
        return zi;
    }
    // 从最后一个「能放下 22 字节 EOCD」的位置往前找
    size_t eocdAt = std::string::npos;
    for (size_t i = tail.size() - 22;; --i) {
        if (rd32(&tail[i]) == SIG_EOCD) { eocdAt = i; break; }
        if (i == 0) break;
    }
    if (eocdAt == std::string::npos) {
        std::fclose(f);
        zi.err = "找不到 zip 结尾标记（文件可能损坏或不是 zip）";
        return zi;
    }

    uint64_t cdOffset  = rd32(&tail[eocdAt + 16]);
    uint64_t cdEntries = rd16(&tail[eocdAt + 10]);

    // ---- 2) 需要的话从 ZIP64 定位器/记录里取真值
    if (cdOffset == kZip64Mark || cdEntries == kZip64Mark16) {
        const uint64_t eocdAbs = total - tailLen + eocdAt;
        if (eocdAbs >= 20) {
            unsigned char loc[20];
            if (readAt(f, eocdAbs - 20, loc, sizeof(loc)) && rd32(loc) == SIG_LOC64) {
                const uint64_t e64 = rd64(loc + 8);
                unsigned char rec[56];
                if (readAt(f, e64, rec, sizeof(rec)) && rd32(rec) == SIG_EOCD64) {
                    cdEntries = rd64(rec + 32);
                    cdOffset  = rd64(rec + 48);
                }
            }
        }
        if (cdOffset == kZip64Mark || cdEntries == kZip64Mark16) {
            std::fclose(f);
            zi.err = "这个 zip 需要 ZIP64，但扩展记录读不出来";
            return zi;
        }
    }

    // ---- 3) 逐条解析中央目录
    uint64_t pos = cdOffset;
    for (uint64_t i = 0; i < cdEntries; ++i) {
        unsigned char h[46];
        if (!readAt(f, pos, h, sizeof(h)) || rd32(h) != SIG_CD) break;
        const uint16_t flags    = rd16(h + 8);
        const uint16_t method   = rd16(h + 10);
        uint64_t       csize    = rd32(h + 20);
        uint64_t       usize    = rd32(h + 24);
        const uint16_t nameLen  = rd16(h + 28);
        const uint16_t extraLen = rd16(h + 30);
        const uint16_t cmtLen   = rd16(h + 32);
        uint64_t       lfhOff   = rd32(h + 42);

        if (nameLen > 4096) break;                       // 名字不可能这么长，防坏文件
        std::string rawName(size_t(nameLen), '\0');
        if (nameLen && !readAt(f, pos + 46, &rawName[0], nameLen)) break;

        // ZIP64：中央目录里被标成 0xFFFFFFFF 的字段，真值按顺序放在 id=0x0001 的扩展字段里
        if (usize == kZip64Mark || csize == kZip64Mark || lfhOff == kZip64Mark) {
            const size_t exN = size_t(extraLen);
            std::vector<unsigned char> ex(exN);
            if (extraLen && readAt(f, pos + 46 + nameLen, ex.data(), ex.size())) {
                size_t p = 0;
                while (p + 4 <= ex.size()) {
                    const uint16_t id = rd16(&ex[p]);
                    const uint16_t sz = rd16(&ex[p + 2]);
                    if (p + 4 + sz > ex.size()) break;
                    if (id == 0x0001) {
                        size_t q = p + 4;
                        if (usize  == kZip64Mark && q + 8 <= p + 4 + sz) { usize  = rd64(&ex[q]); q += 8; }
                        if (csize  == kZip64Mark && q + 8 <= p + 4 + sz) { csize  = rd64(&ex[q]); q += 8; }
                        if (lfhOff == kZip64Mark && q + 8 <= p + 4 + sz) { lfhOff = rd64(&ex[q]); q += 8; }
                        break;
                    }
                    p += 4 + sz;
                }
            }
        }

        ZipEntryInfo e;
        e.name   = nameToUtf8(rawName, (flags & 0x0800) != 0);
        e.usize  = usize;
        e.csize  = csize;
        e.method = method;
        e.offset = lfhOff;
        zi.entries.push_back(e);
        if (method <= 8 && looksLikeNes(e.name)) zi.roms.push_back(e);

        pos += 46 + nameLen + extraLen + cmtLen;
    }

    std::fclose(f);
    zi.ok = true;
    return zi;
}

// ================================================================ 解出单个条目

bool zipExtractTo(const std::string& zipPath, const ZipEntryInfo& e,
                  const std::string& outPath, std::string* err) {
    auto fail = [&](const char* msg) {
        if (err) *err = msg;
        return false;
    };

    if (e.usize > kMaxRomBytes) return fail("包内条目过大，已拒绝解压");
    if (e.method != 0 && e.method != 8) return fail("这个条目用了不支持的压缩方式");

    std::FILE* f = fc::fopenUtf8(zipPath, "rb");
    if (!f) return fail("打不开压缩包");

    // 本地头的 name/extra 长度可能与中央目录不一致，必须以本地头为准
    unsigned char lh[30];
    if (!readAt(f, e.offset, lh, sizeof(lh)) || rd32(lh) != SIG_LFH) {
        std::fclose(f);
        return fail("包内条目头损坏");
    }
    const uint16_t ln = rd16(lh + 26), le = rd16(lh + 28);
    const uint64_t dataOff = e.offset + 30 + ln + le;

    const size_t inN = size_t(e.csize);
    std::vector<unsigned char> in(inN);
    if (e.csize && !readAt(f, dataOff, in.data(), in.size())) {
        std::fclose(f);
        return fail("读取压缩数据失败");
    }
    std::fclose(f);

    std::vector<unsigned char> out;
    if (e.method == 0) {
        out = std::move(in);
    } else {
#ifdef FC_HAVE_ZLIB
        out.resize(size_t(e.usize));
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return fail("zlib 初始化失败");
        zs.next_in   = in.data();
        zs.avail_in  = uInt(in.size());
        zs.next_out  = out.data();
        zs.avail_out = uInt(out.size());
        const int rc = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (rc != Z_STREAM_END && rc != Z_BUF_ERROR) return fail("解压失败（数据可能损坏）");
#else
        return fail("这个构建没有链接 zlib，只能读「不压缩」方式打包的 zip");
#endif
    }

    std::FILE* o = fc::fopenUtf8(outPath, "wb");
    if (!o) return fail("写不出解压结果（目录可能不可写）");
    const size_t w = out.empty() ? 0 : std::fwrite(out.data(), 1, out.size(), o);
    std::fclose(o);
    if (w != out.size()) return fail("写入解压结果时出错");
    return true;
}

} // namespace fc
