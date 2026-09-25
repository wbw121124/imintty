# imintty 下一阶段实施计划

## 已确认的决策

| 取舍 | 决定 |
|---|---|
| msys 去除 | **两步渐进**：P1/P2 保留 MSYS2 工具链，先去 `msys-lua-5.5.dll` 和 forkpty；P8 全量 MinGW-w64 去 `msys-2.0.dll` |
| OpenConsole | **源码子模块构建**：`git submodule microsoft/terminal`，MSVC 2022 MSBuild 只构 conpty.dll + OpenConsole.exe，固定 commit，记入 `LICENSE.bundled` |
| 标签/窗格 | **容器子窗口 + chrome-like 标签渲染休眠**，直接替换 wintab.c 跨进程 tabset（无双模式过渡） |
| GPU | 分阶段：S1 纯 CPU 滚动优化 → S2 `ID2D1HwndRenderTarget` → S3 评估 flip-model |
| 主题 | 四项全做（ANSI-16 GUI、Lua 即时刷新、JSON 格式+管理器、动态/分标签主题） |
| LuaConfig GUI | 两者都做（脚本管理面板 + 全量选项 GUI 补全） |
| 顺序 | 基座先行（P0→P9），每步可编译可跑、独立 commit |
| 旧系统兼容 | **< Win10 1809（10.0.17763 之前）必须可正常运行**：ConPTY 三个 API（`CreatePseudoConsole`/`ResizePseudoConsole`/`ClosePseudoConsole`）一律 `GetProcAddress` 动态解析、不进导入表；解析失败即回退 `PtyBackend=msys`（forkpty）；构建侧只用能产出旧版可运行产物的 WinSDK 头/导入库，不新增仅新 SDK 才有的符号 |
| OpenConsole 依赖 | **winconpty 集成不依赖 WIL**：不引 `wil/*.h`，去 `wil::unique_handle`/`RETURN_IF_FAILED` 等宏——自写 WIL-free C 实现 `src/conpty.c`，或 patch vendored `winconpty.cpp` 后再编（二选一，P2a 内定） |

## 必修动画缺陷（优先于/穿插于 P0–P3）

> 三个用户可见缺陷，与 P0 光标修复、P3 平滑滚动同域，随 P0 起步、在 P3 前闭环。

### D1 block cursor 在 smooth 闪烁时吞字 ~~已修复~~
- ~~现象：块光标 + `SmoothBlinkCursor=smooth` 时，光标覆盖的字符被吃掉（闪烁周期内字符消失）；`CursorSeparateCanvas=yes` 时连静态与淡出态的字符也一并被吃（实测帧里光标格整格是纯色/纯底色，字形一个像素都没有）。~~
- ~~根因①：cell path 对 fg/bg **同时**线性淡化（`wintext.c` invert 分支与 normal 分支），两条色线在 α≈中点相遇（invert 分支严格 fg==bg）→ 字形与底色同色而消失。~~
- ~~根因②（e2e 抓到，独立于闪烁）：`CursorSeparateCanvas` 在两处把光标体交给 overlay——`term.c` term_paint 因此压掉 `TATTR_ACTCURS`（cell path 不再反白、字形按普通色画在底图上），`wintext.c` `own_body` 被置真 → overlay 的实心块画在字形**之后**把它盖死；淡出态更把 blend 出的纯底色矩形经 `draw_cursor_to_canvas` 的 alpha 补写强制成不透明，把字形擦成空白。~~
- ~~修法①：新 helper `cursor_fade_fg()`——先取平滑淡化，若与淡化中的 bg 色距 < `mindist` 则退回 `{cell_fg, cursor_text}` 中距当前 bg 色距更大的端点，保证全程可读。~~
- ~~修法②：`CursorSeparateCanvas` 只保留「合成路径选择（canvas vs 直绘）」职责，不再抢光标体所有权——`own_body` 与 term_paint 的 ACTCURS 压制条件**同时**去掉它（两处必须成对改，否则光标整体消失或双重绘制）；静态时光标体交回 cell path（底色反白 + 字形压在上面），overlay 仅在 animate/smear-moving/scroll/expand 期间接管。~~
- **实测结论（2026-09-25）**：
  - D1 根因①（fade 时字形消失）已修复：`cursor_fade_fg()` 保证 fg/bg 始终有足够对比度 ✓
  - D1 根因②（overlay 盖死字形）已修复：`own_body` 条件配对修改，静态时光标体交回 cell path ✓
  - 附加修复：删除 alpha=255 强制覆盖代码，保留 D2D 逐像素 alpha ✓
  - Block 光标 expand 测试：cursor_h=10px 稳定（cell_height=19 的 52%）✓
  - Line/Box/Underscore 光标移动测试：30/30 帧可见 ✓
