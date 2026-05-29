# 64-bit Underline Bleed / SSH Segfault 终极排查：regex 库 ABI 不匹配

- 日期：2026-05-29
- 类型：跨架构 (32-bit OK / 64-bit BAD) 静默内存越界，最终触发渲染异常 / 段错误
- 影响：64-bit `kitty64.exe` 在显示任何 URL 之后整片屏幕被打下划线；某些 Ubuntu 版本上 SSH 登录直接 segfault
- 真正修复 commit：`5c0a369` — `fix(regex): make 64-bit re_pattern_buffer / regmatch_t layout match the bundled GNULIB libregex_64.a`
- 相关文件：`regex/regex.h`、`regex/libregex_64.a`、`url/urlhack.c`、`0.76b_My_PuTTY/terminal/terminal.c`
- 前置阅读：`docs/superpowers/postmortems/2026-05-28-urlhack-64bit-underline.md`（写在错误假设上的第一版 postmortem），`docs/superpowers/runbooks/wine-debug-kitty.md`（调试环境搭建）

---

## 1. 症状回顾

两个表面不同、实际同源的现象：

### 1.1 URL underline bleed

用户报：

> "64位运行的时候，我在一个正常的窗口里，只要输入 `http://w` 满屏幕就全是下划线了，但是32位就不会。和屏幕宽度没有关系，设置多宽都一样，也和折行没有关系。"

KiTTY 内部的"hyperlink 自动识别"模块用 regex 扫描屏幕文字找 URL，识别到后通过 `terminal.c` 里的 toggle 状态机把 URL cell 标上 `ATTR_UNDER`。bleed 表现为：URL 之后所有屏幕格子（含空白）一直被打下划线，直到该行被新输出覆盖。

### 1.2 SSH 登录 segfault

用户后续补报：

> "64位ssh登录的时候，直接Segmentation fault了。"

并补充：

> "手动输入密码，还是用ssh key都是一样的。并不是所有远程机器都这样，我试了三台ubuntu都会crash, 试了两台centos不会。但是32位的一直都不会crash。"

跟下划线 bug 同样的"64-bit only / Ubuntu 后端触发 / CentOS 后端不触发"模式。

## 2. 错误假设的弯路（先记下来，避免重复）

### 2.1 第一版假设：off-by-one in toggle 状态机

`terminal.c:6905` 有一处可疑代码：

```c
if (urlhack_toggle_x == term->cols - 1) {
    // Handle special case where link ends at the last char of the row
    urlhack_toggle_y++;
    urlhack_toggle_x = 0;
}
```

`region.x1` 是半开区间，最右一格时 `x1 == cols`，不是 `cols - 1`。这是一个真实的 off-by-one。改成 `>= cols` 提交（`b0b31a0`）。

**结果**：客观正确，但**没修好 bug**。用户继续报 `http://w` 触发。

### 2.2 第二版假设：GNU regex 越过 nmatch 写

`urlhack.c` 调 `regexec(..., nmatch=1, &groupArray, ...)`，其中 `groupArray` 是单个 `regmatch_t`。看到 GNU regex 在某些 path 里会按 `re_nsub+1` 写完整 register 数组，怀疑越过 `nmatch` 踩了栈。改成 `regmatch_t groupArray[32]; ... nmatch=32`（`c1ff448`），又改成 `regmatch_t *groupArray = snewn(re_nsub + 1, regmatch_t)` 在堆上分配（`ca09834`）。

**结果**：在我环境下 `ca09834` build 跑起来 SSH 没崩，所以**以为修好了**。但其实 `urlhack_rx.re_nsub` 在 64-bit 下读到的是 0（见 §3.2 详述），所以 `re_nsub + 1 = 1` —— **回退到了 bug 前的 `nmatch=1`**。我没意识到。然后写了第一版 postmortem `docs/superpowers/postmortems/2026-05-28-urlhack-64bit-underline.md`，里面的 root cause 分析也是**错的**。

### 2.3 教训

修了一处看起来像 bug 的代码就声明"修好"，没有真正复现 + 验证。用户两次反复才把我逼到必须搭真实调试环境。

## 3. 真正的排查（Wine + Xvfb + gdb）

### 3.1 搭调试环境

