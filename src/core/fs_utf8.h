// fs_utf8.h — 以 UTF-8 路径访问文件
//
// 程序内部一律用 UTF-8 表示路径（启动器列目录、命令行参数、配置文件里存的都是
// UTF-8），但 Windows CRT 的窄字符接口（std::fopen 等）是按系统 ANSI 代码页
// （中文版是 GBK）解释路径的：一串 UTF-8 的中文字节会被当成乱码去找文件，结果
// 「文件明明在，就是打不开」。所以 Windows 上统一改走宽字符接口。
#pragma once

#include <cstdio>
#include <string>

namespace fc {

// 系统 ANSI（Windows 中文版为 GBK）编码的字符串转成 UTF-8，其他平台原样返回。
// 用途：Windows 上 main() 拿到的 argv 就是 ANSI 编码，先转成 UTF-8 才能和
// 启动器、配置文件里的路径对齐。
std::string ansiToUtf8(const std::string& s);

// 打开文件，path 为 UTF-8。参数与返回值和 std::fopen 完全一致
// （"rb" / "wb" / "r+b" 等）。
std::FILE* fopenUtf8(const std::string& path, const char* mode);

// 删除 / 重命名，path 为 UTF-8，返回值和 std::remove / std::rename 一致。
int removeUtf8(const std::string& path);
int renameUtf8(const std::string& from, const std::string& to);

} // namespace fc