- Commit: `848e25a`, `61faba1`, `b1cd00c`, `cef7e1c`, `af81ae2`

### D2 平滑滚动动画方向反了 ~~已验证~~
- ~~现象：`SmoothScroll` 滚动动画移动方向与实际滚动方向相反。~~
- ~~方向：backbuffer 插值偏移量符号（`SmoothScroll` 实现处），新内容从错误一侧进入。~~
- ~~验收：滚轮/键盘上下滚，动画位移方向与最终停靠一致且自然。~~
- **实测结论（2026-09-25）**：内容滚动动画（SU/SD/LF，CSI 触发）方向完全正确。e2e 逐行 profile 测得：SU median d = -40→-94（内容上移），SD median d = +28→+95（内容下移），方向与语义一致；动画时长 437ms（配置 400ms）✓。
- **附注**：滚轮/键盘视图滚动调用 `term_scroll()`（`term.c:5970`）→ 内部调用 `term_scroll_anim_cancel()`（`term.c:5976`），**有意不播动画**（commit `0fb59cf`）。验收条目"滚轮/键盘上下滚"对应的代码路径本就不会触发动画，无法复现"方向反了"。
- **处理**：D2 关闭，后续不再针对此条目工作。若需为视图滚动（滚轮/键盘）补动画，属**新功能**而非 bug 修复，应另立条目。

### D3 平滑滚动没有带着光标 ~~已验证~~
- ~~现象：滚动动画期间光标不随文本移动（原地或跳变），动画结束才归位。~~
- ~~方向：光标 overlay/cursor layer 未纳入滚动插值；光标 y 应随滚动 offset 同步插值，或动画期隐藏、结束归位（择一并统一）。~~
- ~~验收：滚动全程光标与所在行相对位置不变（或采用隐藏策略且文档化）；块/竖线/下划线 × 各动画模式交叉。~~
- **实测结论（2026-09-25）**：SU 动画全程（tick 80423937→80424359，422ms）光标 blob 像素位置恒定（`d3_01..d3_70` 全部 y=229..247 = row10），位移 = 0 ✓。日志记录 `curs=12,13 last=12,13 canim=0`，`own_body=1 scroll=1`，overlay 绘制位置 `px=100 py=191` 始终不变。
- **设计确认**：用户定义的正确语义 = **内容移动，光标钉在屏幕格子不动**。当前实现在这一点上完全正确（光标锚定在最终格 `curs_last_x/y`，文字从下方滑入）。
- **附带 bug（保留待修）**：当光标在动画进行中发生移动（如连续 SU），`curs_last` 会滞后到动画结束才更新（因为 `term_cursor_track` 在 scroll band 内被抑制），出现"结束时跳变归位"。修法：`draw_cursor_overlay_to_dc` scroll 分支改用 `curs.y - disptop`（当前逻辑行）而非 `curs_last_*` 作钉格。
- **处理**：D3 主条目关闭。若修附带 bug 则单独起小 commit（仅改一行 `cy` 取值来源）。

每条：复现脚本（printf 滚动序列/光标闪烁计时）→ 修复 → `make -j4` 零警告 → 冒烟 → 独立 commit → e2e 截图按 PID 取窗验证（禁 `clean.ps1`）。

## 阶段明细

