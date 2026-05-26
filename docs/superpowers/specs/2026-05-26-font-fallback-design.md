# KiTTY Font Fallback 设计文档

**日期**: 2026-05-26  
**分支**: KiTTY.font.fallback.worktree  
**目标**: 在 KiTTY 现有 GDI cell-based 渲染架构上，通过 DirectWrite 字体 fallback 选择 + GDI 绘制的混合方案，解决主字体缺失 PUA（Nerd Font）和 Box Drawing 字符时的显示问题。

---

## 1. 现状分析

### 1.1 当前字体系统

KiTTY 的字体以 `fonts[]` 数组管理（`window.c:221`），最多 `FONT_MAXNO`（~64–80）个槽，**全部是同一主字体的变体**（normal / bold / underline / italic / wide / OEM），由 `another_font(fontno)` 按需懒创建。没有任何切换到不同字体族的机制。

### 1.2 渲染路径

```
wintw_draw_text()
  └─ do_text_internal()
       ├─ another_font(nfont)          # 选主字体变体
       ├─ SelectObject(hdc, fonts[nfont])
       └─ general_textout()            # LTR → ExtTextOutW
          exact_textout()              # RTL → GetCharacterPlacementW + ETO_GLYPH_INDEX
```

`general_textout()` 的 LTR 路径用 `ExtTextOutW()`，Windows 会做内置字体链接（font linking），但 KiTTY 无法控制选哪个字体，也无法干预 cell 宽度。`exact_textout()` 用 `ETO_GLYPH_INDEX`，**明确破坏了 Windows 字体链接**（代码注释有说明）。

### 1.3 字符宽度计算

`term_char_width()` → PUA 字符调 `win_char_width()` → `wintw_char_width()` → `GetCharWidth32W(主字体)`；失败则 fallback 到 `mk_wcwidth()`（PUA 返回 1）。非 PUA 纯走 `mk_wcwidth()` / `mk_wcwidth_cjk()` Unicode 表。

### 1.4 当前限制

- 主字体没有某字符时，没有任何机制自动切换到其他字体
- PUA 字符宽度查询只查主字体；本 branch 已修复 HDC 时序问题，但若主字体根本没有该 glyph，宽度仍会错误返回 1
- Box Drawing / Block Elements 若主字体缺失，显示豆腐块，无 fallback

---

## 2. 设计方案

### 2.1 方案选择

**选定方案：DirectWrite 选字体 + GDI 绘制（路线 B）**

- 用 `IDWriteFontFallback::MapCharacters()` 确定每个 codepoint 应使用哪个字体
- 用 `IDWriteGdiInterop::ConvertFontToLOGFONT()` 转成 `HFONT`
- 绘制仍走现有 `ExtTextOutW()` GDI 路径，cell 约束逻辑完全不变
- 最低要求：Windows 10+（`IDWriteFactory2` 保证可用）

**未选方案对比**:

| 方案 | 问题 |
|---|---|
| A. 纯 GDI 手工 fallback | `GetGlyphIndicesW` 不支持代理对；无系统 fallback 知识；需手动枚举加载字体 |
| C. 全量 DirectWrite | 架构改动巨大；GDI/DWrite 混用复杂；cell 网格与 proportional 布局存在根本阻抗 |

### 2.2 MVP 范围

第一阶段只覆盖：
- **PUA / Nerd Font**（U+E000–F8FF, U+F0000–FFFFD, U+100000–10FFFD）
- **Box Drawing + Block Elements**（U+2500–259F）

CJK、彩色 Emoji 不在 MVP 范围内。

---

## 3. 架构

### 3.1 新模块：`kitty_fontfallback.c/.h`

职责：给定 Unicode codepoint，返回应使用的 `HFONT` 及该 glyph 像素宽度。

```
┌─────────────────────────────────────────────────────┐
│  do_text_internal()  (window.c)                     │
│                                                     │
│  for each character run:                            │
│    fb = kff_lookup(codepoint)    ← 新增             │
│    if fb.hfont != NULL:                             │
│      SelectObject(hdc, fb.hfont)                    │
│      ExtTextOutW(...)            ← 现有绘制路径     │
│    else:                                            │
│      SelectObject(hdc, fonts[nfont])                │
│      general_textout(...)        ← 原有逻辑         │
└─────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────┐
│  kitty_fontfallback.c                               │
│                                                     │
│  kff_init()    → DWrite factory + fallback 初始化  │
│  kff_lookup()  → codepoint → KffResult             │
│  kff_char_width() → codepoint → cell 宽度（1或2）  │
│  kff_deinit()  → 释放所有资源                      │
└─────────────────────────────────────────────────────┘
```