容器里 wine 9.0 + Xvfb + xdotool + gdb + putty-tools 全装上（详见 `docs/superpowers/runbooks/wine-debug-kitty.md`）。改 Makefile `CFLAGS` 加 `-g`、`LDFLAGS` 去掉 `-s`，build 一个 `kitty64_nocompress.exe`（8.7 MB，含完整 DWARF debug info）。

发现的环境惊喜：

- 这台 Linux 本身就是 Ubuntu 24.04
- 自己跑了 sshd
- 所以 KiTTY 可以 `ssh admin@localhost` 直接复现 Ubuntu 后端场景

### 3.2 复现 + attach gdb

启动 KiTTY 在 Xvfb :99 上、SSH 上 localhost，看到 Ubuntu 的 Welcome MOTD 渲染出来 —— **complete underline bleed 重现**。注意是用最新的 `ca09834` build（自以为已修），所以我之前的 nmatch 修复**没有真的工作**。

attach gdb 到 wine subprocess（PE 二进制 mmap 在 `0x140000000` 开始），`file /tmp/kdb/kitty.exe` 让 gdb 读 DWARF，所有 KiTTY 源码符号立即可用。

查 `link_regions_current_pos`：

```
$1 = 580
```

580 个 link regions！MOTD 才 3 个 URL。

查头几个 region：

```
[0] = {x0=19, y0=5, x1=0, y1=0}
[1] = {x0=19, y0=5, x1=1, y1=0}
[2] = {x0=19, y0=5, x1=2, y1=0}
[3] = {x0=19, y0=5, x1=3, y1=0}
...
[100] = {x0=19, y0=5, x1=20, y1=1}
[200] = {x0=19, y0=5, x1=40, y1=2}
[300] = {x0=19, y0=5, x1=60, y1=3}
```

`x0=19, y0=5` 一直不变（= 第一个 URL `https://help.ubuntu.com` 的位置），`x1` 不停递增。**regex loop 反复匹配同一个 URL，但每次 `rm_eo` 错了**。

查 `urlhack_rx.re_nsub`：

```
$2 = 0
```

**`re_nsub = 0`** ——但 URL regex 明明有 13 个 capture group！

### 3.3 找到 root cause

gdb 直接读 struct 字节：

```
0x1401b1b40 <urlhack_rx+0>:	0x20 0x2f 0x5b 0xff 0xff 0x7f 0x00 0x00   <- buffer ptr
0x1401b1b48 <urlhack_rx+8>:	0xe0 0x00 0x00 0x00 0x00 0x00 0x00 0x00
0x1401b1b50 <urlhack_rx+16>:	0xe0 0x00 0x00 0x00 0x00 0x00 0x00 0x00
0x1401b1b58 <urlhack_rx+24>:	0xfc 0xb2 0x03 0x00 0x00 0x00 0x00 0x00
0x1401b1b60 <urlhack_rx+32>:	0x10 0x2e 0x5b 0xff 0xff 0x7f 0x00 0x00
0x1401b1b68 <urlhack_rx+40>:	0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00   <- KiTTY: re_nsub
0x1401b1b70 <urlhack_rx+48>:	0x0d 0x00 0x00 0x00 0x00 0x00 0x00 0x00   <- 这里有 13!
```

KiTTY 视角 `re_nsub` 在 offset 40 (全 0)。**offset 48 处有 `0x0d = 13`**，正好是 URL regex 的 capture group 数。

去看 `regex/libregex_64.a` 内的 DWARF（`x86_64-w64-mingw32-objdump --dwarf=info regex.o`）：

```
re_pattern_buffer:
  byte_size = 64       <-- 库视角
  buffer    @ 0
  allocated @ 8        size 8
  used      @ 16       size 8
  syntax    @ 24
  fastmap   @ 32
  translate @ 40
  re_nsub   @ 48       <-- 库视角

regoff_t = ssize_t     <-- 库视角，8 bytes on 64-bit
regmatch_t:
  rm_so @ 0
  rm_eo @ 8            <-- 库视角
  total 16 bytes
```

KiTTY 头文件 (`regex/regex.h`) 期望：

```
re_pattern_buffer:
  byte_size = 56       <-- KiTTY 视角 (MinGW LLP64: unsigned long = 4 bytes)
  buffer    @ 0
  allocated @ 8        size 4
  used      @ 12       size 4
  syntax    @ 16
  fastmap   @ 24
  translate @ 32
  re_nsub   @ 40
  
regoff_t = int         <-- KiTTY 视角，4 bytes
regmatch_t:
  rm_so @ 0
  rm_eo @ 4            <-- KiTTY 视角
  total 8 bytes
```