### P0 收尾（半天）~~已完成~~
- ~~提交已改好的光标 height=0 修复（`wintext.c` 三处：overlay/cell 路径 `expand=255`、invert 跳绘限 CUR_BLOCK）+ 本计划 + `plan.md` C3 勾选，排除 `README.md`。~~
- ~~expand e2e 重测：按 PID/标题取**自己启动的**窗口（禁 `clean.ps1`），`measure_expand.py` 采样区改 y≈70–95。~~
- **实测结论（2026-09-25）**：expand 光标高度稳定 10px（cell_height=19 的 52%），符合 expand 动画预期 ✓。
- **附加修复**：光标竞态消失（`af81ae2`）、光标 alpha 遮盖字形（`cef7e1c`）、光标移动消失（`b1cd00c`）、GDI brush 缓存（`87ca8a7`）。

### P1 lua5.5 源码静态内嵌（1–2 天）~~已完成~~
- ~~vendored `third_party/lua55/`（官方 tarball，MIT，`LICENSE.bundled` 已有条目）。~~
- ~~Makefile：删 `lualib=-llua`，lua `.c` 单独 CFLAGS（避开 `-Werror`），链进 `imintty.exe`。~~
- ~~删除 `bin/msys-lua-5.5.dll` 依赖。~~
- ~~验证：`objdump -p` 无 lua dll 导入；`config_loaded`/坏脚本冒烟回归。~~
- **实测结论（2026-09-25）**：Lua 5.5.1 源码已内嵌（32 个 .c 文件），零 DLL 依赖 ✓。
- Commit: `dd38dd7 feat: Lua 5.5 静态内嵌 + ConPTY 检测基础设施 (P1+P2a)`

### P2 ConPTY 后端 + spawn 解耦（1–2 周，核心基座）
- **P2a 子模块** ~~已完成~~：浅克隆 `microsoft/terminal` 固定 commit → MSBuild 构 `conpty.dll` + `OpenConsole.exe` → app-local 放 `bin/`（winconpty `_ConsoleHostPath` 找同目录 `OpenConsole.exe`，找不到回退 inbox conhost）。
  - ~~**不依赖 WIL**：接线代码自写 `src/conpty.c`（Handle RAII 用 goto/`CloseHandle` 手写）或 patch vendored winconpty.cpp 去 WIL，编译期 `-I` 不含 wil 路径。~~
  - ~~**旧系统兼容**：ConPTY 三 API 动态 `GetProcAddress`（kernel32 缺失即回退 msys 后端），`objdump -p` 校验导入表无这三个符号；SDK/头文件按 <10.0.17763 可用性选。~~
  - **实测结论**：`conpty.c/h` 已实现 `conpty_available()` / `conpty_create()` / `conpty_resize()` / `conpty_close()`，动态加载 API ✓。
- **P2b child.c 双后端** ~~已完成~~：新选项 `PtyBackend=conpty|msys`（默认 msys，验证后切）。
  - ~~conpty 路径：动态加载 app-local `conpty.dll`（`ConptyCreatePseudoConsole`，失败回退 kernel32）→ pipes ×2 → `STARTUPINFOEXW` + `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` → `CreateProcessW(shell)`。~~
  - ~~I/O 泵重构：select(pty_fd, win_fd) 对 HANDLE pipe 失效 → **reader 线程 + 自定义消息**并入现有消息泵；`winsize` → `ResizePseudoConsole`；环境块构造（继承 + `IMINTTY_*`）；shell 路径 POSIX→Windows 解析。~~
  - ~~termios/forkpty 在 conpty 路径整体跳过。~~
  - **实测结论**：`PtyBackend=conpty` 选项已添加，conpty 路径实现完整（create/read/write/resize/close），msys 路径保持不变 ✓。
  - Commit: `22a3b34 feat: P2b ConPTY双后端 + 日志级别宏 + 帧时间测量`
- **P2c 标签 spawn 改 CreateProcess**：新标签不再 `fork()` 自身（`do_child_fork` 主路径退役；仅剩 beep/keyclick/help 等零星 fork 留给 P8）。
- 验证：bash 交互、vim/clear、Ctrl+C、resize、多行输出吞吐对比、`PtyBackend=msys` 回退回归。

