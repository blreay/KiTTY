# KiTTY Windows 终端字体 Fallback 机制设计

- 日期：2026-05-27
- 状态：草案，等待用户复核
- 目标：在 KiTTY (Windows) cell-based 终端渲染架构内，引入近似
  Windows Terminal / DirectWrite 的字体 fallback 效果，最小侵入、
  分阶段可演进、cross 与 cross64 编译皆通过。

## 1. 背景与现状

### 1.1 用户痛点

主字体（如 Consolas）缺字时，KiTTY 显示豆腐方块。典型缺失类别：

- Box Drawing / Block Elements / Geometric Shapes
- Misc Symbols / Dingbats（含 `U+23F5` ⏵）
- Nerd Fonts / Powerline / Font Awesome / PUA 私有区
- 部分 CJK 与黑白 emoji

### 1.2 现有渲染路径（`0.76b_My_PuTTY/windows/window.c`）

- **字体表**：`static HFONT fonts[FONT_MAXNO]`（64 ~ 79 槽，按
  `NORMAL/BOLD/UND/ITALIC/WIDE/HIGH/NARROW/OEM` 组合索引）
- **唯一字体源**：`conf_get_fontspec(conf, CONF_font)`
- **lazy 加载**：`another_font(int fontno)`（line 2817），按需 `CreateFont`
- **绘制入口**：`wintw_draw_text` → `do_text_internal`（line 6185）
  - DBCS `CSET_ACP` → `ExtTextOutW`
  - `DIRECT_FONT` → `ExtTextOut`
  - 其它 Unicode → `general_textout`
    - RTL 段：`exact_textout`（`ETO_GLYPH_INDEX`，禁用 font-linking）
    - LTR 段：`ExtTextOutW`（保留 GDI SystemLink）
- **字符宽度**：`wintw_char_width`（line 6854），仅在 `FONT_NORMAL`
  上 `GetCharWidth32W`

### 1.3 为什么 fallback 不生效

GDI 的 SystemLink 默认只把固定宽度英文字体（Consolas、Lucida Console
等）链接到几个 CJK 字体（MS YaHei / PMingLiU 等），这些字体也不含
符号区与 PUA。结果：主字体缺字 → SystemLink miss → `.notdef` 方块。

## 2. 设计决策

| 决策点 | 选择 | 理由 |
|---|---|---|
| 渲染后端 | **GDI + `GetGlyphIndicesW`** | 最小侵入、Win7+、cross/cross64 零工具链改动；彩色 emoji 留给二阶段 DirectWrite |
| Emoji 颜色 | MVP 单色 | 黑白 fallback 即可满足主要需求 |
| fallback 列表 | 内置默认 + ini 覆盖/追加 | 开箱即用，高级用户可自定义 |
| 宽度来源 | EAW + KiTTY 现有 `wintw_char_width` 主导 | 严守 cell 网格；fallback 字体真实 advance 仅参与 clip |
| baseline 对齐 | 不调整 y，靠 GDI ascent | 简单、性能好；少量字体 1-2 px 偏移可接受 |
| 日志 | 独立文件日志，5 级 + ini 开关 | 不污染 PuTTY Event Log；可定位高频问题 |
| 日志路径 | KiTTY 二进制所在目录 `fontfallback.log` | 与 KiTTY portable 模式一致 |

## 3. 架构总览

新增独立模块 `0.76b_My_PuTTY/windows/winfont_fallback.{h,c}`，
KiTTY 主体只通过 6 个函数与其交互：

