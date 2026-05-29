# 在 Linux 上用 Wine + Xvfb 调试 KiTTY 64-bit 二进制

- 适用场景：64-bit-only bug（在 Windows 上崩，但 32-bit 不崩；或 64-bit 行为异常）
- 关键能力：在 Linux 上直接运行 `kitty64.exe`、用 Xvfb 提供虚拟显示、用 gdb 或 winedbg 抓栈帧
- 验证过的环境：Ubuntu 24.04（noble）+ Wine 9.0 + Xvfb 21.1
- 验证过的复现：URL underline bleed（输入 `http://...` 后整片屏幕带下划线）

---

## 1. 一次性环境准备

### 1.1 安装包

```bash
sudo apt-get update
sudo apt-get install -y \
    wine wine64 \
    xvfb xdotool \
    gdb \
    putty-tools \
    imagemagick \
    openssh-server
```

| 包 | 作用 |
|---|---|
| `wine` / `wine64` | 跑 Windows PE 二进制 |
| `xvfb` | 无头虚拟 X server（不需要图形终端） |
| `xdotool` | 在虚拟 X 里模拟鼠标键盘 |
| `gdb` | 调试 wine 进程（MinGW 编译的 DWARF 调试信息 GDB 能读） |
| `putty-tools` | 提供 `puttygen`，把 OpenSSH key 转 PuTTY `.ppk` |
| `imagemagick` | `import` 命令，从 Xvfb 截屏 |
| `openssh-server` | 本机自跑 sshd，KiTTY 可 SSH 连 `localhost`（真 Ubuntu prompt，与远端复现等价） |

### 1.2 启动本机 sshd

容器/无 systemd 环境：

```bash
sudo /usr/sbin/sshd -D -p 22 &           # 或 2222 端口（避免与 host 冲突）
ss -tnlp | grep :22                       # 验证监听
```

带 systemd 的发行版：

```bash
sudo systemctl start ssh
```

### 1.3 生成 SSH key 给 KiTTY 用

KiTTY/PuTTY 不认 OpenSSH 格式的私钥，需要 `.ppk`。

```bash
# 1. 生成 OpenSSH key（如已有可跳过）
ssh-keygen -t ed25519 -f ~/.ssh/id_ed25519_test -N ""

# 2. 把 public key 写到本机 authorized_keys
cat ~/.ssh/id_ed25519_test.pub >> ~/.ssh/authorized_keys
chmod 600 ~/.ssh/authorized_keys

# 3. 验证 OpenSSH 这条路通
ssh -o StrictHostKeyChecking=no -i ~/.ssh/id_ed25519_test -p 22 \
    "$(whoami)@localhost" "echo OK; uname -a"

# 4. 转 .ppk 给 KiTTY 用
puttygen ~/.ssh/id_ed25519_test -o /tmp/id_ed25519_test.ppk -O private
ls -la /tmp/id_ed25519_test.ppk
```

### 1.4 初始化 wine prefix

```bash
export WINEPREFIX=/tmp/wp64 WINEARCH=win64
mkdir -p "$WINEPREFIX"
WINEDEBUG=-all wineboot -i
```

第一次会有 `wine32 missing` 警告——**可以忽略**，64-bit 二进制不需要 wine32。

---

## 2. 编译带调试符号的 kitty64.exe

默认 Makefile 用 `-O2` 编译、`-s` 链接（strip）。要调试需要：
- **保留 DWARF**：加 `-g` 到 CFLAGS
- **不要 strip**：把 LDFLAGS 的 `-s` 去掉

### 2.1 改 `0.76b_My_PuTTY/windows/MAKEFILE.MINGW`

```diff
-CFLAGS = -Wall -O2 -std=gnu99 -Wvla -D_WINDOWS -DWIN32S_COMPAT \
+CFLAGS = -Wall -O2 -g -std=gnu99 -Wvla -D_WINDOWS -DWIN32S_COMPAT \
         -D_NO_OLDNAMES -D__USE_MINGW_ANSI_STDIO=1 -I.././ \
         -I../charset/ -I../windows/ -I../unix/ -I../crypto/ -I../proxy/ -I../ssh/ -I../terminal/ -I../utils/
-LDFLAGS = -s
+LDFLAGS =
```

