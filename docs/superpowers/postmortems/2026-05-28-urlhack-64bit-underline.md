# 64-bit Underline 异味事件：urlhack regexec 栈越界

- 日期：2026-05-28
- 类型：跨架构 (32-bit OK / 64-bit BAD) 渲染异常
- 影响：64-bit KiTTY 上，只要屏幕上出现一个 URL，URL 之后所有屏幕格子都被打上下划线
- 修复 commit：`c1ff448` —— *fix(urlhack): allocate enough regmatch_t slots to keep regexec from clobbering the stack*
- 相关文件：`url/urlhack.c`、`0.76b_My_PuTTY/terminal/terminal.c`

---

## 1. 症状

**用户报告**：

> 64位运行的时候，我在一个正常的窗口里，只要输入 `http://w` 满屏幕就全是下划线了，但是32位就不会。和屏幕宽度没有关系，设置多宽都一样，也和折行没有关系。

复现要点：

- 只在 **64-bit** 二进制 (`kitty64.exe`) 触发，32-bit (`kitty.exe`) 永远不出
- 触发字符串可短至 8 字符（`http://w`）
- 跟窗口宽度、终端折行、字体设置无关
- URL 自身的下划线是正确的；问题在 URL **之后**所有屏幕格子（包括空白）也被加了下划线，直到该行被新输出覆盖

## 2. 初次误判（先记录这条岔路）

我最初看到 `terminal.c:6905` 一处明显的 off-by-one：

```c
// 错的旧代码
if (urlhack_toggle_x == term->cols - 1) {
    // Handle special case where link ends at the last char of the row
    urlhack_toggle_y++;
    urlhack_toggle_x = 0;
}
```

URL 区域的 `region.x1` 是**半开区间** (`[x0, x1)`)，所以 URL 覆盖到屏幕最右一格时 `x1 == cols`，而不是 `cols - 1`。于是改成了 `>= cols`（commit `b0b31a0`）。

这个改动在客观上是正确的，但**并不是当前 bug 的原因**。修完之后用户继续报：同样的 bug，只是输入 `http://w` 就触发。

教训：拿到一个 bug 报告，先尽量复现、再下手；不要看到第一个长得像 bug 的代码就动手。

## 3. 重新分析：为什么 32-bit 没事

"32-bit 正常 / 64-bit 异常"是非常强的信号，候选根因被收窄到这几类：

| 类别 | 排查结果 |
|---|---|
| 编译器警告差异 | 用 `gcc -Wall` 跑 `cross64`，`urlhack.c` 和 `terminal.c` 均无新增警告 |
| `int` / `long` / `size_t` 类型差异 | Win64 是 **LLP64**：`int=4`、`long=4`、`pointer=8`、`size_t=8`。bug 区域涉及 `unsigned long tchar`（4 字节，两个 arch 一致）、`int urlhack_toggle_x/y`、`int x0/y0/x1/y1`、`unsigned int link_regions_*` —— **没有**类型尺寸的真实差异 |
| 库 ABI 差异 | `regex/libregex.a`、`regex/libregex_64.a` 同源编译，`regex_t` / `regmatch_t` / `regoff_t` 在两个头文件中定义完全一致 |
| 我自己引入的 font fallback 改动 | T8 集成完全没碰 URL 高亮路径，且 `winfb_split` 返回单 primary run 时走原路径 —— 排除 |
| 优化级别差异 | 两个 arch 都是 `-O2`、CFLAGS 完全一样 —— 排除 |
| 栈布局差异 | **保留作为唯一仍可能的解释** |

剩下的只能是**栈布局**或**内存覆盖**。这种 bug 的典型剧本：UB 在 32-bit 上"碰巧不踩坏关键位"，64-bit 上栈帧重排后正好踩中了载荷局部。

## 4. 找到根因

排查 `url/urlhack.c` 中所有 regex 相关调用点，发现 `urlhack_go_find_me_some_hyperlinks` (line 266) 的核心循环：

```c
// 旧代码
char* text_pos = window_text;
regmatch_t groupArray;                                 // ← 单个 regmatch_t 在栈上
int error;
error = regexec(&urlhack_rx, text_pos, 1, &groupArray, 0);
while (error == 0) {
    // ... 用 groupArray.rm_so / rm_eo 算 link region ...
    text_pos = text_pos + groupArray.rm_eo + 1;
    error = regexec(&urlhack_rx, text_pos, 1, &groupArray, REG_NOTBOL);
}
```

参数：`nmatch = 1`、`pmatch = &groupArray`（指向**一个** `regmatch_t`，约 16 字节，两个 `int` 偏移）。

但是当前编译的 URL 默认正则（`kitty_commun.c:11`）：

```
((ht|f)tp(s?):\/\/[0-9a-zA-Z]([-\.\w]*[0-9a-zA-Z])*([:][0-9]+)?\/?...)
|(mailto:[a-zA-Z0-9\-_\.]+@[a-zA-Z0-9\-_\.]+\.[a-z]{2,})
|(ssh:\/\/([-a-zA-Z0-9_]+([:][^@]*)?@)?[-a-zA-Z0-9_\.]+(:[0-9]{2,5})?(\/[-a-zA-Z0-9_]+)?)
```

里面 `(` 形式的捕获组**超过 13 个**。`regcomp` 编译后 `re_nsub` 是个不小的值。

POSIX 的规定是：
> The application shall ensure that the pmatch argument points to an array with at least nmatch elements. The regexec() function shall fill in the structure pointed to by pmatch.

