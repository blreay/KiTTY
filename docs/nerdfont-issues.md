# KiTTY Nerd Font 显示问题分析

## 问题一：PUA 字符只显示一半

### 现象

字符 U+F115（Nerd Font 图标）在 Windows 10 文本编辑器中正常显示，但在 KiTTY 中只能显示一半，终端上另一半已占位但为空白。使用的字体为 Sarasa Fixed SC Nerd Font。

### 根因

问题出在 `wcwidth.c` 中字符宽度计算与字体实际字形宽度不匹配。

KiTTY 通过 `term_char_width()` 确定字符在终端网格中占几个单元格，该函数调用 `mk_wcwidth()` / `mk_wcwidth_cjk()` 查询静态 Unicode 宽度表。PUA（Private Use Area）字符没有标准宽度定义，其宽度完全取决于字体中字形的实际 advance width，但静态表无法反映这一点。

**非 CJK 模式下的流程：**

1. `mk_wcwidth(0xF115)` 返回 **1**（PUA 不在 `wide[]` 表中，`wcwidth.c:184-301`）
2. 终端为该字符分配 1 个单元格
3. 渲染时 `wintw_char_width()` 通过 `GetCharWidth32W()` 查询字体，Nerd Font 图标的 advance width 为 2 倍字宽，返回 **2**
4. `terminal.c:6965-6967` 检测到 `ATTR_WIDE` 未设置但 `win_char_width()` 返回 2，于是设置 `ATTR_NARROW`
5. `window.c:6251-6252` 将 `FONT_NARROW` 加入字体选择，`window.c:2857-2858` 将字体宽度压缩为 `(font_width+1)/2`
6. 图标被水平压缩到 1 个单元格内，显示为"只显示一半"

**CJK 模式下的流程：**

1. `mk_wcwidth_cjk(0xF115)` 返回 **2**（PUA 在 `ambiguous[]` 表中，`wcwidth.c:524: {0xE000, 0xF8FF}`）
2. 终端为该字符分配 2 个单元格，第二个单元格填入 `UCSWIDE` 标记
3. 如果字体中该字形实际是单宽的，图标只填充第一个单元格，第二个单元格（UCSWIDE）为空白
4. 视觉效果为"另一半已占位但为空"

### 修复

修改 `term_char_width()` 对 PUA 字符使用字体实际宽度（通过 `win_char_width()` → `GetCharWidth32W()` 查询），并在显示循环中跳过 PUA 字符的 `ATTR_NARROW` 压缩。

**问题分析：**

仅查询字体 advance width（返回 2）会分配 2 个单元格并设置 `ATTR_WIDE`，但显示循环检测到字体宽度=2 且 `ATTR_WIDE` 未设置时会添加 `ATTR_NARROW`，将字形压缩到一半宽度。这导致图标只显示左半部分。

**修复方案：**

1. `term_char_width()` 对 PUA 字符使用字体实际宽度（`win_char_width()`），返回 2 时分配 2 个单元格
2. 显示循环中对 PUA 字符跳过 `ATTR_NARROW`，避免字形被压缩
3. 图标以原始大小在 2 个单元格内渲染，`ATTR_WIDE` 确保 `char_width = 2 * font_width`

**涉及文件：**

| 文件 | 修改内容 |
|------|---------|
| `0.76b_My_PuTTY/terminal/terminal.c` | `term_char_width()` PUA 查询字体宽度；`ATTR_NARROW` 检查跳过 PUA |
| `0.76_My_PuTTY/terminal.c` | 同上 |
| `0.76b_My_PuTTY/stubs/noterm.c` | `term_char_width()` 无 font 查询能力，回退到 wcwidth |

**核心修改逻辑：**

```c
int term_char_width(Terminal *term, unsigned int c)
{
    /* PUA 字符使用字体实际宽度 */
    if (term && term->win &&
        ((c >= 0xE000 && c <= 0xF8FF) ||    /* BMP PUA */
         (c >= 0xF0000 && c <= 0xFFFFD) ||   /* Supplementary PUA-A */
         (c >= 0x100000 && c <= 0x10FFFD))) { /* Supplementary PUA-B */
        int font_w = win_char_width(term->win, c);
        if (font_w > 0)
            return font_w;
    }
    if (term)
        return term->cjk_ambig_wide ? mk_wcwidth_cjk(c) : mk_wcwidth(c);
    return mk_wcwidth(c);
}

/* 显示循环中，跳过 PUA 字符的 ATTR_NARROW 压缩 */
if ((tattr & ATTR_WIDE) == 0 &&
    win_char_width(term->win, tchar) == 2 &&
    !((tchar >= 0xE000 && tchar <= 0xF8FF) ||
      (tchar >= 0xF0000 && tchar <= 0xFFFFD) ||
      (tchar >= 0x100000 && tchar <= 0x10FFFD)))
    tattr |= ATTR_NARROW;
```