**注意**：不要用 `-O0`。`-O0` 在当前代码库里会暴露 `wintw_clip_write` 等 `static` 函数从其它 TU 引用的 layering bug，链接失败。`-O2 -g` 是可工作的最小调试 build。

### 2.2 build

```bash
cd /home/admin/git/KiTTY.new
./zzy.sh cross64
```

构建末尾会 upx 压缩 `kitty64.exe`（破坏调试符号），**但** Makefile 在压缩之前会先 `cp kitty64.exe /builds/kitty64_nocompress.exe`——所以 **用 `kitty64_nocompress.exe` 调试**，不要用 `kitty64.exe`。

验证调试信息在二进制里：

```bash
x86_64-w64-mingw32-objdump -h /builds/kitty64_nocompress.exe | grep debug
# 应看到 .debug_info / .debug_line / .debug_abbrev / .debug_aranges 等若干 section
```

### 2.3 复原 32-bit 兼容性（cross64 副作用）

`./zzy.sh cross64` 会把 `mini_64.a` 覆盖到 `mini.a`（其它几个同理），让后续 `./zzy.sh cross` 链接 64-bit 库失败。要交替构建，**每次 cross64 后**：

```bash
# 临时（用 git index 里的版本）— 但仓库里 mini.a 现在也是 64-bit
git checkout base64/base64.a bcrypt/bcrypt.a mini/mini.a

# 或：从 035070c（确认是 32-bit 内容的旧 commit）取回
git show 035070c:mini/mini.a    > mini/mini.a
git show 035070c:bcrypt/bcrypt.a > bcrypt/bcrypt.a
git show 035070c:base64/base64.a > base64/base64.a
```

---

## 3. 启动 Xvfb + KiTTY

### 3.1 启动 Xvfb（无头虚拟显示）

```bash
Xvfb :99 -screen 0 1280x800x24 >/tmp/xvfb.log 2>&1 &
sleep 2
ls /tmp/.X11-unix/             # 应看到 X99
```

让所有后续 wine / xdotool 命令使用 `DISPLAY=:99`。

### 3.2 准备 KiTTY 工作目录

```bash
mkdir -p /tmp/kdb
cp /builds/kitty64_nocompress.exe /tmp/kdb/kitty.exe

# 最小化 kitty.ini（portable 模式 + fontfallback trace 日志）
cat > /tmp/kdb/kitty.ini << 'EOF'
[KiTTY]
savemode=dir
configdir=/tmp/kdb

[FontFallback]
Log=trace
EOF

mkdir -p "/tmp/kdb/Sessions/Default Settings"
```

### 3.3 启动 KiTTY 直接 SSH 到 localhost

```bash
cd /tmp/kdb
WINEPREFIX=/tmp/wp64 DISPLAY=:99 WINEDEBUG=-all \
    wine ./kitty.exe "$(whoami)@localhost" -P 22 -i /tmp/kdb/id_ed25519_test.ppk \
    > /tmp/wine_out.log 2>&1 &
echo "wine launcher PID: $!"
sleep 5
ps -ef | grep -E "kitty\.exe|wineserver64" | grep -v grep
```

第一次连接会弹"host key not cached"对话框，需要点 Accept。

### 3.4 截屏看现状

```bash
DISPLAY=:99 import -window root /tmp/k.png
ls -la /tmp/k.png
```

用 image viewer / scp 把 `/tmp/k.png` 看下。在带 Read tool 的环境直接 read 即可。

### 3.5 模拟鼠标点击（关掉对话框、点按钮）

```bash
# 比如 Accept 按钮在 (360, 330) 附近
DISPLAY=:99 xdotool mousemove 360 330 click 1

# 给 KiTTY 窗口发键盘事件（先聚焦到它的窗口）
DISPLAY=:99 xdotool search --name "KiTTY" windowactivate type "ls -la
"
```

