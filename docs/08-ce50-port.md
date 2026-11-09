# Windows CE 5.0 移植：对照 docs/07 清单逐项落实

2026-11-08。用户在新 VM 里装好了 Windows CE 5.0（PB 5.0，2014 年 6 月的最后一
批更新），决定把 ClassPad 移植目标从 CE 6.0 R3 改成 CE 5.0（docs/07 原先讨论
的是 4.2，结论同样适用：5.0 是 4.2 的直接后继，内核模型相同）。本文按
docs/07 的编号逐条记录 CE 5.0 的核对结果和做法。对应的 BSP 在
`bsp/classpad50/`，翻译器的改动在 `dbt/rt/nkbin.c`。

核对依据：`/Volumes/WINCE500` 里的 `PRIVATE\WINCEOS\COREOS\NK`（内核源码）、
`PLATFORM\ASPEN`（SH7750 样例 BSP）、`PLATFORM\COMMON\SRC\INC`、
`PUBLIC\COMMON\OAK\LIB\SH4\retail\nkmain.lib`（用 COFF 符号表扫出内核对 OAL
的全部外部引用）。

## 一、内核源码

| docs/07 | CE 5.0 的情况 | 做法 |
|---|---|---|
| 1 WaitOneMore proxy 泄漏 | **不存在**：`schedule.c` 的 `exitfunc` 对中断事件走 `((LPEVENT)pProx->pObject)->pProxList = 0` | 不需要补丁；驱动仍不对中断事件做超时等待 |
| 2 loader 拒绝 SH3 PE | 同样存在：`loader.c` ~2537 的 `#ifdef SH3 / MIPS / ARMV4I` 链没有 SH4 分支 | `bsp/kernel/ce50-loader-sh3-images.patch`；改完要 `build -c` 重编 nkmain.lib 并 Sysgen（nk.lib 由 Sysgen 生成） |
| 3 内核直接访问 P4 寄存器 | 同样（`shexcept.src`、`mdsh3.c`、`vmshx.c`），但代码在 nk.exe 里（kernel + OAL 一个模块） | 翻译器：模块表里没有 kernel.dll 时取 nk.exe 的代码节作为设备感知范围（`dbt/rt/nkbin.c`，加载时自动判断，CE 6.0 镜像不受影响）。OAL 在范围内，因此 OAL 对 0xFFxxxxxx 的访问改用不交换的 `P4_*` 宏（`cache.c` 的 MMUCR.TI）；0xA4xxxxxx 的外设仍用预交换的 `MMIO_*` |
| 4 bank 1 常驻状态 | 相同：r4_bank1 = pCurThread，r7_bank1 = 0xFF000000（`shexcept.src` 258/286/318/666） | 翻译器不变 |
| 5 OEMInit 在 BL=1、bank 1 | 相同：`KernelStart` 置 `PR_B1_BK`，从 P2 别名调 `SH3Init`，其中调 `OEMInitDebugSerial`、签名串、`OEMInit` | 调试输出仍走 RAM 环形缓冲区 |
| 6 API 陷阱地址 | **相同**：`ksshx.h` `FIRST_METHOD = 0xFFFFFE01`，`SYSCALL_RETURN = FIRST_METHOD-2` | 翻译器不变 |
| 7 UTLB 回填捷径（shell-performance 分支） | 绑定 CE6 的 `TLBMissHandler` 字节 | 该分支未合并；合并时对 CE 5.0 镜像要关掉或重做 |
| 页大小 | `mem_shx.h`：SH 一律 4096 | pe4k 重对齐照旧 |

## 二、OAL（`bsp/classpad50/SRC/OAL`）

CE 5.0 的 OAL 契约（nkmain.lib 未定义符号里属于 OAL 的）：`OEMNMI`（汇编符
号，无下划线）、`InitClock`、`OEMCacheRangeFlush`、`OEMGetExtensionDRAM`、
`OEMGetRealTime/SetRealTime/SetAlarmTime`、`OEMIdle`、`OEMInit`、
`OEMInitDebugSerial`、`OEMInterruptEnable/Disable/Done`、`OEMIoControl`、
`OEMPowerOff`、`OEMReadDebugByte`、`OEMWriteDebugByte/String`、
`SC_GetTickCount`、`SH4CacheLines`。可写的内核公共变量：`OEMRomChain`、
`OEMExtraCCR`、`pOEMUpdateRescheduleTime`、`pQueryPerformanceCounter/
Frequency`、`curridlelow/high`、`idleconv`、`dwNKDrWatsonSize`。