**库和头文件 ABI 完全不匹配**！

### 3.4 为什么 32-bit 不出问题

| 类型 | 32-bit MinGW | 64-bit MinGW (LLP64) | 64-bit Linux/glibc (LP64) |
|---|---|---|---|
| `unsigned long` | 4 bytes | **4 bytes** | **8 bytes** |
| `int` | 4 bytes | 4 bytes | 4 bytes |
| `ssize_t` | 4 bytes | 8 bytes | 8 bytes |
| pointer | 4 bytes | 8 bytes | 8 bytes |

`regex/libregex_64.a` 是 GNULIB regex 编译，用了 glibc 风格 `unsigned long = 8 bytes` + `regoff_t = ssize_t`。这俩值在 32-bit 下都是 4 bytes —— **碰巧**和 KiTTY 头文件视角一致，所以 32-bit 不出问题。

64-bit 下两边对同一字节有不同解读，灾难发生。

### 3.5 完整级联

库每次调 `regexec` 会：
1. **写**到 `regmatch_t` 的 offset 0 (`rm_so`) 和 offset 8 (`rm_eo`)
2. KiTTY 从 `regmatch_t` 读 offset 0 (`rm_so`) 和 **offset 4** (`rm_eo` —— 实际是库的 `rm_so` 高 4 字节，永远是 0)

所以 KiTTY 永远看到 `rm_eo == 0`。循环里：

```c
text_pos = text_pos + groupArray[0].rm_eo + 1;   // 永远只前进 1 字节
error = regexec(&urlhack_rx, text_pos, nmatch, groupArray, REG_NOTBOL);
```

regex 反复在 `text_pos += 1` 的字符串上匹配，每次都从 `https://...` 起点开始匹配（regex 库内部正确推进了，但 KiTTY 没拿到正确的 end offset）。每次循环 push 一个新 link region 到 `link_regions[]`。直到 regex 内部 `text_pos` 超过 `window_text` 末尾才停。

580 个 link regions 灌进 toggle 状态机后，`terminal.c:6896` 的 `j == urlhack_toggle_x && i == urlhack_toggle_y` 反复触发 toggle，`urlhack_is_link` 最终卡在 `1` —— 后续屏幕格子全部 `tattr |= ATTR_UNDER`。这就是 underline bleed。

SSH segfault 的可能机制：
- `urlhack_add_link_region()` 每次都 `snew(text_region)` 在堆上分配 16 字节
- 一次 MOTD 渲染产生 ~580 次分配，长时间会话产生数万次
- 某些 Ubuntu MOTD 内容 + 屏幕宽度组合可能让 link_regions 数组扩展到很大（`sresize`、`memmove`），触发堆破坏或 OOM 路径
- 加上库写 `re_pattern_buffer` 比 KiTTY 期望的多 8 字节，会踩 `is_regexp_compiled`（4 byte int）之后 4 字节 padding 区，某些环境下可能踩到下一个全局变量
- 任一路径都可能在不同时机触发 segfault，所以"CentOS 不崩 Ubuntu 崩"取决于 MOTD 内容 + KiTTY 内部内存布局

## 4. 修复

`regex/regex.h` 加一段条件类型定义：

```c
#if defined(_WIN64) || defined(__MINGW64__) || defined(__x86_64__)
#  include <stdint.h>
#  include <sys/types.h>   /* ssize_t */
#  define KITTY_REGEX_ULONG  uint64_t
#  define KITTY_REGEX_OFFT   ssize_t
#else
#  define KITTY_REGEX_ULONG  unsigned long
#  define KITTY_REGEX_OFFT   int
#endif
```

然后把 `re_pattern_buffer` 里的 `unsigned long allocated/used` 改成 `KITTY_REGEX_ULONG`，把 `regoff_t` 的 typedef 从 `int` 改成 `KITTY_REGEX_OFFT`。

效果：
- 64-bit 下 KiTTY 视角的 struct layout 与库一致（`re_pattern_buffer = 64 字节`，`regmatch_t = 16 字节`，`re_nsub` 在 offset 48，`rm_eo` 在 offset 8）
- 32-bit 下完全不变（保留原历史行为，正巧与库一致）