### 3.2 数据结构

```c
/* 公开类型 */
typedef struct {
    HFONT hfont;    /* NULL = 用主字体 */
    int   glyph_px; /* glyph 实际像素宽度，0 = 未知 */
} KffResult;

/* 内部缓存（开放寻址，4096 桶） */
typedef struct {
    unsigned int codepoint;  /* 0 = 空槽 */
    KffResult    result;
} KffCacheEntry;

/* HFONT 池：相同 LOGFONT 复用 */
typedef struct KffFontNode {
    LOGFONT           lf;
    HFONT             hfont;
    struct KffFontNode *next;
} KffFontNode;

/* 模块单例状态 */
typedef struct {
    IDWriteFactory2     *dw_factory;
    IDWriteFontFallback *dw_fallback;
    IDWriteGdiInterop   *dw_gdi_interop;

    KffCacheEntry cache[4096];
    KffFontNode  *font_pool;

    wchar_t user_fonts[8][LF_FACESIZE]; /* INI 读入，优先级最高 */
    int     user_font_count;
    bool    initialized;
} KffState;
```

**缓存失效条件**：仅在 `kff_init()` 重新调用时（`init_fonts()` 触发，即字体配置改变）。

---

## 4. 核心调用链

### `kff_lookup(codepoint)`

```
1. 查缓存 → 命中则返回

2. GetGlyphIndicesW(fonts[FONT_NORMAL], wch, len, &idx, GGI_MARK_NONEXISTING_GLYPHS)
   idx != 0xFFFF → 主字体有该字形 → 缓存 {NULL,0}，返回

3. 对 PUA 字符：
   按序试 user_fonts[] → kff_builtin_fonts[] 中的 Nerd Font 变体
   找到有该 glyph 的字体 → 跳到步骤 5

4. 对非 PUA（Box Drawing 等）：
   IDWriteFontFallback::MapCharacters(wch, len, locale, ...) → IDWriteFont*
   mappedFont == NULL → 缓存 {NULL,0}，返回

5. IDWriteGdiInterop::ConvertFontToLOGFONT(mappedFont, &lf)
   lf.lfHeight  = font_height   /* 与主字体对齐 */
   lf.lfWidth   = 0
   lf.lfQuality = 与主字体相同
   hfont = kff_pool_get_or_create(&lf)

6. GetCharWidth32W(tmp_hdc, codepoint, codepoint, &glyph_px)

7. 缓存 {hfont, glyph_px}，返回
```

### `do_text_internal()` 修改

在构造 `wbuf[]` 后、调 `general_textout()` 前，按"是否同一 fallback 字体"分段：

```c
/* 伪代码 */
int seg_start = 0;
HFONT seg_font = fonts[nfont];
for (int i = 0; i <= len; i++) {
    HFONT cur_font = (i < len) ? kff_resolve_hfont(wbuf[i], nfont) : NULL;
    if (cur_font != seg_font || i == len) {
        /* 绘制 [seg_start, i) 段 */
        int xoff = compute_xoffset(seg_font, wbuf+seg_start, i-seg_start);
        SelectObject(hdc, seg_font);
        ExtTextOutW(hdc, x + xoff, y, ETO_CLIPPED|ETO_OPAQUE,
                    &seg_box, wbuf+seg_start, i-seg_start, lpDx+seg_start);
        /* bold-shadow 随段处理 */
        seg_start = i;
        seg_font  = cur_font;
    }
}
```

`kff_resolve_hfont()` 是 `kff_lookup()` 的轻量包装，对主字体有该字形的字符直接返回 `fonts[nfont]`。

---

## 5. Cell 宽度策略