按照规范，`nmatch=1` 时 regexec 只应该填 `pmatch[0]`。**但仓库捆绑的 GNU regex 实现并不严格遵守这条**——在某些路径里它会按 `re_nsub` 全量写入 register 数组。

这就是 bug 的真实根因：

- regexec 越过 `&groupArray` 写到栈上更高位置
- 在 64-bit 下，恰好踩到外层的 `error` 变量、`text_pos` 指针、或循环的别的相关栈位
- 这些被踩花的局部把 while 循环带进了"幽灵 link"状态：
  - `urlhack_add_link_region` 被反复调用，往 `link_regions` 数组里塞错误的 `(x0, y0, x1, y1)`
  - 或者 `text_pos` 指针跳到了错误位置，下一次匹配在屏幕的随机位置又"命中"
- 下游 `terminal.c` 的 toggle 状态机 (`urlhack_toggle_x/y` ↔ `urlhack_is_link`) 拿到的"链接区域"已经不可信
- 结果就是 `urlhack_is_link` 翻转后再也没机会翻回 0，后续所有屏幕格子被 `tattr |= ATTR_UNDER`

为什么 32-bit 不出问题：32-bit 编译的栈帧布局不同，`groupArray` 之外的栈位置塞的是**对 URL 检测逻辑无影响**的局部变量（也许是某个尚未被写入的临时变量，或者是上一帧的残留），所以写坏了也"恰巧没事"。这就是典型的 *"UB 但碰巧不崩"*。

## 5. 修复

`url/urlhack.c` 中把单变量改成定长数组，把 `nmatch` 拉到与之相同的容量，库就有合法空间把它想写的全写完：

```c
// 新代码
char* text_pos = window_text;
/*
 * Allocate enough match slots for the whole-match group AT LEAST,
 * but the bundled GNU regex implementation may write past `nmatch`
 * when the compiled pattern has many subexpressions (the urlhack
 * default regex has well over a dozen capture groups).  A single
 * regmatch_t on the stack used to be clobbered by adjacent stack
 * variables on 64-bit builds, which manifested as the URL-underline
 * state machine going haywire and painting every subsequent cell
 * with ATTR_UNDER.  Provide a generous fixed buffer that covers the
 * built-in regexes; if a user-supplied regex needs more, we still
 * only inspect [0].
 */
regmatch_t groupArray[32];
int error;
error = regexec(&urlhack_rx, text_pos, 32, groupArray, 0);
while (error == 0) {
    // ... 现在改用 groupArray[0].rm_so / rm_eo
    text_pos = text_pos + groupArray[0].rm_eo + 1;
    error = regexec(&urlhack_rx, text_pos, 32, groupArray, REG_NOTBOL);
}
```

关键点：

- 32 个 `regmatch_t` 远多于内置正则需要的捕获组数，提供安全余量
- 代码逻辑**完全没变**——仍然只读 `groupArray[0]`（整体匹配的 `rm_so/rm_eo`），其他捕获组依然忽略
- 没有改变任何外部 API，没有改变任何状态机逻辑
- 改动局限在一个 .c 文件、22 行

## 6. 验证

- `./zzy.sh cross`（i686-w64-mingw32）：编译通过，无新增 warning，输出 `kitty.exe`
- `./zzy.sh cross64`（x86_64-w64-mingw32）：编译通过，无新增 warning，输出 `kitty64.exe`
- 用户在 64-bit 二进制里复测 `echo http://www.baidu.com` 和 `http://w`：预计下划线行为应只出现在 URL 区域，URL 之后所有空白格子保持无下划线

## 7. 教训

1. **跨架构 bug 的标准嫌疑名单**：编译警告 → 类型尺寸（`long`/`pointer`/`size_t`）→ ABI → 优化级别 → **栈布局 / UB**。如果前几样都正常，UB 是最后的合理解释。

2. **`regmatch_t buf[1]; regexec(..., 1, buf, ...)` 是一个反模式**。即使 POSIX 规定"只该写 pmatch[0]"，给一个**合理大小**的固定缓冲区是更便宜也更安全的写法。GNU regex 不是唯一可能"不规矩"的实现，其它移植的实现（uClibc、TRE、Henry Spencer 版本）历史上都有类似毛病。

3. **"我刚改了 X，所以 bug 是 X 引入的"是错误推理**。本 bug 跟 font fallback 改动毫无关系，是一个仓库历史里就存在的潜伏问题。用户的上一个 commit (`fead573`) 标题里就写了 "**32bit and 64bit build OK, works fine, except the underline issue in 64bit**"，已经感知到这个问题存在，只是没找到根因。

4. **第一个看起来像 bug 的代码不一定是当前 bug 的成因**。`terminal.c:6905` 的 off-by-one 是真 bug、修复也是对的，但跟用户报的现象无关。先尝试复现、再下手，能少走弯路。

## 8. 后续可考虑

- 全仓库扫一遍其它 `regexec(...., 1, ...)` 的调用点，看是否还有类似坑（搜 `regexec` 仅一处，本次已修）
- 评估是否升级 `regex/` 子项目到一个**严格遵守 POSIX nmatch 语义**的现代实现（如 system libc 的 regex），但要先确认 MinGW-w64 是否原生提供
- 64-bit 二进制建议常开 `-fstack-protector-strong`，类似栈越界 UB 在编译期/运行期更容易被发现