附带优化：`url/urlhack.c` 加一个 `if (nmatch < 4) nmatch = 4;` 防御未编译 regex 时 `re_nsub = 0` 仍能拿到可用 buffer。

## 5. 验证

修复后再 attach gdb：

```
$1 = {buffer = 0x..., allocated = 224, used = 224, syntax = 242428,
      fastmap = 0x... "", translate = 0x0, re_nsub = 13,
      can_be_null = 0, regs_allocated = 0, fastmap_accurate = 1, ...}
$2 = (link_regions_current_pos) 3
```

| 状态 | 修复前 | 修复后 |
|---|---|---|
| `re_nsub` | 0 | **13** ✓ |
| `used` (library 写入) | 0（KiTTY 读错位） | **224** ✓ |
| `link_regions_current_pos` (Ubuntu MOTD 后) | **580** | **3** ✓ |
| underline 渲染 | 整片屏幕 bleed | 仅 URL 上 ✓ |

截图验证：URL 自身正确下划线，"This system has been minimized..."、"Last login..."、`$` prompt 全部无下划线。

## 6. 教训

1. **跨架构 bug 的嫌疑名单**比上一版 postmortem 列的还要长一项：**第三方 prebuilt 库的 ABI**。规模较小的项目 vendor 一份 `.a` 配一份 `.h`，但 `.h` 是从上游 GNU 1993 版改来的，`.a` 是后来用 GNULIB 重编的，layout 早已不一样。**只要库和头不是同一次编译产物，就要怀疑 ABI**。

2. **写 postmortem 也要先 verify**。我写完上一版 postmortem（`2026-05-28-urlhack-64bit-underline.md`）后没回头去看实际行为，里面的整段 "GNU regex 越过 nmatch 写栈" 分析在新数据面前完全垮掉。**verification-before-completion 也适用于复盘文档**。

3. **"看起来像 bug 的代码"和"当前 bug 的成因"是两件事**。`terminal.c:6905` 的 off-by-one 是真 bug、修了也对，但和用户报的 bleed 无关。先复现再下手。

4. **DWARF 调试信息能在 PE 二进制里跨平台用**。MinGW 编出来的 `.exe` 跑在 wine 里，是真的 Linux 进程，Linux 上的 gdb 可以直接 attach 并读 DWARF。这套调试链很多人不知道存在，但是金子。

5. **拿到 `gdb -p PID`，第一步是 `x/64bx struct_addr` 看真实字节**，不要立刻信任 `print struct` —— 你的 struct 定义可能和库的不一致，print 会按 KiTTY 视角解析、骗你。

## 7. 后续改进建议

- **替换 `regex/libregex_64.a`**：用 MinGW-w64 工具链重编一份 GNULIB regex，得到一份真正匹配 KiTTY 头文件（或干脆生成新头）的 .a。现在的 fix 是把 KiTTY 头文件适配到库，等价于"接受库的 ABI"；如果有上游变更，仍然脆弱。
- **加 build-time 断言**：在某个 .c 文件里写 `_Static_assert(sizeof(regex_t) == 64, "regex ABI mismatch");`（64-bit），如果未来库再换、size 又变，编译期立刻报错。
- **用 system regex**：如果允许 Win10+ 的 ucrt，POSIX regex 可能可以直接调用，省掉这份历史包袱。
- **追加 `valgrind` 在 wine 下的内存追踪流程到 runbook**：能直接定位 free/realloc 边界附近的越界。

## 8. 相关 commit / 文件清单

| Commit | 内容 |
|---|---|
| `b0b31a0` | off-by-one 修复（客观正确但不是当前 bug 的因） |
| `c1ff448` | 把 regmatch_t 改成 32-slot 数组（基于错误假设的修复） |
| `a6c8cd8` | 第一版 postmortem（基于错误假设，结论错了） |
| `ca09834` | 改成动态 `re_nsub + 1`（在 ABI bug 下退化成 nmatch=1，等于没修） |
| `8fd81b7` | Wine + Xvfb + gdb 调试 runbook |
| `5c0a369` | **真正的 fix：ABI 修复**（本文档对应的根因修复） |
| 本文档 | 真正的 root cause postmortem |
