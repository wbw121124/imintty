# imintty 现代终端特性与体验增强计划

## 现状结论

| 项 | 结论 |
|---|---|
| 工作目录 | `E:\吴邦玮\项目\imintty`（clone 到当前目录根） |
| 构建链 | MSYS2 位于 `F:\msys64`（`usr\bin` 有 gcc/make），PATH 需会话级设置；imintty 官方支持 MSYS2 构建 |
| imintty 已有 | Unicode 17、真彩、sixel/ReGIS、OSC 8、undercurl、Uniscribe 连字（`Ligatures`）、tabbar、iTerm2 图片 |
| 主要缺口 | 渲染引擎全量迁移（D2D+DWrite+HarfBuzz）、LuaConfig 脚本引擎、光标画布透明与动画渲染修复、OpenType 风格集完善 |

## 实施顺序（当前）

> 顺序已确认：**plan.md → Phase C（Lua）→ Phase B（渲染）→ 回验 Phase A**

### Phase C — LuaConfig 完整脚本配置引擎

#### C1 选项与路径
- 新选项 `LuaConfig`（`OPT_WSTRING`，`.iminttyrc`：`LuaConfig=~/.config/imintty/init.lua`）。
- 路径展开统一 helper（复用/扩展 `path_posix_to_win_w`）：
  - MSYS2 POSIX：`/c/Users/...`、`~/...`
  - Windows：`C:\...`
  - 超长路径：`\\?\` 前缀规范化（读写前统一加/剥策略）
- 加载时机：`load_config` 读完 rc 后求值；主题切换 / Options Apply 可重入；错误 pcall + 初始化期报告。

#### C2 嵌入与 API（MSYS2 Lua 5.5，`-llua` 动态链接）
- `LICENSE.bundling` 已增加 Lua 许可条目；Makefile `lualib=-llua`（系统包 `lua 5.5.x`，`msys-lua-5.5.dll` 运行时）。
- 首期完整 API：
  - `imintty.set(key, value)` / `get(key)` — 走 `set_option` 同一类型系统（只进内存 `cfg`，不回写 rc）
  - `imintty.rc(path)` — 读取子 rc/theme（复用 `load_config`）
  - `imintty.on(event, fn)` — 钩子：`config_loaded`、`bell`、`command`（逐步扩展）
  - `imintty.command(name, fn)` — 注册命令（对接 UserCommands 分发）
  - `imintty.key(keyspec, action)` — 键绑定（对接 KeyFunctions）
  - `imintty.expand_path(path)` / `imintty.home` — 路径与家目录
  - 标准库全开（`luaL_openlibs`：io/os/string/table/…）；完整脚本 API，不额外禁用 `os.execute`
- 加载：`finish_config()` 后 `winlua_init` → `winlua_load_config` → `fire("config_loaded")`；`win_reconfig` 检测 `LuaConfig` 变更则 `winlua_reload`；`exit_imintty` → `winlua_shutdown`。
- 事件：`config_loaded`、`bell`；命令：`KeyFunctions`/`UserCommands` 未命中内置名时查 `imintty.command`。
- GUI：选项对话框展示 `LuaConfig` 路径 + “重新加载 Lua”（可选，未做）。
- 改动不回写 `.iminttyrc`（函数/表无法序列化为 `name=value`）。

#### C3 验证
- [x] `make -C src` 零警告（`-Werror`）
- [x] 冒烟：`LuaConfig=~/.config/imintty/init.lua`，`config_loaded` 中 `set("Rows","24")` → stderr `lua ok rows=24`，exit 0
- [x] 坏脚本 `error(` → 报告 `unexpected symbol`，exit 0 不崩溃
- [ ] GUI 选项框 LuaConfig 路径 + 重新加载按钮（可选）

---

### Phase B — 渲染引擎全量迁移（D2D + DirectWrite + OpenType + HarfBuzz）

> **边界变更**：原「不做 GPU/Direct2D 全量重写」已废止，改为「分层迁移 + `RenderBackend` 可回退到 GDI」。

#### B1 DirectWrite 文字路径（先落实现 `FontRender=dwrite`）
- [x] 修复死代码：`use_dwrite` 宏定义位置、作用域错误 → 新建 `src/windwrite.c`（TextAnalyzer shaping + cluster→cell + `ExtTextOutW(ETO_GLYPH_INDEX)` GDI 合成）。
- [x] `FontFeatures=ss01,zero,calt,…`（`dw_parse_features`；`Ligatures>1` 且无显式 features 时强制 liga+calt）。
- [ ] `IDWriteFontFallback::MapCharacters`（`FontFallback=yes`；与 `FontChoice`/`FontSubst` 优先级对齐）。
- [x] **cluster→cell 映射**：等宽契约，advance 聚类回格（连字不破格对齐，参考 #601）。
- [ ] 字形检测统一走 DWrite（逐步废弃 `GetGlyphIndicesW` 双源）。
- [ ] 宽度测量：`GetGlyphMetrics` advance 替代 `win_char_width` 扫像素（保留 `@cjkwide` 等特判）。
- [x] 配置：`FontFeatures`（`OPT_WSTRING` + `config.template`）；`FontRender=dwrite|uniscribe|textout` 已有。
- [x] `wintext.c` 接线：删死块；`use_dwtext` 运行时 flag（与 `#define use_dwrite` 宏隔离）；`text_out_*` 三路分派；`win_init_fontfamily` 挂 `dw_font_changed`；combining / `wscale!=100` / 测量路径关闭 DWrite。
- [x] 验证：`make -j4` 零警告；`FontRender=dwrite|uniscribe|textout` 冒烟 exit 0。

#### B2 Direct2D 渲染目标
- `ID2D1HwndRenderTarget`（后期可评估 DXGI swapchain）接管客户区。
- 文字：`DrawGlyphRun`（消费 B1 排版结果）。
- 光标 / overlay / 滚动：D2D 形状 + 真 alpha（取代 `blend_colour` 假 alpha）。
- sixel / emoji / iTerm2 / Kitty 图像：`ID2D1Bitmap`（WIC 解码）+ alpha 合成；GDI+ 路径保留为 fallback。
- RTL：`minibidi` 双向重排保留；shaping 换 HarfBuzz；`SetWorldTransform` 镜像改 D2D transform。
- 配置：`RenderBackend=d2d|gdi`（默认迁移完成后 d2d，可回退）。

#### B3 HarfBuzz + OpenType shaping
- 字体 blob：`IDWriteFontFace` → `hb_blob_create`（或 FreeType 后端，按包可用性定）。
- 替换 `do_shape` / 逐步退役 Uniscribe 成 `hb_shape`；`minibidi` 只做 bidi。
- 配置：`ShapingEngine=harfbuzz|uniscribe`（默认 harfbuzz，可回退）。
- 连字/样式集：`hb_feature_from_string` 与 `FontFeatures` 同一解析入口。

#### B4 分阶段落地（每步可编译可跑、独立 commit）
1. [x] B1 纯文字 DWrite（仍与 GDI 合成）
2. [ ] B2 光标 + overlay 进 D2D
3. [ ] B2 文字进 D2D
4. [ ] B3 HarfBuzz 接入
5. [ ] 图像层（WIC + D2D bitmap）迁移
6. [ ] 默认 `RenderBackend=d2d`，GDI 仍可选

#### B 验证
- 每步 `make -C src` 零警告；中英/RTL/连字/emoji/sixel 冒烟；`FontRender` 与 `RenderBackend` 回退路径回归。

---

## Phase A — 光标画布透明 + 动画渲染修复（回验）

> **状态：待 C/B 之后回验问题是否仍在**（此前已有相关修复提交，需目测确认）。

### A1 分离光标画布背景透明
- `draw_cursor_to_canvas`（`wintext.c`）：
  - `CreateCompatibleBitmap` → **32bpp DIBSection**；清 alpha=0，去掉整幅不透明 `Rectangle(bg)`。
  - overlay 提前 `return` 时跳过合成（避免纯背景盖全窗）。
  - 合成：`BitBlt(SRCCOPY)` → 光标脏矩形 blit 或 `AlphaBlend(AC_SRC_ALPHA)`。
  - 合成前复位 `SetWorldTransform`（与 win_paint 路径对齐）。

### A2 动画不渲染
- `draw_cursor_overlay` 返回条件不得触发全窗覆盖。
- `do_update` 状态机 `BLOCKED→IDLE` 丢帧：恢复时重新排程自续定时器。
- 动画 tick：`term_invalidate` 路径补 `win_schedule_update` 兜底。
- 验证矩阵：块/竖线/下划线 × smooth/expand/phase/trail × 分离画布 on/off。

### A 验证步骤（C/B 完成后执行）
1. 启动后 Options → 动画 → 各模式切换，观察光标是否正常出现/消失。
2. 开启 `CursorSeparateCanvas=yes`，确认光标层背景透明（文本不被纯色盖住）。
3. 光标平滑移动 + 拖尾 + expand 闪烁目测。
4. 若问题仍在 → 按 A1/A2 修复并提交；若已不在 → 记录「已由既有提交覆盖」并关闭。

---

## 已完成阶段归档（原 Phase 0–7）

> 以下阶段代码已落地（含后续 bugfix），保留作对照；后续文档以「当前实施顺序」为准。

### Phase 0 — 环境与基线构建 ✅
1. `git clone …` 到当前目录。
2. 会话级 PATH 指向 `F:\msys64\usr\bin`。
3. `make -C src` 产出 `bin/imintty.exe`。

### Phase 1 — 平滑闪烁（三路独立配置）✅
- 配置：`SmoothBlinkAttr/Cursor/Bell`、`SmoothBlinkDuration`（动画模式：blink/smooth/phase/expand/solid）。

### Phase 2 — 平滑光标（类 VS Code）✅
- 配置：`SmoothCursor`、`SmoothCursorDuration`；光标拖尾 `ControlCursorTail`；分离画布 `CursorSeparateCanvas`（透明与动画问题见 Phase A 回验）。

### Phase 3 — 平滑滚动 ✅
- 配置：`SmoothScroll`、`SmoothScrollDuration`、`SmoothScrollLines`；背缓冲 + 插值。

### Phase 4 — 渲染性能 + 默认配置 + UI 打磨 ✅（部分持续）
- 脏矩形、`LigaturesSupport` 默认、选项对话框暴露动画/光标配置（Animation 面板空列崩溃已修）。

### Phase 5 — Kitty 键盘协议 ✅
- CSI u 解析/编码、5 渐进位、DA1、`KittyKeyboard` 配置、wiki 文档。

### Phase 6 — OpenType 字体特性 + DirectWrite 回退 → **并入 Phase B**
- 原单文件 `windwrite.c` 桩已确认不可用（宏/作用域问题），按 B1 重做。

### Phase 7 — Kitty 图形协议核心子集 ✅
- APC 解析、`t/T` 分块、`q=?` 应答、`KittyGraphics` 配置；动画/RGBA+zlib 仍暂缓。

### 已修 bug（近期）
- 大选区复制卡死（RTF O(N²) 增长）+ 对话框 `plat_ctrl` 空指针。
- Options → 动画面板 `CTRL_COLUMNS` 空 percentages 崩溃。
- `cursor_neovide_expand` → `ControlCursorTail` 重命名；中文文案与 po 同步。

---

## 验证策略
- 每 Phase / 每 B 子步结束：`make -C src` 零警告回归 + 启动冒烟。
- 协议：kitty 官方测试序列 / `printf` 脚本验证键盘标志位、`q=?` 探测、PNG 显示。
- 动画与光标：按 Phase A 矩阵目测；可配置关断。
- Lua：示例 `init.lua`（set/on/command/key）+ 错误脚本容错。
- 渲染：`FontRender` 三档 × `RenderBackend` 两档交叉冒烟（中/英/RTL/emoji/sixel）。

## 风险与边界
- Phase B 工作量大：B1→B4 分层提交，始终保持 GDI 回退可编译。
- 等宽契约与 cluster→cell 映射是 DWrite/HarfBuzz 最大回归面（连字、CJK、emoji overhang）。
- RTL：`minibidi` 负责 bidi 缓存（鼠标/选择依赖），不可整体删除；HarfBuzz 只替换 shape。
- Lua：静态链接许可需记入 `LICENSE.bundling`；`os.execute` 默认禁用；脚本改动不回写 rc。
- 不上游 PR（imintty 政策），改动留本地仓库。

## 目前不实现
> FontFeatures/FontFallback 的 GUI 细化完善、Kitty 图形 mode 0（仅传输不显示）验证、实际 PNG e2e 渲染测试；D2D DXGI swapchain / 独立合成线程（B2 之后再评估）。