- 没有 OEMGLOBAL：`init.c` 直接定义 `OEMInit`，不再有 `OEMInitGlobals`。
- CE 5.0 没有 SH4 的 PLATFORM\COMMON 库（`SRC\dirs` 写着 `DIRS_SH=SHx`，目录
  却不存在）。`oalcommon.c` 自带 SYSINTR 映射、`OEMIoControl` 分发表、通用
  IOCTL（设备信息/ID/UUID/处理器信息/缓存信息/注册表初始化）、日志、唤醒源、
  `g_oalTimer`/QPC/`SC_GetTickCount`。头文件仍用 PQOAL 的。
- 定时器：`InitClock()` 由 OEMInit 和内核（`CallOEMPowerOff` 之后）调用；
  `OEMIdle(DWORD)`；可变节拍靠 `pOEMUpdateRescheduleTime`。
- 中断：`HookInterrupt` + `IntrPrio[]` 与 6.0 相同；`ExceptionTable` 有
  16 + 496 项，KEYSC 的向量 95 放得下；`dwSHxEventCodeLength` 默认 12 位。
- 缓存：CE 5.0 的 `CACHE_ENABLE = OCE|ICE|CB = 0x105`，SH-4A 位定义一致；
  `OEMExtraCCR` 留 0。`SH4CacheLines = 1024`（SH7305 OC 32 KiB / 32 字节）。
- EXT 区：`OEMRomChain = &g_extRom`，`KernelFindMemory` 把 NK 区接在后面。
- `kern.exe`（`SRC\OAL\KERN`）：`nk.lib + oal.lib + loadauth.lib + fulllibc`
  加 KITL 桩，与 ASPEN 的 `KERNEL\BUILDEXE\KERN` 同构。

## 三、镜像布局（`FILES\config.bib`）

- 两个 XIP 区照旧（NK 在 eActivity 区 0x83860000，EXT 在空洞 0x80EE0000），但
  **CE 5.0 的 romimage 4.0 不会布局两个不相邻的区**：给它两条 MEMORY 行，它把
  NK 放到 EXT 的地址、EXT 紧接在 NK 之后（2026-11-09 makeimg.log：
  `RAM AutoSize: RAM Start=813c1000`，nk.exe 在 80ee0000，Xpander 在
  81443000 起），最后在收尾 NK.bin 时断言退出；`DLLADDR_AUTOSIZE` 只在
  `AUTOSIZE=ON` 下生效，而 `AUTOSIZE=ON` 又会把 RAM 挪到镜像后面。CE 6.0 的
  romimage 能正确处理同一份 config.bib。解决：**两次 romimage**——
  `PostFmergeBib.bat` 把 ce.bib 里区列为 EXT 的条目移到
  `ext.modules.bib`/`ext.files.bib`，makeimg 的 romimage 只建 NK；
  `PostRomImage.bat`（makeimg 的另一个钩子）用 `ext.bib.hdr` 加这两个文件拼出
  `ext.bib`，再跑一次 romimage 得到 EXT.bin。两次运行的 DLL 地址范围必须错开：
  NK 的 SH4 DLL 数据页从 0x02000000 向下、代码在 slot 1 从 0x04000000 向下；
  EXT 的 Pocket PC 模块没有 .rel 文件（"old fixup style"），整体落在数据范围里
  （25 个模块 5.1 MiB）。于是 NK 数据 01F00000..02000000、代码
  02200000..04000000；EXT 数据 01400000..01F00000、代码 02100000..02200000。
  按 ce.bib 统计 NK 约 4.9 MB、EXT 约 4.0 MB，两个区都放得下，合成一个区放不下。
- `FSRAMPERCENT=08080808`、`COMPRESSION=OFF`、`KERNELFIXUPS=ON`、
  `AUTOSIZE=OFF`、`ROMFLAGS=00` 照旧。`ROMFLAGS` bit 1 必须为 0：CE 5.0 的
  驱动和 GWES 要在内核模式跑，驱动的固定 P2 地址 MMIO 才能用。
- `PostFmergeBib.bat` 钩子在 CE 5.0 的 makeimg.exe 里也有，裁网络栈和
  .wav 的做法照旧。
- 构建选项：关 eboot/KITL/内核调试器，不开 ship build；`classpad.bat` 另设
  `IMGNOSHAREETH`、`KERNELNOSHAREETH`。

## 四、驱动

- GPE、Layout Manager、touch DDSI 三组头文件 5.0/6.0 只差版权行，源码不改。
- 库名与 6.0 相同：`gpe.lib`、`tchmdd.lib`（Sysgen 在 `cesysgen\oak\lib` 里由
  gpe_lib/tchmain 生成；`PUBLIC\COMMON\OAK\LIB` 只有这些输入库，第一次构建就
  栽在这里）、`layoutmanager.lib`、`inputlang_0409.lib`；`touch.def` 用
  `cesysgen\oak\inc` 里的。