```c
/* 启动 / 清理 / 重载 */
void winfb_init(HDC hdc, const LOGFONT *primary,
                int cell_w, int cell_h,
                const char *fallback_csv,        /* ini Fallback */
                const char *override_lines[],    /* ini Override 多行 */
                int n_override);
void winfb_reset(void);          /* 主字体重建时清缓存 */
void winfb_cleanup(void);

/* 切片：把待绘文本切成同字体连续 run */
typedef struct {
    int start;      /* wchar_t 索引 */
    int len;        /* wchar_t 数（含代理对/VS 配对） */
    int slot;       /* -1 = primary, 0..N-1 = fallback slot */
} WinFB_Run;
int winfb_split(const wchar_t *wbuf, int len,
                WinFB_Run *out, int max_runs);

/* 多 run 绘制：替代 general_textout */
void winfb_draw_runs(HDC hdc, int x, int y, const RECT *line_box,
                     const wchar_t *wbuf, int len, const int *lpDx,
                     const WinFB_Run *runs, int nruns,
                     bool opaque,
                     bool bold, bool italic, bool underline);

/* HFONT 获取（用于其它路径如需要） */
HFONT winfb_hfont(int slot, bool bold, bool italic, bool underline);

/* 日志 */
enum winfb_log_level {
    WINFB_LOG_OFF=0, WINFB_LOG_ERROR=1, WINFB_LOG_WARN=2,
    WINFB_LOG_INFO=3, WINFB_LOG_DEBUG=4, WINFB_LOG_TRACE=5
};
void winfb_log_init(int level, const char *path /* NULL = 默认 */);
void winfb_log_close(void);
void winfb_logf(int level, const char *fmt, ...);
```

### 3.1 内部数据结构

```c
#define WINFB_MAX_SLOTS  16
#define WINFB_ATTR_DIM    8   /* bold × italic × underline */

typedef struct {
    char    name[LF_FACESIZE];
    HFONT   hfont[WINFB_ATTR_DIM];   /* lazy */
} winfb_slot;

static winfb_slot g_slots[WINFB_MAX_SLOTS];
static int        g_n_slots;
static LOGFONT    g_base_lf;
static int        g_cell_w, g_cell_h;
static HDC        g_probe_dc;        /* 兼容 DC，用于 GetGlyphIndicesW probe */

/* BMP 全量驻留缓存（64KB）
 * 取值：-2 未探测，-1 主字体含（或全 fallback 都缺，回主字体出 .notdef）
 *        0..N-1 命中 fallback slot */
static int8_t g_bmp_map[0x10000];

/* SMP 区段哈希（emoji / Nerd Font 高位 PUA） */
typedef struct { uint32_t cp; int8_t slot; } winfb_smp_ent;
static winfb_smp_ent g_smp[1024];
static int g_n_smp;

/* Override 段：用户在 ini 中强制指定的范围 */
typedef struct { uint32_t lo, hi; int slot; } winfb_override;
static winfb_override g_ovr[64];
static int g_n_ovr;
```

### 3.2 探测算法 `winfb_lookup_slot(uint32_t cp)`

1. 命中 override 范围 → 返回对应 slot。
2. 查 `g_bmp_map[cp]` / SMP hash，已知则返回。
3. 在主字体上 `GetGlyphIndicesW(g_probe_dc, &wc, 1, &gi,
   GGI_MARK_NONEXISTING_GLYPHS)`。`gi != 0xFFFF` → 写入 -1。
4. 否则依序遍历 `g_slots[0..g_n_slots)`，逐个 lazy `CreateFontIndirect`
   并 probe，第一个命中即记录。
5. 全部缺 → 写入 -1（主字体出 .notdef），避免反复探测。
6. 日志：每个首次新映射打 INFO；每次 probe 步骤打 TRACE。

### 3.3 内置默认 fallback 列表

```
Segoe UI Symbol
Segoe UI Emoji
Segoe UI Historic
Cascadia Mono
Microsoft YaHei UI
Microsoft JhengHei UI
Yu Gothic UI
Malgun Gothic
```

未安装的字体在 `winfb_init` 阶段静默跳过（`EnumFontFamiliesEx` 探测
+ WARN 级别日志）。

## 4. 渲染路径接入

`do_text_internal()` 仅修改 line ~6651 的 normal-unicode 分支：