```
权威来源：term_char_width()（字符入屏时决定，存为 ATTR_WIDE）
  PUA:     GetCharWidth32W(主字体) → 成功则用
           主字体无该字形 → kff_char_width(fallback字体) → 成功则用
           全失败 → mk_wcwidth()
  非 PUA:  mk_wcwidth() / mk_wcwidth_cjk()

渲染时：
  glyph_px > cell_width  → ETO_CLIPPED 裁剪（不改变占格数）
  glyph_px < cell_width  → xoffset = (cell_width - glyph_px) / 2 居中
  glyph_px ≈ cell_width  → xoffset = 0
```

Box Drawing 永远单宽（mk_wcwidth 返回 1），fallback 字体的这类 glyph 也是单宽设计，不会出现占格数判断错误。

---

## 6. 配置

### INI 配置项

```ini
FallbackFont0=Sarasa Fixed SC Nerd Font
FallbackFont1=JetBrainsMono Nerd Font Mono
; 最多 8 条，追加到内置列表头部
```

### 内置 fallback 列表（按优先级）

```c
static const wchar_t *kff_builtin_fonts[] = {
    L"Symbols Nerd Font Mono",
    L"Symbols Nerd Font",
    L"Segoe UI Symbol",
    L"Segoe UI Emoji",
    L"Cascadia Mono",
    L"Consolas",
    L"Microsoft YaHei",
    L"SimSun",
    NULL
};
```

内置列表用于 PUA 字符的显式试探；非 PUA 字符的 fallback 由 `MapCharacters()` 系统决策。

---

## 7. 改动文件清单

| 文件 | 改动 | 估计行数 |
|---|---|---|
| `kitty_fontfallback.c`（新建） | DWrite 初始化、`kff_lookup()`、缓存、HFONT 池 | ~300 |
| `kitty_fontfallback.h`（新建） | `KffResult`、公开 API | ~40 |
| `0.76b_My_PuTTY/windows/window.c` | `init_fonts()` + `deinit_fonts()` 接入；`do_text_internal()` 分段逻辑 | ~70 |
| `0.76b_My_PuTTY/terminal/terminal.c` | `term_char_width()` PUA 分支补 `kff_char_width()` | ~10 |
| `0.76b_My_PuTTY/windows/MAKEFILE.MINGW` | 加 `kitty_fontfallback.o`，链接 `-ldwrite` | ~3 |
| `kitty_settings.c` + `kitty_ini.h` | `FallbackFont0`…`7` 读写 | ~20 |

---

## 8. 风险与对策

| 风险 | 对策 |
|---|---|
| DWrite 初始化失败 | `kff_init()` 失败时设标志位，`kff_lookup()` 返回 `{NULL,0}`，完全降级到现有行为 |
| HFONT 池生命周期 | `kff_deinit()` 在 `deinit_fonts()` **末尾**调用，避免悬空句柄 |
| bold-shadow 路径遗漏 | 分段后每段独立处理 shadow 重绘，随主绘制逻辑复制 |
| PUA 宽度查询在 paint 外调用 | `kff_char_width()` 内部用 `GetDC(NULL)` 临时 HDC，与现有 `wintw_char_width()` 修复一致 |
| 线程安全 | KiTTY 单线程 Win32 消息循环，hash table 无需加锁 |
| 字体不存在时的 PUA 探测性能 | 首次探测结果缓存；未安装的字体 `GetGlyphIndicesW` 失败快速跳过 |

---

## 9. 验收标准

- 使用 Consolas 主字体时，`U+23F5`（▶）能自动从系统字体 fallback 显示
- 常见 Box Drawing 字符（`─ │ ┼ ╔ ╗`）不因 fallback 而错位
- 安装了 Nerd Font 时，PUA 图标能完整显示（不再出现半截或豆腐块）
- CJK 与 ASCII 的单宽/双宽行为不比当前版本更差
- DWrite 不可用时行为与当前版本完全一致

---

## 10. 未覆盖的已知限制

- 彩色 Emoji（COLR/CBDT 字体）：GDI 不支持彩色字体，需全量 DirectWrite 才能解决
- 复杂 RTL shaping（阿拉伯文、天城文）：`exact_textout()` 路径未修改
- CJK 在 MVP 阶段不主动 fallback（`mk_wcwidth` 已覆盖宽度，显示依赖主字体是否含 CJK）