- `platform.bib`：没有 `K` 标志，也不需要 `k.touch.dll`。
- `pm.h` 在 CE 5.0 的 `SDK\INC`（已在包含路径里）。

## 五、Pocket PC 程序、MFC、DocList、PegHelp、Plotter-Z

- aygshell 在 CE 5.0 里由 `PUBLIC\SHELLSDK` 提供（“AYGShell API Set”），
  mfcce300 要的序号 4/21/34/40/56/69/83/84 在 `aygshell.def` 里都有。
- SH3CRT.dll、DocList.dll、Plotter-Z 的 `sources` 只用 coredll/commctrl/
  commdlg，CE 5.0 都有；代码不改。rotatekey 用 LoadLibrary 取 SHFindMenuBar，
  不链接 aygshell.lib（CE 5.0 的 SH4 LIB 里没有它）。
- `_PROJECTROOT\cesysgen\sdk\lib` 路径在 CE 5.0 同样有效。
- PegHelp（`xpander-help-button-crash` 工作区，未提交）与 CE 版本无关，
  等它提交后复制到 `SRC\APPS\PEGHELP` 即可。
- SH3 二进制的 4 KiB 对齐问题在 CE 5.0 同样存在（页 4096），pe4k 照旧。

## 六、翻译器

- `dbt/rt/nkbin.c`：没有 kernel.dll 时用 nk.exe 的代码节作为设备感知范围，
  打印 `[nk] nk.exe (kernel + OAL, CE 5.0 style) ...`。宿主测试
  （`make test`）通过；`nkxip` guest 仍用 kernel.dll 的模块表，行为不变。
- 超级调用契约、SR.FD 模拟、bank 1 保存、合成异常都不变。
- 因为 OAL 也在设备感知范围内，OAL 代码的每条 16/32 位访存多 5 条指令；
  OAL 的热路径只有定时器 ISR 和中断胶水，可以接受。KDEV（shell-performance
  分支）要为 nk.exe 重做特征码后才能启用。

## 七、第一次启动（2026-11-09，casio-emu fx-cp400 配置）

VM 里 Sysgen + Build and Sysgen Current BSP + makeimg（两次 romimage）得到
NK.bin 4.87 MB、EXT.bin 4.03 MB。`make -C dbt NK_WINDOW=eact NKBIN=… EXTBIN=…
nkshot` 的时间线（墙钟，M5 Max）：

| 翻译器 | POSTINIT | 显示驱动起 | 桌面首帧 | Explorer 任务栏 |
|---|---|---|---|---|
| 合并 shell-performance 前（4 MiB 缓存、14400 块头，无二次机会/ASID 哈希/FAST_HELPERS） | — | < 15 min | 约 20–25 min（按帧文件时间推算） | — |
| 合并后（3.25 MiB、19200 块头、二次机会、ASID 哈希、FAST_HELPERS；KDEV/TLB_REFILL 对 nk.exe 自动关闭） | 3.4 s | 45 s | 111 s | 129 s |

截图 `notes/ce/desktop-cp400-ce50.png`（notes/ce 下的图片按 .gitignore 不入库，
与 CE 6.0 的截图一样只在本地）：Windows CE v5.00 Build 1400 的桌面，
My Device / Recycle Bin / WordPad / My Documents，任务栏时钟和 SIP 图标。
日志里内核签名、`CCR=0105`、EXT 区 31 模块 14 文件、三个驱动的启动行都在；
桌面空闲 5 分钟后 Power Manager 按 platform.reg 的超时挂起，翻译器进入
R-standby（`[dbt] R-standby`），属预期行为。CE 6.0 用合并前那代翻译器到桌面
约 5 分钟，所以 CE 5.0 不比 6.0 慢；合并后的翻译器对两者都是数量级的改善，
其中多少归功于 CE 5.0 更简单的地址空间模型，要用同一翻译器跑 CE 6.0 镜像才
能分开。

同一镜像的后续两轮（模拟触笔 `HLE_CB_TOUCHSEQ`）：

- tick 2.0M 双击 My Device（35,40）：触摸驱动报 `down raw (862,501)`，Explorer
  打开 My Device 窗口（`notes/ce/mydevice-cp400-ce50.png`）。