```c
} else {
    static WCHAR *wbuf = NULL;
    static int   wlen = 0;
    if (wlen < len) { sfree(wbuf); wlen = len; wbuf = snewn(wlen, WCHAR); }
    for (int i = 0; i < len; i++) wbuf[i] = text[i];

    WinFB_Run runs[64];
    int nruns = winfb_split(wbuf, len, runs, 64);

    if (nruns == 1 && runs[0].slot < 0) {
        /* 全部主字体 → 走原 general_textout，保留 RTL / 既有 font-linking */
        general_textout(wintw_hdc, x + xoffset,
                        y - font_height*(lattr==LATTR_BOT) + text_adjust,
                        &line_box, wbuf, len, lpDx,
                        opaque && !(attr & TATTR_COMBINING));
    } else {
        winfb_draw_runs(wintw_hdc, x + xoffset,
                        y - font_height*(lattr==LATTR_BOT) + text_adjust,
                        &line_box, wbuf, len, lpDx, runs, nruns,
                        opaque && !(attr & TATTR_COMBINING),
                        (nfont & FONT_BOLD)      != 0,
                        (nfont & FONT_ITALIC)    != 0,
                        (nfont & FONT_UNDERLINE) != 0);
    }
}
```

`winfb_draw_runs` 内：

- 第一个 run：`ETO_OPAQUE` 刷背景
- 后续 run：`SetBkMode(TRANSPARENT)`，共享同一 `line_box` 作 clip 框
- 各 run 使用各自 `lpDx + run.start` 子序列（保持 cell 宽度严格不变）
- 结束 `SelectObject(hdc, fonts[nfont_primary])` 还原主字体

**不动**的代码路径：
- DBCS（`CSET_ACP`）
- DIRECT_FONT（OEM）
- RTL（`exact_textout` 内部）
- shadow bold 偏移、下划线、删除线
- 背景图（`MOD_BACKGROUNDIMAGE`）

## 5. 字符宽度与对齐策略

| 项 | 策略 |
|---|---|
| 单宽 / 双宽判定 | KiTTY 现有 `wintw_char_width`（EAW） — 不改 |
| `lpDx` | 全部 `char_width`，fallback 字体不重新量 advance |
| 横向 | `ETO_CLIPPED` 到 `line_box`，确保不越 cell |
| 居中 | 仅 `font_varpitch` 为 true 时沿用原 `TA_CENTER + xoffset = char_width/2` |
| baseline | 不调整 y；fallback HFONT 用与主字体相同 `lfHeight`、`lfWidth` |
| 高度 | `LATTR_TOP/BOT/WIDE` 仍走原 `FONT_HIGH/WIDE` 复制语义；fallback 不参与（不为 fallback 创建 `FONT_HIGH` 变体） |

## 6. 缓存与失效

| 缓存 | 容量 | 失效条件 |
|---|---|---|
| `g_bmp_map[0x10000]` | 64 KB | `winfb_reset` |
| SMP hash | ≤ 1024 项 | `winfb_reset` |
| `winfb_slot.hfont[8]` | 16 × 8 HFONT | `winfb_cleanup` 时 `DeleteObject` |
| `g_probe_dc` | 单兼容 DC | `winfb_cleanup` |

主字体重载触发点：`reset_window` → `deinit_fonts` → `init_fonts`。
在 `init_fonts` 末尾调用 `winfb_reset()` 再 `winfb_init(...)`。

## 7. 日志模块

### 7.1 级别与频次

| 级别 | 记录什么 | 典型频次 |
|---|---|---|
| ERROR | CreateFont 失败、文件 IO 失败 | 罕见 |
| WARN  | fallback 字体未安装、ini 解析错误 | 启动一次 |
| INFO  | 启动加载、首次 codepoint→slot 映射、reset 触发 | 启动 + 偶发 |
| DEBUG | 每次 `winfb_split` 切片摘要 | 每帧多次（默认关） |
| TRACE | 每个 codepoint 的 probe 路径 | 极高频（仅排错） |

### 7.2 默认行为

- 默认 `Log=off`，整个日志模块零开销（hot-path `if (level > g_log_level) return;`）
- 默认路径：**KiTTY 可执行文件所在目录** `fontfallback.log`
  （`GetModuleFileNameA(NULL,...)` 取路径前缀拼接）
