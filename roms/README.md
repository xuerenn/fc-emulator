# roms/

**这个目录里的 ROM 不会进版本库。**

`.gitignore` 排除了 `*.nes` / `*.fds` / `*.unf` / `*.unif` 等所有卡带格式，
原因有两个：ROM 受版权保护，而且体积不小。

放 ROM 和存档的约定：

```bash
# 自制的最小 NROM 测试卡带（含源码，可放心使用）
python tools/make_test_rom.py roms/test.nes
build/fc-headless.exe roms/test.nes 60 shot.ppm

# 你自己的卡带 dump —— 只放在本地，不要提交
cp /path/to/your-dump.nes roms/
```

运行时的存档（`<rom>.fcstate`）与键位配置（`fc-keys.cfg`）同样被忽略，
见仓库根目录的 `.gitignore`。

合法的 ROM 来源：你自己拥有并 dump 的卡带、公有领域作品、以及作者明确允许分发的
homebrew / 技术演示。**请勿提交商业游戏的 ROM。**
