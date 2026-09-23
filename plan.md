# imintty 现代终端特性与体验增强计划

## 现状结论

| 项 | 结论 |
|---|---|
| 工作目录 | `E:\吴邦玮\项目\imintty`（clone 到当前目录根） |
| 构建链 | MSYS2 位于 `F:\msys64`（`usr\bin` 有 gcc/make），PATH 需会话级设置；imintty 官方支持 MSYS2 构建 |
| imintty 已有 | Unicode 17、真彩、sixel/ReGIS、OSC 8、undercurl、Uniscribe 连字（`Ligatures` 选项）、tabbar、iTerm2 图片 |
| 主要缺口 | Kitty 键盘协议、Kitty 图形协议、OpenType 风格集开关、DirectWrite 字体回退、平滑滚动/光标/闪烁、默认体验调优 |

## 实施顺序（按性价比排序）

### Phase 0 — 环境与基线构建
1. `git clone https://github.com/imintty/imintty.git .` 到当前目录。
2. 会话级 PATH：`$env:PATH = "F:\msys64\usr\bin;$env:PATH"`（不持久化污染系统）。
3. `make -C src` 产出 `bin/imintty.exe`，确认零修改基线可编译可运行。

### Phase 1 — 平滑闪烁（三路独立配置）
- 改动：`src/term.c`（ATTR_BLINK）、光标闪烁/BEL 闪屏定时器（`src/winmain.c` / `src/wintext.c`）。
- 定时器 + `AlphaBlend` 多帧 alpha 过渡替代硬开关。
- 配置（`config.c` + `config.template`）：
  - `SmoothBlinkAttr=yes/no` — ANSI SGR 5 属性闪烁
  - `SmoothBlinkCursor=yes/no` — 光标闪烁
  - `SmoothBlinkBell=yes/no` — `\a` 响铃屏幕闪光
  - `SmoothBlinkDuration`（ms）

### Phase 2 — 平滑光标（类 VS Code）
- 改动：`src/wintext.c` 光标绘制 + 定时器。
- 单元格移动时保存上一帧/目标矩形，逐帧插值；缓冲避免字符残影；支持块/下划线/竖线。
- 配置：`SmoothCursor=yes/no`、`SmoothCursorDuration`。

### Phase 3 — 平滑滚动
- 改动：`term.c` 屏幕滚动 → `winmain.c` 失效重绘路径。
- 逻辑状态立即更新，显示层记录像素偏移，定时器插值到 0；每帧背缓冲 `BitBlt` + 新行绘制；兼容图片/sixel 叠层。
- 配置：`SmoothScroll=yes/no`、`SmoothScrollDuration`、`SmoothScrollLines`（阈值）。

### Phase 4 — 渲染性能 + 默认配置 + UI 打磨
- 性能：审计脏矩形，减少全窗重绘；连字/闪烁/光标帧增量绘制。
- 默认值：`LigaturesSupport=2`、平滑项默认开、ScrollbackLines、现代光标默认等（`config.c`）。
- UI：选项对话框（`windialog.c`/`winctrls.c`）暴露新配置。

### Phase 5 — Kitty 键盘协议
- 解析（`termout.c`）：`CSI = flags ; mode u`、`CSI > flags u` / `CSI < u`（pop）、`CSI ? flags u` 查询（DA1 前回）；模式栈。
- 编码（`wininput.c`）：5 渐进位——① 消歧义；② press/release/repeat（WM_KEYUP）；③ 备选键位；④ 全键 CSI u；⑤ 关联文本。
- DA1 能力位；配置 `KittyKeyboard=yes/no`；文档 `wiki/CtrlSeqs.md`。

### Phase 6 — OpenType 字体特性 + DirectWrite 回退（共用后端）
- 新路径 `FontRender=dwrite`（新增 `src/windwrite.c`）：`IDWriteTextLayout` + `IDWriteTypography`（`AddFontFeature`）+ `IDWriteFontFallback::MapCharacters`；保留 Uniscribe/TextOut 可回退。
- 配置：`FontFeatures=ss01,ss02,zero,calt,…`；`FontFallback=yes/no`（默认开，`FontChoice` 显式配置优先）。
- 难点：等宽约束（参考 #601）；CJK/Arabic 与 `FontSubst`(#1352) 衔接。
- 参考：Vim `gui_dwrite.cpp`、Windows Terminal 回退逻辑。

### Phase 7 — Kitty 图形协议核心子集
- 解析：`termout.c` APC（`ESC _ G … ST`）。
- 子集：传输 `t/T`（分块 base64、`m=0/1/2`、PNG 走 WIC）；placement 复用 `winimg.c` 按格叠层；删除 `d`；查询 `q=?` 先于 DA1 立即应答；DA1 加能力位。
- 暂缓：动画协议、RGBA+zlib（二期）。
- 配置：`KittyGraphics=yes/no`。

## 验证策略
- 每 Phase 结束：`make -C src` 零警告回归 + 启动冒烟。
- 协议：kitty 官方测试序列 / `printf` 脚本验证键盘 5 标志位、`q=?` 探测、PNG 显示。
- 动画：目测 + 可配置关断。

## 风险与边界
- Phase 6/7 工作量大；Phase 1–5 完成即为可交付版本。
- 不上游 PR（imintty 政策），改动留本地仓库。
- 不做 GPU/Direct2D 全量重写。