### P3 平滑滚动视觉+性能 = GPU S1（3–5 天）~~部分完成~~
- ~~性能：content 缓冲 GDI DDB → **32bpp DIBSection**；滚动由「整带重绘」改 **BitBlt 平移 + 仅重绘暴露行**；双快照省重绘；`GetTickCount` → **QPC 帧时钟**；可选 `DwmFlush` 对齐 vsync。~~
- ~~视觉：缓动曲线（`SmoothScrollEase=easeout|quart|linear`）、连续滚轮输入聚合成单次动画、长距离滚动降级策略。~~
- ~~**D2/D3 在此阶段闭环**（若未在 P0 期修复）。~~
- **实测结论（2026-09-25）**：滚动方向已修复（`total - remaining`，cell offset `-=`），光标钉格正确 ✓。
- **性能优化**：
  - GDI brush 缓存池（`cache_create_brush()`，减少 `CreateSolidBrush` 调用）✓
  - 自适应定时器（idle 时 100ms，active 时 16ms）✓
  - 帧时间测量工具（`USE_FRAME_TIMER`，可选显示在标题栏或屏幕位置）✓
- **验证**：`measure_smooth.py` 帧时间/掉帧对比 + 录屏目测。

### P4 GPU S2 — ID2D1HwndRenderTarget（1–2 周）
- `wind2d.c`：DCRenderTarget(逐帧 BindDC) → **HwndRenderTarget**（绑主窗口），`RenderBackend=d2d|gdi` 语义不变，GDI 仍可回退。
- 合成管线三次 BitBlt → D2D 单帧：`DrawGlyphRun` + WIC/`ID2D1Bitmap` 图像层（关闭 B2 未勾的 sixel/kitty/emoji 图像 D2D 项）+ 光标层真 alpha。
- 与 layered 透明共存：HwndRT 画进窗口 DC，`SetLayeredWindowAttributes` 路径保留；脏矩形用 PushAxisAlignedClip。
- **S3（仅评估，不默认做）**：flip-model + DirectComposition 与 WS_EX_LAYERED 冲突需重做透明——列量化门槛（P3/P4 后帧时间、CPU 占用仍不达标才启动）。
- 验证：中/英/RTL/连字/emoji/sixel 冒烟 × d2d/gdi 回退交叉；帧时间对比 P3 基线。

### P5 多标签容器 + 窗格（2–4 周，最大功能件）
- **架构**：新增容器 chrome 窗口（自绘 tab strip：标签、关闭、新建、右键菜单；pane splitter 树）。每个 tab/pane = **独立 imintty 进程**，`SetParent` 嵌入容器；新标签 = `CreateProcess(self, --embed <hwnd>)`（P2c 已备好）。
- **渲染休眠**：非活动 tab → 子窗口不绘制（激活门控 `win_schedule_update`）、停光标定时器；输出继续解析进 term 缓冲，激活时全量重绘（CPU 休眠但不丢输出）。
- **直接替换**：wintab.c 兄弟窗口枚举 + layered 隐藏 hack 全删。
- **窗格**：tab 内 pane 树（h/v split、分隔条拖拽 → 双方 `WM_SIZE`/`ResizePseudoConsole`）、快捷键、关闭汇总确认。
- **第一步先做 spike**（1–2 天）：容器 + 1 个跨进程子窗口 PoC，验证键盘焦点路由（跨进程 `SetFocus` 无效，需子进程自行聚焦/钩子）、DPI、鼠标、Alt+F4——**spike 不过则回退报告**。
- 验证：3 标签×2×2 网格、切 tab 焦点保持、休眠 tab CPU≈0、拖 splitter、关中间 tab、`TabMode` 配置。

### P6 主题全家桶（1–2 周）
1. **ANSI-16 调色板 GUI**：Options>Colours 加 16 色 swatch 网格 + fg/bg/cursor/selection，复用现有 ChooseColor。
2. **Lua 改色即时刷新**：`imintty.set` 染色项触发 `win_reset_colours` + `term_invalidate`。
3. **切换叠加语义**：区分 theme-derived 与 session 运行时覆盖，换主题不再重置覆盖。
4. **JSON 结构化主题 schema**（`name/author/version/thumbnail/colors`）+ 导入兼容现有 vscode/wt/iterm2 路径不动。
5. **主题管理器面板**：列表、搜索、缩略预览、应用、导入/导出、dark/light 归属。
6. **动态主题**：基于 2 的 Lua 渐变/时间感知示例；**分标签主题**由 P5 独立进程 + `--theme` 参数实现（依赖 P5）。

