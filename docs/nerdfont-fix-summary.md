# Nerd Font 图标只显示左半部分的修复说明

## 问题现象

使用 "Sarasa Fixed SC Nerd Font" 字体时，Nerd Font 图标字符（如 ）只显示左半部分，右半部分留白。字符宽度看起来正确（占2个cell），但只有第一个cell被绘制，第二个cell为空白。

使用 "Consolas Nerd Font Mono" 时显示正常。

## 根本原因

### 时序问题：`wintw_hdc` 在字符接收时为 NULL

KiTTY 中有一个全局 HDC 变量 `wintw_hdc`（`window.c:559`），它 **仅在绘制期间有效**：

| 时机 | `wintw_hdc` 状态 |
|------|-----------------|
| WM_PAINT 处理期间 | 有效（`= hdc from BeginPaint`） |
| `term_update()` → `do_paint()` 期间 | 有效（`= make_hdc()`） |
| SSH 数据到达、处理字符时 | **NULL** |

### 完整故障链路

```
SSH数据到达
  → term_display_graphic_char()
    → term_char_width(term, c)           // 确定字符占几个cell
      → win_char_width(term->win, c)     // 对PUA字符查询字体宽度
        → wintw_char_width(tw, uc)
          → SelectObject(wintw_hdc, ...)  // wintw_hdc = NULL!
          → GetCharWidth32W(NULL, ...)    // 失败，返回0
          → return 0                     // 宽度查询失败
      → font_w = 0, 不满足 > 0
      → 回退到 mk_wcwidth(c)            // PUA不在wide表中，返回1
    → width = 1                          // 错误！应该是2
  → 字符以 width=1 存储（无 ATTR_WIDE，无 UCSWIDE）
```

然后在渲染时（`wintw_hdc` 有效了）：

```
do_paint()
  → d[1].chr != UCSWIDE                  // 因为存储时没有放UCSWIDE
  → ATTR_WIDE 不被设置
  → char_width = font_width              // 只有1个cell的像素宽度
  → line_box.right = x + font_width      // 裁剪区域只有1个cell
  → ExtTextOutW(..., ETO_CLIPPED, &line_box, ...)
  → 字体以自然宽度(2×font_width)渲染字形
  → ETO_CLIPPED 将输出裁剪到 line_box
  → 结果：只显示左半部分！
```

### 为什么 "Consolas Nerd Font Mono" 不受影响

"Mono" 变体的图标字形被缩放为单宽（1个cell）。无论字体查询是否成功，`mk_wcwidth()` 返回1，字形也确实是1个cell宽，不存在裁剪问题。

### 之前修复为何无效

之前的修复在 `do_paint()` 中为 PUA 字符跳过了 `ATTR_NARROW` 压缩，意图是让图标不被压缩。但这实际上让情况 **更糟**：

| 方案 | 行为 | 视觉效果 |
|------|------|---------|
| 原始代码（设置 ATTR_NARROW） | 字形被压缩到半宽 | 图标完整但被挤压变形 |
| 之前的修复（跳过 ATTR_NARROW） | 字形以原始2x宽度渲染，但被裁剪 | **只显示左半部分** |
| 正确修复（HDC修复） | 字符正确存储为width=2 | 图标完整显示 |

## 代码修改

### 1. 核心修复：`wintw_char_width()` 创建临时 HDC

**文件**: `0.76b_My_PuTTY/windows/window.c`, `0.76_My_PuTTY/windows/window.c`

当 `wintw_hdc` 为 NULL 时，通过 `make_hdc()`（即 `GetDC(term_hwnd)`）创建临时 HDC，完成字体宽度查询后释放。

```c
static int wintw_char_width(TermWin *tw, int uc)
{
    int ibuf = 0;
    HDC hdc;
    bool need_free = false;

    if (!font_dualwidth &&
        !((uc >= 0xE000 && uc <= 0xF8FF) || ...))
        return 1;

    // 核心修复：wintw_hdc 为 NULL 时创建临时 HDC
    hdc = wintw_hdc;
    if (!hdc) {
        hdc = make_hdc();
        if (!hdc)
            return 0;
        need_free = true;
    }

    // ... 使用 hdc 代替 wintw_hdc 进行宽度查询 ...

    if (need_free) free_hdc(hdc);

    ibuf += font_width / 2 - 1;
    ibuf /= font_width;
    return ibuf;
}
```

### 2. 回退 `do_paint()` 中的 PUA ATTR_NARROW 排除

**文件**: `0.76b_My_PuTTY/terminal/terminal.c`, `0.76_My_PuTTY/terminal.c`

移除了之前为 PUA 字符跳过 ATTR_NARROW 的逻辑，恢复为原始简洁代码：

```c
if ((tattr & ATTR_WIDE) == 0 &&
    win_char_width(term->win, tchar) == 2)
    tattr |= ATTR_NARROW;
} else if (term->disptext[i]->chars[j].attr & ATTR_NARROW)
    tattr |= ATTR_NARROW;
```

**原因**：HDC 修复后，PUA 字符在输入时就正确获得 width=2，存储时设置了 ATTR_WIDE。渲染时 `(tattr & ATTR_WIDE) == 0` 为 FALSE，根本不会进入这个检查。即使在极端情况下 HDC 创建失败，保留 ATTR_NARROW 也能提供更好的降级体验（压缩显示完整图标，而非裁剪显示一半）。

## 修改文件清单

| 文件 | 修改内容 |
|------|---------|
| `0.76b_My_PuTTY/windows/window.c` | `wintw_char_width()` 添加临时 HDC 逻辑 |
| `0.76_My_PuTTY/windows/window.c` | 同上 |
| `0.76b_My_PuTTY/terminal/terminal.c` | 回退 ATTR_NARROW PUA 排除，更新注释 |
| `0.76_My_PuTTY/terminal.c` | 同上 |