- 点图标条的 Main（114,552）：`CLASSPAD TOUCH: Main released` →
  `ROTATEKEY: Math Xpander started`（rotatekey 自己在 EXT 里）→ 36 s 后 Math
  Xpander 的工作簿出现（`notes/ce/xpander-cp400-ce50.png`：Preferences、
  Angular Units、Display Precision，底部 Pocket PC 命令栏和工具栏）。这一步
  同时证明：EXT 区的 XIP 模块能加载和运行；**SH3 的 XIP 模块不需要 loader
  补丁**（romimage 的 P 标志 + 内核对 ROM 模块不查 CPU 类型，VM 里还没打
  ce50-loader-sh3-images.patch），补丁只有把 SH3 程序当普通文件装进来时才
  需要；mfcce300 + DocList + SH3CRT 的导入链在 CE 5.0 下解析并运行。

还没验证：SH3CRT 的浮点结果（数学键盘点按）、开始菜单和 Plotter-Z、键盘、
真机。

## 八、KDEV 与 TLB_REFILL 的 CE 5.0 版本（2026-11-09）

`claude/shell-performance-issue-6298c3` 的两项优化绑定 CE 6.0 的内核二进制，
合并进来后对 nk.exe 自动关闭（docs/04）。为 CE 5.0 重做：

- **KDEV**：`tools/kernel_devfuncs.py` 现在在模块表里没有 kernel.dll 时取
  nk.exe，规则不变（0xFF 开头的字面量、banked 寄存器、`mov #-1` 移位、异常向量
  区），`--table kdev_funcs50 -o dbt/rt/kernel_dev50.h` 从 kern.exe 的
  NK.bin + kern.map 生成第二张表：25 个函数，5384 字节（nk.exe .text 共
  204 KB），其中 TLBMissHandler、APICall、InterruptExceptionHandler、
  SetCPUASID、LoadKPage、SH3Init、DoSetRAMMode、DumpFrame 是真正碰 P4 的内核
  函数，`_OEMCacheRangeFlush` 是 OAL 的 MMUCR.TI 写（`P4_*` 宏），其余几个
  （`__stod`、`___SH3Raise`、`_SC_CreateThread`、`_Rtlp*`）是规则的保守命中，
  多付几条指令无害。`rt/kdev.c` 两张表都编进去，`kdev_locate(..., is_nk)` 按
  `nkbin.c` 判出的内核模块选表；找不到或有歧义仍退回整模块设备感知。
- **TLB_REFILL**：`rt.c` 增加 `tlbr5_code`（CE 5.0 `TLBMissHandler` 从第一条
  指令到 `rte` 的 46 个字，无 fixup，只有一个 `mov.w` 字面量 BLOCK_MASK=0x1FF，
  一并核对）和 `tlb_refill5`：TEA ≥ 2 GiB 交回 guest；TTB → `SectionTable
  [TEA>>25]` → `SECTION[(TEA>>16)&0x1FF]` → MEMBLOCK（必须是内核指针，
  NULL_BLOCK/RESERVED_BLOCK 交回）→ `aPages[(TEA>>12)&15]`，bit 0（WT 位，
  内核用作 valid）为 1；访问锁 `MEMBLOCK.alk` 与当前线程的 `aky`
  （`pCurThread`=r4_bank1，`ctx->b1[4]`，+0x14）要有公共位，否则交回 guest
  走 tlb50 的访问违例；然后 PTEA=(entry>>29)|((entry>>9)&1)<<3、
  PTEL=entry&~1，`ldtlb`，出错的宿主指令重跑。CE 6.0 的路径不变，
  `tlbr_check` 先比对 6.0 的字节，不符再比对 5.0 的。

验证：`make KDEV=0 TLB_REFILL=0 EXTRA_CFLAGS="-DDBT_DEV_SITES
-DDBT_STATS_EVERY=20000"` 跑一次启动加 My Device 双击，收集到 14 个
`[devsite]`（TLBMissHandler+0x4e/0x50 各 43k 次、_SetCPUASID+0x52 23k、
GeneralException+0x138 12k、_OEMCacheRangeFlush+0xda 98、_LoadKPage 与
_SH3Init 各 1），`kernel_devfuncs.py … --table kdev_funcs50 --check NK.bin
sites.txt`：25 个函数全部定位到，14 个访问点全在其中。

结果（同一镜像、同一双击，墙钟）：

| | KDEV/TLB_REFILL 关 | 开（CE 5.0 表 + 回填） |
|---|---|---|
| first idle / 显示驱动 | 45 s | 32 s |
| 桌面首帧 | 112 s | 73 s |
| Explorer 任务栏 | 129 s | 84 s |
| tlb_refills（到 131 s） | — | 24,914 |