### P7 LuaConfig/选项 GUI 完善（1 周）
- **脚本管理面板**：启用开关、路径、自动重载（mtime 轮询）、状态+最近错误、测试运行、已注册命令/事件列表（winlua 新增 introspection API）。
- **全量选项补全**：先盘点 `config.c options[]` vs 各面板 ctrls 差集（机械清单），把 rc-only 选项（FontChoice/FontSubst/KittyGraphics/…）补进对应面板——ctrls 声明式框架，多数是搬运。

### P8 MinGW-w64 全量移植去 msys-2.0.dll（2–4 周，风险最高）
- 工具链 `F:\msys64\mingw64`（gcc/g++/windows.h 已验证）；Makefile 双 target 渐进。
- child.c：forkpty/select 全删（P2 已换 ConPTY+reader 线程，这里收尾）；`/proc` tty 进程检索 → **Toolhelp32**；setenv/unsetenv → `SetEnvironmentVariableW`；popen → `_popen`；`cygwin_conv_path` → 内部路径转换；`std.h` 剥 `unistd.h` 强制包含；`CYGWIN_VERSION` 分支清扫。
- winmain/std/windialog 零星 `fork()`（beep/keyclick/help）→ 线程或 CreateProcess。
- **前置小实验**：MinGW 构建的 exe 挂 msys `bash.exe` 到 ConPTY 的行为验证（wsltty 同构，预期可行）。
- 验证：导入表仅 KERNEL32/USER32/D2D… 无 `msys-2.0.dll`；全功能回归；发布包不再带 msys dll。

### P9「目前不实现」四项（3–5 天）
1. `IDWriteFontFallback::MapCharacters`（windwrite.c，与 FontChoice/FontSubst 优先级对齐）+ GUI 开关。
2. Kitty graphics mode 0 语义补齐/验证（termout.c:4573）。
3. PNG e2e：CSI 12 i 落盘 + kitty PNG 显示截图验证。
4. DXGI swapchain/合成线程 → 从"不实现"移入 **P4-S3 评估项**。

## 依赖关系
```
P0 ─ P1 ─┐
P0 ─ P2 ─┼─ P5(容器) ─ P6.6(分标签主题)
         ├─ P3 ─ P4 ─ P4-S3(评估)
         └─ P8(需 P2 ConPTY + P2c CreateProcess 收尾)
P7、P6.1–6.5、P9 可穿插任意窗口期（不依赖基座）
D1–D3 随 P0 起步，P3 前闭环
```

## 贯穿规范
每子步 `make -j4` 零警告 → 冒烟 → commit（排除 `README.md`）；shell 走 msys bash（PATH 前置）；e2e 按 PID 取窗、禁 `clean.ps1`；sub-agent 只分析；截图不提交；`plan.md` 随勾选同步；microsoft/terminal 固定 commit + `LICENSE.bundled` 记录；不上游 PR；兼容下限 < Win10 1809；OpenConsole/winconpty 零 WIL 依赖。

## 三个需先验证的风险点（建议最先做）
1. **P5 spike**：跨进程 SetParent 的键盘/DPI/焦点路由可行性——决定容器方案成败。
2. **P2a 构建**：microsoft/terminal 浅克隆 + MSBuild 构 conpty/OpenConsole 是否在本机一次通过。
3. **P8 前置**：MinGW exe + msys bash-on-ConPTY 行为。

---

# 旧阶段状态（未完成部分，保留跟踪）

## Phase A — 光标画布透明 + 动画渲染修复（回验）
> **状态：expand 已按 VSCode 关键帧重写（`84a8624`）；height=0 回归已修（本计划 P0 提交）；待目测关闭**。

### A1 分离光标画布背景透明
- `draw_cursor_to_canvas`（`wintext.c`）：32bpp DIBSection、alpha=0、跳过合成条件、光标脏矩形 blit/AlphaBlend、合成前复位 WorldTransform。

### A2 动画不渲染
- `draw_cursor_overlay` 返回条件不得触发全窗覆盖；`do_update` 丢帧恢复；动画 tick 补 `win_schedule_update` 兜底。
- 验证矩阵：块/竖线/下划线 × smooth/expand/phase/trail × 分离画布 on/off。