### 相关代码路径

```
字符输入 → term_display_graphic_char() [terminal.c:4100]
         → term_char_width() [terminal.c:4081]
         → PUA 字符查询 win_char_width() 返回字体实际宽度
         → 非 PUA 字符走 mk_wcwidth() / mk_wcwidth_cjk()

渲染时   → terminal.c:6984 检查 win_char_width() 与 ATTR_WIDE
         → PUA 字符跳过 ATTR_NARROW（不压缩）
         → ATTR_WIDE 时 char_width *= 2，字形在 2 单元格内渲染
         → 非 PUA 双宽字符正常设置 ATTR_NARROW
         → window.c:6251 FONT_NARROW 字体宽度压缩为 (font_width+1)/2
```

---

## 问题二：Nerd Font 字体不出现在字体选择对话框

### 现象

字体 "Consolas Nerd Font Mono zzy" 在 Windows 10 记事本的字体对话框中可以选择，但在 KiTTY 的字体选择对话框中不显示。

### 根因

KiTTY 的字体对话框使用了 `CF_FIXEDPITCHONLY` 标志，只列出等宽字体。

**关键代码（`controls.c:2049`）：**

```c
cf.Flags = (dp->fixed_pitch_fonts ? CF_FIXEDPITCHONLY : 0) |
           CF_FORCEFONTEXIST | CF_INITTOLOGFONTSTRUCT | CF_SCREENFONTS;
```

`fixed_pitch_fonts` 默认为 `true`（`controls.c:2666`），因此 `CF_FIXEDPITCHONLY` 生效。

**Windows 的等宽判定机制：**

Windows 通过 `TEXTMETRIC.tmPitchAndFamily` 的 `TMPF_FIXED_PITCH` 位判断字体是否等宽。该位定义是**反的**：

- `TMPF_FIXED_PITCH` 位 **置1** → 变宽字体（variable-pitch）
- `TMPF_FIXED_PITCH` 位 **清0** → 等宽字体（fixed-pitch）

代码中也有注释说明（`window.c:2481`, `window.c:2623`）：

```c
/* Note that the TMPF_FIXED_PITCH bit is defined upside down :-( */
```

**Nerd Font 被判定为变宽的原因：**

Nerd Font 补丁在字体中添加了大量 PUA 图标字形，这些图标的 advance width 通常是普通字符的 **2 倍**。因此字体中存在两种不同的 advance width（1x 和 2x），Windows 将 `TMPF_FIXED_PITCH` 位置 1，归类为变宽字体。

即使字体名带 "Mono"（如 "Consolas Nerd Font Mono"），只要字体内部存在不同 advance width 的字形，Windows 就会将其标记为 variable-pitch，`CF_FIXEDPITCHONLY` 就会将其过滤掉。

**记事本能显示的原因：**

Windows 记事本的字体对话框不使用 `CF_FIXEDPITCHONLY` 标志，显示所有字体，因此 Nerd Font 能正常出现。

### 解决方法

在 KiTTY 配置界面中：**Window → Appearance → Font settings**，勾选 **"Allow selection of variable-pitch fonts"**，然后重新打开字体选择对话框，Nerd Font 字体就会出现在列表中。

勾选后 `fixed_pitch_fonts` 变为 `false`，`CF_FIXEDPITCHONLY` 不再设置，字体对话框将显示所有已安装字体。

### 相关代码路径

```
配置界面 → ctrl_checkbox("Allow selection of variable-pitch fonts") [config.c:302]
         → variable_pitch_handler() [config.c:32-39]
         → dlg_set_fixed_pitch_flag(dlg, false)

字体对话框 → ChooseFont(&cf) [controls.c:2052]
           → cf.Flags 含 CF_FIXEDPITCHONLY [controls.c:2049]
           → Windows 仅列出 TMPF_FIXED_PITCH 位为 0 的字体

字体加载 → CreateFont() [window.c:2627]
         → GetTextMetrics() 检查 tmPitchAndFamily [window.c:2624]
         → TMPF_FIXED_PITCH 位判断等宽/变宽 [window.c:2624-2629]
```