启动日志里 `[kdev] device-aware nk.exe ranges: …`（9 段）和 `[dbt] TLB
refill in the translator: CE 5.0 handler (SectionTable walk)` 确认两者生效；
My Device 照常打开。

**不扫描，先对地址。** 最初是启动时按特征码扫描 nk.exe 的 .text（casio-emu
里 5.6 s，加首字预过滤后 3.8 s）。nk.exe 和翻译器是配套发行的，表里直接记下
每个函数在生成镜像里的 P1 地址（`hint` 字段，`kernel_devfuncs.py` 输出的
第四项），`kdev_locate` 先在记录的地址上核对首 4 字和 CRC，全部命中就跳过扫描
（日志 `[kdev] nk.exe functions at their recorded addresses`），POSTINIT 回到
2.3 s；只有重链接后函数挪了位才退回扫描，再找不到仍是整模块设备感知。
CE 6.0 的 `kernel_dev.h` 同样带 hint（手工补为 0，走扫描）。

**TLB 刷新为什么杀掉这么多块。** `-DDBT_INVAL_DIAG` 按来源统计失效：到 132 s
时"范围"类失效 216 次杀 77k 块，无范围的 HC_FLUSH（保留 ROM 块）25 次只杀
134 块，ldtlb 换页 14k 次杀 383 块。翻译总数是存活块数的 2.8 倍，几乎都是
这 77k 次重翻。来源追到两处，都是"所有用户态映射可能变了"的全杀：

1. CE 5.0 内核的 `InvalidateRange(addr, len)`：`INC\kernel.h` 里 SH 没有
   `HARDWARE_PT_PER_PROC`，宏展开成 `OEMCacheRangeFlush(0, 0,
   CACHE_SYNC_FLUSH_TLB)`——范围被丢掉（"there might be aliasing
   somewhere"）。每次 decommit、VirtualProtect、栈回收、换页（`virtmem.c`、
   `pgpool.c`、`physmem.c`）都走到这里。
2. OAL 对它的实现先写 MMUCR.TI 再发 F9。翻译器把 guest 写 TI 也当作
   "用户态映射全没了"（`rt.c` 设备感知存储钩子），所以用户态块在 F9 到达
   之前就已经全被杀了；F9 无范围再杀一次已经没东西可杀。两条路径合计每次
   约 357 块。

**按页表核对，而不是全丢（`rt.c user_mappings_changed` → `tlb_reval5`，
2026-11-09）。** TI 写、无范围的 F9、R-standby 唤醒三处都改走同一个函数：
认出 CE 5.0 的 TLB 处理程序（`g_tlbr_ok == 5`）后不再清空，而是对每个存活的
用户态块按它的 ASID 走一遍 CE 5.0 的页表（和 `tlb_refill5` 同一套：
SectionTable → SECTION → MEMBLOCK → aPages）：pc 仍映射到翻译时记下的物理页
（`binfo.phys`，翻译时从 UTLB 读出）就保留，页没了或换了才丢。slot 0 用
当前进程的 SectionTable[0]，别的进程的块用 SectionTable[procnum+1]（CE 5.0 的
ASID 就是 procnum；NK 的 NKSection 不在表里，非当前时丢）；slot 1–63 各进程
相同。跨页的块和 slot 0 里 ASID_ANY 的块保守地丢。内容变化不归这条路管：内核
写代码后会发 HC_FLUSH（有范围的 F2 在整个启动里没有一次杀超过 8 块）。
内核未改，对 CE 6.0 无影响（它的 TI 仍是全杀：`g_tlbr_ok` 是 6）。

两处细节：同一页上的块共用一次页表遍历（每次调用内按 (页, ASID) 记忆）；
OAL 对一次 InvalidateRange 先写 TI 再发 F9，两次之间除了 trapa 本身没有任何
事件（异常、翻译、ldtlb、失效计数都没变）时第二次直接跳过。

顺手改掉的另一处：ASID_ANY（多个进程共用，CE 5.0 下 slot 1 的全部 DLL 代码）
的块死掉时，`hash_remove` 原来要扫一遍 32768 项的哈希表，前一天先改成每批
一次扫描，现在改成按"guest 运行过的 ASID 集合"逐个探测它可能在的槽位
（`g_asid_seen`，switch_asid 记录；超过 64 个 ASID 才退回扫描）：条目只会在
插入时的当前 ASID 对应的槽位上。

结果（同一镜像、同一双击，墙钟，run9 → run16）：

