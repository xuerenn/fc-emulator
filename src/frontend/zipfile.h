// zipfile.h — 最小 ZIP 读取器（只读、只解析中央目录，用于从压缩包里取 .nes）
//
// 为什么自己写而不是引第三方库：MinGW-w64 自带 zlib（-lz 直接可用），
// 而 zip 的容器格式本身只是「中央目录 + 本地头 + deflate」，几十行就能覆盖
// 我们需要的全部能力（列出条目、取其中一个）。引入 miniz/libzip 反而多一个依赖。
//
// 明确不支持：加密条目、分卷压缩、写回。遇到 ZIP64 会自动按扩展字段解析
// （大压缩包常见），但超过 4GB 的单个条目仍会拒绝——卡带不会有那么大。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fc {

struct ZipEntryInfo {
    std::string name;        // 包内路径（已转成 UTF-8，含目录前缀）
    uint64_t    usize = 0;   // 解压后大小
    uint64_t    csize = 0;   // 压缩后大小
    uint16_t    method = 0;  // 0 = 不压缩，8 = deflate
    uint64_t    offset = 0;  // 本地头（local file header）偏移
};

struct ZipInfo {
    bool        ok = false;
    std::string err;                  // ok == false 时的原因（人话）
    std::vector<ZipEntryInfo> entries; // 包内全部条目
    std::vector<ZipEntryInfo> roms;    // 其中看起来是 NES 卡带的那些
};

// 只读中央目录，不解压任何数据。失败时 ok=false 且 err 有原因。
ZipInfo zipOpen(const std::string& zipPath);

// 把指定条目解出来写到 outPath。
//   outPath 的父目录不存在时会自动创建（Windows 下）。
// 返回 false 时 err 给原因。
bool zipExtractTo(const std::string& zipPath, const ZipEntryInfo& e,
                  const std::string& outPath, std::string* err);

} // namespace fc