- 10 MB 触发一次滚转：`fontfallback.log` → `fontfallback.log.1`
  （单层滚转，已存在的 `.log.1` 直接覆盖；不做多代保留）

### 7.3 格式

```
2026-05-27 14:32:25.117 [INFO ] map U+23F5 -> slot 0 "Segoe UI Symbol"
2026-05-27 14:32:25.205 [DEBUG] split len=14 runs=3 [(0,7,P)(7,1,S0)(8,6,P)]
2026-05-27 14:32:25.205 [TRACE] probe U+1F600 slot0=miss slot1=hit
```

### 7.4 性能保护

- TRACE/DEBUG 下 `fflush` 改为每 64 行一次
- `winfb_logf` 第一行 inline 化的 level gate
- 日志写入用 `FILE *` + `setvbuf` BUF 模式

## 8. ini 配置

`kitty.ini` 新增 section：

```ini
[FontFallback]
; 默认 off。值: off|error|warn|info|debug|trace
Log=off

; 默认空 = KiTTY 可执行文件所在目录的 fontfallback.log
; 也支持绝对路径
LogFile=

; 在内置默认列表之后追加用户字体（逗号分隔）
; 若以 ! 开头则完全替代内置：Fallback=!MyMono,MyEmoji
Fallback=

; 强制范围映射，可重复多行
; 格式: 起点-终点:字体名  或  单点:字体名
Override=E000-F8FF:Symbols Nerd Font Mono
Override=1F600-1F64F:Segoe UI Emoji
```

读取由 `kitty.c` 在 `WinMain` 启动早期完成（在 `init_fonts` 之前），
透传给 `winfb_log_init` 与 `winfb_init`。

## 9. 修改文件清单

**新增**：

- `0.76b_My_PuTTY/windows/winfont_fallback.h`（~70 行）
- `0.76b_My_PuTTY/windows/winfont_fallback.c`（~700 行）

**修改**：

- `0.76b_My_PuTTY/windows/window.c`
  - line ~6651 normal-unicode 分支：插入 split + draw_runs
  - `init_fonts()` 末尾：`winfb_init(...)`
  - `deinit_fonts()`：`winfb_cleanup()`
  - `reset_window` 字体重建处：`winfb_reset()`
- `0.76b_My_PuTTY/windows/MAKEFILE.MINGW`：`winfont_fallback.o` 加入对象列表（cross 与 cross64 共用）
- `kitty.c`：启动阶段读取 `[FontFallback]` ini 并初始化日志
- `kitty_ini.h` / `kitty_ini.txt`：补 `[FontFallback]` 文档

## 10. 阶段拆分

| 阶段 | 内容 | 验证 |
|---|---|---|
| S1 | 新增 `winfont_fallback.{c,h}`，仅实现日志（路径/级别/滚转）+ 空桩 | cross / cross64 编译，无回归，日志按 ini 创建 |
| S2 | `winfb_lookup_slot` + GetGlyphIndicesW probe + 内置 fallback 列表 | 探测正确，日志可见 codepoint→slot |
| S3 | 接入 `do_text_internal` 主路径 | `U+23F5` / Box / Block / 常见符号正确显示 |
| S4 | ini 集成（Log、LogFile、Fallback、Override） | ini 修改即生效，覆盖与追加均工作 |
| S5 | 主字体重载钩子 `winfb_reset` | 改字体后缓存重建，无残留 |
| S6 | 测试矩阵 + 用户文档 | 覆盖所有目标字符类 |

## 11. 测试矩阵

主字体设 Consolas，输入测试串：