| | 改前 | 改后 |
|---|---|---|
| first idle / 显示驱动 | 26.5 s | 15.1 s |
| 桌面首帧 | 69.1 s | 35.0 s |
| Explorer 任务栏 | 80.5 s | 43.0 s |
| 失效的块（到 ~130 s） | 77,823 | 1,156 |
| 翻译次数（到 ~130 s） | ~100,000 | 50,000 |
| 页表核对调用 / 其中跳过的重复 | — | 137 / 57 |

心跳采样（`HLE_HB=5`，casio-emu 每 5M 条指令打一个 PC，`hbprof.py` 符号化）
在前 43 s 里翻译器 C 代码仍占 68%：`dbt_translate_block` 9%、
`user_mappings_changed`（页表核对）7%、`invalidate_block` 7%、
`finish_invalidations` 6%、`tlb_drops_block`（ldtlb 钩子）5%、`sh4_decode` 5%；
翻译后的 guest 代码占 23%，片上 stub 8%。

**按页而不是按块核对（`rt.c` cpage_*，2026-11-09）。** 更细的采样
（`HLE_HB=2`）里页表核对在 shell 阶段（16–50 s）占 11%：每次都要走完
`g_user_idx`（含死块，最多 19200 项）。现在翻译器维护一张"用户态代码页"表：
每个有用户态块的 (4 KiB 页, ASID) 一项（slot 0 按 ASID 分，slot 1–63 共享），
记翻译时的物理页；块提交时登记（跨页的块登记两页，第二页从 UTLB 读），
slot 0 的块被另一个进程复用时也在该进程的 ASID 下登记。TLB 刷新时每页走一次
页表（几百次），物理页没变就什么都不做，变了才沿页链表杀这一页的块。
表 2048 项、开放寻址、满四分之三或遇到无法编码的键就退回逐块核对；bank
回收后按存活块把表重建到另一份缓冲里。表占的 32 KiB 来自 ASID 标签构建里
用不到的 `g_user_hash_seen`。

同一轮里：ASID_ANY 块的哈希清理改成按"guest 用过的 ASID 列表"探测
（原来是扫 256 位的位图），`PEND_RANGES` 从 16 降到 4（超过就用 32 字节粒度
的位图，`pend_dead` 对每个 IC/链接目标都要跑）。

| | run16 | run18 |
|---|---|---|
| first idle | 15.1 s | 13.6 s |
| 桌面首帧 | 35.0 s | 30.1 s |
| Explorer 任务栏 | 43.0 s | 38.2 s |

shell 阶段的翻译器剩余热点：`finish_invalidations` 13%（每批失效两遍扫全部
块头，批 = 每次 ldtlb 换页杀块 + 每次 TLB 刷新杀块，约 550 批/128 s）、
`lookup_or_translate` 8%（其中 `hash_evicted` 4 万次：直接映射哈希里按
(pc, ASID) 的条目互相挤出，退到 C 的块表查找）、`dbt_translate_block` 7%
（bank 回收 27 次/128 s 引起的重翻）、ldtlb 钩子 4%。

**死块入口改成转发，摘链接推迟到 bank 回收（`DBT_LAZY_DETACH`，默认开，
2026-11-09）。** 杀块时只记下死区间（原来的 pend_add），并把块开头 28 字节改写成
块出口 stub 进入查找 stub 的那套序列（r1 存进 Y0，X0 = 块的 guest pc，X1 = 0，
`jmp` 到 `dbt_lookup`，延迟槽里恢复 r1）；指向它的 IC、直接链接、哈希项都不动。
谁跳进死块，就多付一次哈希查找落到活的翻译上。不能用 trap：guest 在 SR.BL=1
下跑（内核初始化期间就有）时任何异常都会复位 CPU，第一版用 `trapa` 就是这样
在 2.5 s 死掉的。两遍全表扫描（`finish_invalidations`）只在 bank 回收和整表
清空前跑一次，因为那时死区间的内存要被重用。`-DDBT_EAGER_DETACH` 恢复旧行为。

| | run18 | run22 |
|---|---|---|
| first idle | 13.6 s | 13.8 s |
| 桌面首帧 | 30.1 s | 29.2 s |
| Explorer 任务栏 | 38.2 s | 36.7 s |

shell 阶段（16–45 s）翻译器 C 代码从 64% 降到 57%，`finish_invalidations`
从榜首消失；剩下的是查找退到 C（`lookup_or_translate` 8%，哈希冲突）、翻译本身
（`dbt_translate_block` 7% + `sh4_decode` 3% + `emit_flush_lits` 2%，bank 回收
19 次/100 s）和查找 stub 本身（`_dbt_lki_ts` 7%，这是正常的快路径）。