注意 xdotool 的 `type` 不带尾随换行，要换行用 `key Return` 或在字符串里写真的 `\n`。

### 3.6 关掉/清理

```bash
pkill -f "kitty.exe"
pkill -f wineserver
pkill Xvfb
```

---

## 4. 用 gdb attach 到 wine 里的 KiTTY 进程

wine 在 Linux 下是真正的 Linux 进程；MinGW 的 DWARF 调试符号 GDB 能直接读。

### 4.1 找到 KiTTY 进程

```bash
ps -ef | grep "kitty.exe" | grep -v grep
# 找到 wine 启动的那个 PID（不是 wineserver64）
PID=<the PID>
```

### 4.2 attach

```bash
sudo -n gdb -p $PID
```

注意：attach 需要 `ptrace` 权限。容器/受限环境可能要 root 或 `setcap cap_sys_ptrace+ep gdb`。

### 4.3 GDB 内常用命令

```
# 列出 KiTTY 的 C 函数
(gdb) info functions urlhack
(gdb) info functions winfb_
(gdb) info functions do_text_internal

# 在函数上打断点
(gdb) b urlhack_go_find_me_some_hyperlinks
(gdb) b urlhack_add_link_region
(gdb) b winfb_split

# 查看变量、调栈
(gdb) c              # continue
(gdb) bt             # 当前调栈
(gdb) frame 3        # 切到第 3 帧
(gdb) info args
(gdb) info locals
(gdb) print groupArray[0]
(gdb) print urlhack_rx.re_nsub
(gdb) print link_regions_current_pos
(gdb) print link_regions[0]->x0
(gdb) x/16wx text_pos
```

注意：**KiTTY 的栈帧大多是 windows GUI 回调**（`WindowProc` → ...），所以 `bt` 会看到许多 wine 的 Windows API frame；focus 你的源代码 frame。

### 4.4 GDB 用 source-mapped breakpoint

```
(gdb) b terminal.c:6896
(gdb) b winfont_fallback.c:382
(gdb) b kitty.c:1748
```

### 4.5 监视 stack-smashing 的小工具：watch points

```
(gdb) watch groupArray[1].rm_so     # 当某个不该被写的 slot 被改时就停
```

---

## 5. 用 winedbg 替代 gdb

如果不需要源码级调试、只想看到崩溃栈：

```bash
WINEPREFIX=/tmp/wp64 DISPLAY=:99 winedbg /tmp/kdb/kitty.exe \
    admin@localhost -P 22 -i /tmp/kdb/id_ed25519_test.ppk
```

winedbg 是 wine 自带的调试器，崩溃时自动停在出错指令上，可以看 backtrace / registers，但不识别 DWARF —— 只能看到 Windows-style 函数名（如果有 PE symbol table）。对于本仓库的 MinGW 二进制，**强烈推荐用 gdb 而不是 winedbg**。

---

## 6. 用 xpra 跑（如果你想从远程客户端看 GUI）

xpra 比 Xvfb 多个能力：可以从其它机器 attach 一个真窗口看 KiTTY。但**调试场景下不必要**——Xvfb + `import` 截屏已经够用。

如果你确实想用 xpra：

```bash
# 启动 xpra session
xpra start :100 --no-mdns --no-pulseaudio --no-notifications

# 在 :100 里启动 KiTTY
WINEPREFIX=/tmp/wp64 DISPLAY=:100 wine /tmp/kdb/kitty.exe

# 从另一台机器 attach（带 GUI）
xpra attach ssh://user@host/100
```

xpra 启动慢、依赖 X 转发链路；除非你要远程肉眼看实时画面，建议忽略，用 Xvfb + 截屏更轻。

---

## 7. 一键复现 "URL underline bleed"

把下面这段当成 runbook：