```
[1] ASCII:         The quick brown fox jumps over the lazy dog 0123456789
[2] Box Drawing:   ┌─┬─┐  ╔══╦══╗  ┃ ╋ ┻ ┳ ╳ ▔ ▁ ▏ ▕
[3] Block:         █ ▓ ▒ ░ ▀ ▄ ▌ ▐
[4] Geometric:     ● ○ ■ □ ▲ △ ▶ ▼ ◀ ◆ ◇ ★ ☆
[5] Misc Symbols:  ☀ ☁ ☂ ☃ ☄ ⌘ ⌥ ⌃ ⏏ ⏵ ⏸ ⏹      ← U+23F5
[6] Dingbats:      ✓ ✗ ✘ ✿ ❄ ❤ ✈ ✂
[7] Arrows:        ← ↑ → ↓ ↔ ↕ ⇐ ⇒ ⇔
[8] CJK 单宽:      「」『』、。
[9] CJK 双宽:      你好，世界！中文测试 漢字 ひらがな カタカナ 한글
[10] Surrogate:    😀 😁 😂 🤔 🦊 🚀 (单色)
[11] Nerd Font:       (Powerline)
                             (Font Awesome)
[12] Combining:    é = e + ́    ñ = n + ̃
[13] Width edge:   ▶▶▶ABC│CJK 你好│End
```

验收：

- ASCII / 主字体字符与改造前像素级一致
- `U+23F5` 从 Consolas fallback 到 Segoe UI Symbol，不再豆腐
- CJK 不退化（单/双宽分类正确）
- Nerd Font：ini 配 Symbols Nerd Font Mono 后正确显示
- Surrogate emoji：黑白，不裁切，不越右 cell
- `Log=off` 时无可观察性能影响
- `./zzy.sh cross` 与 `./zzy.sh cross64` 均编译通过

## 12. 风险与限制

| 风险 | 缓解 |
|---|---|
| fallback 字体真实 advance 与 cell 不一致 | `ETO_CLIPPED` 到 `line_box`，宽度强制 cell |
| baseline 偏差 | 同 `lfHeight`/`lfWidth` 创建 fallback，让 GDI ascent 自动对齐；接受 1-2 px |
| PUA 在不同字体语义冲突 | 默认按列表顺序首命中；用户可 `Override` 显式映射 |
| RTL 与 fallback 共存：当一段文本既含主字体 RTL 字符又含 fallback 字符时，主字体 run 将走 `winfb_draw_runs` 而非 `general_textout`，丢失 `exact_textout` 的 GCP 排版。MVP 不做特殊处理 | 文档明确限制；纯 RTL 文本（无 fallback）仍走原路径不退化 |
| 彩色 emoji 缺失 | 文档明确：MVP 单色；二阶段 DirectWrite |
| 高频日志拖性能 | 默认 OFF；INFO 仅首次新映射；DEBUG/TRACE 仅排错时打 |
| MinGW 头文件兼容 | 所用 API（`GetGlyphIndicesW` / `GGI_MARK_NONEXISTING_GLYPHS` /
  `TEXTMETRIC`）皆 MinGW-w64 标准 |
| 32/64 位编译 | 纯 GDI 无指针大小敏感；cross/cross64 同对象列表 |

## 13. 不在本设计范围

- 彩色 emoji（COLR/CBDT/sbix）
- **非 BMP 字符的真实 fallback**：MVP 路线 A 使用 `GetGlyphIndicesW`，该 API
  以 UCS-2 code unit 为单位查询字形。代理对（surrogate pair）的高/低半区
  在任何字体中都返回 `0xFFFF (.notdef)`，所以 emoji（U+1F600+）等 SMP
  字符无法成功探测到 fallback 字体，会显示为主字体的 `.notdef` 方块。
  解决方案需要切换到 Uniscribe（`ScriptItemize` + `ScriptShape`）或
  DirectWrite — 二阶段交付。
  **临时变通**：用户可在 `[FontFallback]` 中通过 `Override=` 显式映射
  特定 SMP 范围（绕过探测），例如：
  `Override=1F600-1F64F:Segoe UI Emoji`
- 复杂脚本 shaping（阿拉伯连写、印度系组合）
- 字体子像素 hinting / ClearType 控制改造
- 自动从系统字体表中"未配置时也能命中 Nerd Font"的自动发现

以上五项作为路线 C（DirectWrite）二阶段候选。