## 十、真机反馈（2026-11-09）

第一次真机运行 CE 5.0（`dbt/build-dev50/wince.hh3` + NK.bin/EXT.bin）后的两个问题：

- **Plotter-Z 载入任意 sample 后 Recalc 一开始就死机。** 模拟器里复现
  （Start → Programs → Plotter-Z → File → Samples → Saddle Surface →
  Render）：翻译器打印 `[dbt] fpu op 0000f34d status 00000002` 然后 halt。
  0xF34D 是 `fneg fr3`，此时 FPSCR.PR=1（双精度模式），fpuemu 把"PR=1 下的奇数
  寄存器"当作保留编码拒绝了。fneg/fabs 只翻/清 FRn 的符号位，PR=1、偶数 n 时
  那正是 DRn 的符号；CE 5.0 的 SH-4 编译器在双精度模式下也对奇数寄存器发
  fneg，硬件照样处理。改成无论 PR 都按 FRn 操作（`fpuemu/src/fpuemu.c`，
  host 测试加了回归项）。模拟器里 Recalc 随后正常推进到 100%。
  真机上看不到 halt 信息（没开面板控制台），表现为卡死。
- **点触摸条上的 Rotate 不转屏。** 模拟器里 `ROTATEKEY: mode=1 result=-2`
  （DISP_CHANGE_BADMODE）。反汇编 CE 5.0 的 gwes.exe：
  `Gwe::ChangeDisplaySettingsEx_I` 第一次被调用时用
  `GdiGetScreenRotation(short*)` 向驱动发 `DRVESC_GETSCREENROTATION`，
  **cjOut = 0**、pvOut 指向一个 16 位字（低字节当前角度，高字节支持的角度
  集合，CE 5.0 的示例驱动都不看 cjOut 直接写一个 DWORD）；驱动失败就把这个字
  记成 0，以后所有 `DM_DISPLAYORIENTATION` 请求都因"不支持该角度"返回 -2，
  只有 CDS_TEST 查询能过。我们的驱动沿用 CE 6.0 的 `cjOut >= 4` 检查，于是
  每次都失败。`classpad_ddi.cpp` 去掉了这个检查（只要 pvOut 非空就写）。
  需要在 VM 里重新 Build and Sysgen Current BSP + Make Run-Time Image。

**还有没有别的 FPU 指令会出事？** 静态扫了 NK.bin 和 EXT.bin 里所有 SH-4 模块的
.text（`tools/cesym.py` 的模块表），找 fpuemu 不实现的编码：fipr、ftrv、
fsca、fsrra、fpchg 以及 F?CD/F?DD、F???F 一类保留编码——除了 nk.exe .text 里
的数据字（异常向量区的字面量）没有命中，frchg/fschg 只在内核的
Save/RestoreFloatContext 里。剩下的风险是"PR=1 下的奇数寄存器"这类依赖运行
时模式的组合（float/ftrc/fsqrt/fcnvsd/fcnvds/fmac 和双精度算术），编译器
不该发它们，但 fneg 的教训是不能笃定。因此翻译器对 FPU_UNIMPL 不再 halt
整机，而是按硬件的一般非法指令（0x180，延迟槽 0x1A0）交给内核，出事的只是
那个线程；fpuemu 报告的使能 FPU 异常（FPSCR cause 已置、结果未写）同样作为
0x120 交给 guest，不再静默丢结果。统计里 `fpudis=a/b` 的 b 是这类投递的次数。

顺带量了 Recalc 为什么慢（模拟器里 20×20 的 Saddle Surface 约 100 s）。
按翻译记录把心跳采样的宿主 PC 映射回 guest 符号（`hbguest.py`；翻译记录要带
`hostlen`，并取样本时刻之前最新的那次翻译，否则 bank 回收过的旧块会把采样算到
别的函数头上——第一版就把 Plotter-Z 的代码错算成 filesys/OpenADll，用
`-DDBT_WATCH_GPC` 在 `LoadOneLibraryW` 入口放 trapa 一看，Recalc 期间根本没有
LoadLibrary）。真实分布：

| 占比 | 在哪 |
|---|---|
| 30% | Plotter-Z 自己的 `PaintCanvas16Bpp`：每次重绘把 8 位画布经 LUT 转成 16 位 DIB |
| 27% | 显示驱动 GPE 软件 blit 的逐像素颜色转换（画布 DIB 是 16 位 BI_RGB 即 RGB555，面板 RGB565） |
| 28% | GWES 的 TrueType 字形缩放（`scl_ComputeScaling`、`fs_ContourScan`、`mth_max_abs`）：命令栏随整窗失效一起重绘，字形每次重新光栅化 |
| 0.4% | coredll `pow`，真正的计算 |