```bash
# 准备
sudo /usr/sbin/sshd -D -p 22 &
Xvfb :99 -screen 0 1280x800x24 &
sleep 2
mkdir -p /tmp/kdb && cp /builds/kitty64_nocompress.exe /tmp/kdb/kitty.exe
puttygen ~/.ssh/id_ed25519_test -o /tmp/kdb/id.ppk -O private 2>/dev/null
cat > /tmp/kdb/kitty.ini << 'EOF'
[KiTTY]
savemode=dir
configdir=/tmp/kdb
[FontFallback]
Log=trace
EOF
mkdir -p "/tmp/kdb/Sessions/Default Settings"

# 启动 KiTTY
cd /tmp/kdb
WINEPREFIX=/tmp/wp64 DISPLAY=:99 WINEDEBUG=-all \
    wine ./kitty.exe "$(whoami)@localhost" -P 22 -i /tmp/kdb/id.ppk \
    >/tmp/wine_out.log 2>&1 &

# 等连接成功，点 Accept
sleep 5
DISPLAY=:99 xdotool mousemove 360 330 click 1
sleep 3

# 截屏看 Ubuntu MOTD 是否触发 underline bleed
DISPLAY=:99 import -window root /tmp/k.png
# 或在 KiTTY 里手动输 "http://w" 触发短链接
DISPLAY=:99 xdotool search --name "KiTTY" windowactivate
DISPLAY=:99 xdotool type "echo http://w
"
sleep 1
DISPLAY=:99 import -window root /tmp/k_url.png

# 检查
ls -la /tmp/k.png /tmp/k_url.png
```

---

## 8. 故障排查清单

| 现象 | 原因 / 处理 |
|---|---|
| `it looks like wine32 is missing` 警告 | 跑 64-bit 二进制时无害，忽略 |
| `winediag:ntlm_check_version ntlm_auth was not found` | KiTTY 不用 NTLM，无害 |
| `wineserver64` 已存在、新 KiTTY 启动报错 | `pkill wineserver`，再启 |
| Xvfb `directory /tmp/.X11-unix will not be created` | 警告，不致命；如果连不上 X 可以 `sudo mkdir -p /tmp/.X11-unix && sudo chmod 1777 /tmp/.X11-unix` |
| `import -window root` 截到全黑 | KiTTY 已退出，看 `ps` 是否还在跑；或者刚启动还没画完，sleep 多一点 |
| 改了源码、build 完了但跑的是旧二进制 | `pkill -f kitty.exe; pkill wineserver` 后重启，确保不是缓存进程 |
| gdb 报 `ptrace: Operation not permitted` | 容器需要 `--cap-add SYS_PTRACE` 或写 `/proc/sys/kernel/yama/ptrace_scope = 0` |
| 32-bit / 64-bit build 互相破坏 .a 文件 | 见 §2.3 |
| MinGW 编译 `-O0` 时 `undefined reference to wintw_clip_write` 等 | 用 `-O2 -g`，不要用 `-O0`（代码有 `static` vs `extern` 的 layering bug，被 `-O2` 内联绕过） |

---

## 9. 后续可优化

- 把 build 流程拆出一个 `make debug64` target，自动添加 `-g`、跳过 `upx`
- 用 `record-full` + `reverse-step` 做"时间旅行"调试（gdb-15 支持）
- 用 `valgrind` 跑 wine 进程检查内存错误：`WINEDEBUG=-all valgrind --tool=memcheck --suppressions=wine.supp wine ./kitty.exe ...`（重，会慢 50×，但是定位 stack-overrun 的金标准）
- 把 `kitty.ini` 的 `[FontFallback] Log=trace` 改为 `info` 减少日志量；在 `/tmp/kdb/fontfallback.log` 里看实时 trace

---

## 10. 当前已知的复现

在本环境用最新 `ca09834` build：

- **可复现**：URL underline bleed（Ubuntu MOTD 后整片屏幕带下划线）
- **未复现**：SSH 登录 segfault（用户在 Windows 上能复现，wine 下没复现）

后者可能是 wine 屏蔽了某个 Windows-specific 路径，或某个具体 escape sequence 触发，或 wine 的 GDI 行为更宽容。要复现可能需要：
- 真 Windows 机器 + WinDbg
- 或装 wine-staging（实验性 patch 更接近 Windows 行为）
- 或用 `valgrind` 在 wine 下跑，让微小的内存错误立即被检测