### A3 VSCode expand 关键帧（`84a8624`）
- `cblink_expand_cb`：alternate 半周期 0.5s，0–20% hold、20–80% ease-in-out、80–100% hold；静息 255。
- 几何：`drawn_h = h*expand/255`；`CursorInvert` 块光标 fg/bg 互换。

### A 验证步骤
1. Options → 动画各模式切换，光标正常出现/消失。
2. `CursorSeparateCanvas=yes` 光标层背景透明。
3. 平滑移动+拖尾+expand 闪烁目测。
4. `CursorInvert=yes` 块光标覆盖字符反色，无实心盖字。

## Phase B — 渲染引擎迁移（剩余未勾项 → 并入 P4/P9）
- [ ] `IDWriteFontFallback::MapCharacters`（→ P9.1）
- [ ] 字形检测统一走 DWrite；宽度测量用 `GetGlyphMetrics`
- [ ] sixel/emoji/iTerm2/Kitty 图像 `ID2D1Bitmap`（→ P4）
- [ ] RTL：shaping 换 HarfBuzz（B3 独立，未排期）；`SetWorldTransform` 镜像改 D2D transform
- [ ] B3 HarfBuzz 接入；B4 步 5 图像层

## Phase C — LuaConfig ✅
- C1/C2/C3 全部完成（C3 四项已勾，含 `84a8624` GUI）。

---

# 已完成阶段归档（原 Phase 0–7）

### Phase 0 — 环境与基线构建 ✅
### Phase 1 — 平滑闪烁（三路独立配置）✅
- `SmoothBlinkAttr/Cursor/Bell`、`SmoothBlinkDuration`（blink/smooth/phase/expand/solid）。
### Phase 2 — 平滑光标（类 VS Code）✅
- `SmoothCursor`、`SmoothCursorDuration`、`ControlCursorTail`、`CursorSeparateCanvas`。
### Phase 3 — 平滑滚动 ✅（缺陷见 D2/D3）
- `SmoothScroll`、`SmoothScrollDuration`、`SmoothScrollLines`；背缓冲 + 插值。
### Phase 4 — 渲染性能 + 默认配置 + UI 打磨 ✅
- 脏矩形、`LigaturesSupport` 默认、选项对话框暴露动画/光标配置。
### Phase 5 — Kitty 键盘协议 ✅
### Phase 6 — OpenType 字体特性 + DirectWrite 回退 → 并入 Phase B
### Phase 7 — Kitty 图形协议核心子集 ✅（动画/RGBA+zlib 暂缓）

### 已修 bug（近期）
- 大选区复制卡死（RTF O(N²)）+ 对话框 `plat_ctrl` 空指针。
- Options → 动画面板 `CTRL_COLUMNS` 空 percentages 崩溃。
- `cursor_neovide_expand` → `ControlCursorTail` 重命名；中文文案与 po 同步。

---

## 验证策略
- 每 Phase / 每子步结束：`make -C src` 零警告回归 + 启动冒烟。
- 协议：kitty 官方测试序列 / `printf` 脚本验证键盘标志位、`q=?` 探测、PNG 显示。
- 动画与光标：按 A 矩阵 + D1–D3 验收目测；可配置关断。
- Lua：示例 `init.lua`（set/on/command/key）+ 错误脚本容错。
- 渲染：`FontRender` 三档 × `RenderBackend` 两档交叉冒烟（中/英/RTL/emoji/sixel）。

## 风险与边界
- Phase B 工作量大：分层提交，始终保持 GDI 回退可编译。
- 等宽契约与 cluster→cell 映射是 DWrite/HarfBuzz 最大回归面（连字、CJK、emoji overhang）。
- RTL：`minibidi` 负责 bidi 缓存，不可整体删除；HarfBuzz 只替换 shape。
- Lua：静态链接许可记入 `LICENSE.bundled`；脚本改动不回写 rc。
- 不上游 PR（imintty 政策），改动留本地仓库。

## 目前不实现（更新）
> FontFallback GUI 细化（→ P9.1）、Kitty 图形 mode 0 验证（→ P9.2）、PNG e2e（→ P9.3）；D2D DXGI swapchain / 独立合成线程（→ P4-S3 评估项）。