起因是 `DrawRecalcProgress` 每算完一列就 `InvalidateRect(NULL)` + `UpdateWindow`。
已做：驱动给"16 位 → 16 位、带颜色转换的 SRCCOPY"加了查表快路径
（`classpad_ddi.cpp ConvertedCopyBlt16`：64K 项的表用 GWES 给的转换器填一次，
用四个探针值辨认转换器是否还是同一个；不处理拉伸、透明、alpha、掩码和旋转，
那些仍走 EmulatedBlt）；`platform.reg` 把 `SYSTEM\GDI\GLYPHCACHE\Limit` 提到
64 KiB（gwes.exe 里读的就是这个值名）。两者都要重建镜像才能量。Plotter-Z 自己
那 30% 和整窗失效是上游的事：进度条只失效自己的矩形、DIB 用 BI_BITFIELDS 565。

**帮助文件打不开。** Math Xpander 的 Help 是
`CreateProcess(L"PegHelp.exe", L"file:math1.htm#...")`，CE 5.0 镜像里没有
PegHelp.exe（CE 的那个是 IE 的 htmlhelp.exe），于是什么都不发生。把 CE 6.0
工作区里只依赖 coredll 的 `SRC\APPS\PEGHELP`（hpcore.c 解析/排版帮助文件用到的
HTML 子集，peghelp.c 用 GDI 画）搬进 `bsp/classpad50`，`platform.bib` 放进 EXT；
`make test` 的 `test_peghelp` 在宿主上解析 Xpander 的 6 个帮助文件、217 个链接。
同样要重建镜像。

**重建后的镜像（2026-11-09 10:30 的 NK.bin/EXT.bin）在模拟器里：** 触摸条
Rotate 转屏成功（`ROTATEKEY: mode=1 result=0 size=528x320`，桌面横过来了）；
Math Xpander 的 ? 按钮打开 PegHelp 的 "Workbook and Notes help"；Plotter-Z 的
Saddle Surface：新镜像从 Render 到 100% 约 54 s，旧镜像同一时刻才 60%（估计
全程 160 s 以上），快路径加字形缓存把 Recalc 缩到三分之一左右（宿主这一小时
比早上慢了约 1.5 倍，两次都在同一条件下跑）。开始菜单多了 Help 一项（有了
peghelp.exe 之后 shell 自己加的），Programs 从 y=371 挪到 349。

**转屏之后桌面图标颜色不对。** 像素比对：图标内部的颜色 = 图标颜色 OR 桌面
底色（(123,125,123)|(57,109,164) = (123,125,255)），即画图标的两步
（SRCAND 掩码、SRCPAINT 图像）里 AND 这一步没起作用；文字、填充、Windows CE
logo 都对。GPE 的 sysgen 版 gpe.lib 已经包含 emulrotate（gperotate.lib 只是
它的拷贝），1616 的 emul 例程和 `GPESurf::GetPtr` 的旋转寻址也核对过没问题，
剩下的嫌疑是 `EmulatedBltRotate_Internal` 通用路径里 1 bpp 源的取位。驱动里
加了转屏后前 96 次 blit 的日志（rop4、源/掩码格式、lookup/convert、裁剪），
等下一次重建拿数据。

## 九、还没做的

- 在 VM 里构建（第一次构建会暴露 SDK 的差异：头文件、`WARNISERROR`）。
- `tools/windows/claude-build` 的 CE 5.0 版本（现在是 C:\WINCE600 的路径和
  kern.dll 重链接逻辑）。
- 真机：R-standby、面板初始化黑屏问题与 CE 版本无关，仍待验证。
- 翻译器：`finish_invalidations` 每批两遍全表（可选：死块入口打
  trap、链接到 bank 回收时再摘，或把 IC 目标的 bank 也记进 `g_link_banks`，
  后者要改 stubs.S 里的 IC 回填）；哈希冲突（`hash_evicted` 4 万次/128 s，
  两路组相联 `DBT_HASH_TWO_WAY` 目前与 ASID 混合互斥，合并要改 `_dbt_lki_ts`）；
  bank 回收引起的重翻（翻译器与 CE 共享 RAM 的划分未重新评估）。
- 内核侧可选：`INC\kernel.h` 的 `InvalidateRange` 对 SH 传真实范围
  （一行改动，需在 VM 里重建 nk.lib），翻译器的 F9 有范围路径就能直接用，
  不必走页表核对；现在不改内核也已经不是瓶颈。
