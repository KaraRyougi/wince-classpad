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
- **两个区对调（2026-11-09）**：NK 进空洞（`NK 80EE0000 00500000`，
  `ROMSTART=80EE0000`），EXT 进 eActivity 区（ext.bib.hdr：
  `EXT 83860000 007A0000`）。理由见 docs/03 "The regions swapped"：空洞是
  Casio OS 在 OS 更新之外从不写的地方，放进去的 NK 一次烧写长期有效；
  eActivity 区每次 Casio OS 开机都会重新格式化，放那里的只有 Math Xpander
  和 mfcce300，缺了 CE 照常启动（OEMInit 校验不过就不挂链）。BSP 自己的程序
  （Plotter-Z、PegHelp、classpad_rotate/rotatekey、sh3crt、DocList、
  plotterz.lnk，共约 120 KiB）改到 NK，所以 NK.bin 一个文件就是完整系统。
  对调前的 NK 实占 4.88 MiB，加这 120 KiB、减 romexclude 新删的 ws2.dll
  （32 KiB，CE 5.0 镜像里没有导入者；servicesd 早已不在镜像里），余量约
  120 KiB，5 MiB 的空洞放得下但不宽裕；WordPad（约 720 KB）和三个游戏
  （约 200 KB）是下一步可删的。DLL 地址范围不变。同步改的常量：
  image_cfg.h `IMAGE_WINCE_CODE_PA`、classpad_extrom.h `CLASSPAD_EXT_ROM_PA/
  BYTES`、dbt/Makefile 的 `NK_WINDOW=hole`（新默认：NK 0x00EE0000/5 MiB，
  EXT 0x83860000/7.625 MiB；`eact` 保留为对调前镜像的布局）、rt/main.c 与
  hhk 的后备常量、tools/mkextrom.py 与 bench 脚本的默认地址。wince.hhk 的
  提示多了一项：**Keyboard 键 = 不重烧 EXT 直接启动**（EXT.bin 与 flash
  不一致时才出现），省掉 Casio OS 格式化之后 4 MB 的重烧。KDEV 的
  `rt/kernel_dev50.h` 记的是旧镜像里的地址（hint），新镜像上 kdev.c 会退回
  扫描；重建后用 `tools/kernel_devfuncs.py NK.bin kern.map --table
  kdev_funcs50 -o rt/kernel_dev50.h` 重做。
  第一次按新布局构建（2026-11-09）的两个教训：PB 的 Copy Files to Release
  Directory 跳过了被 PB 自己打开着的 config.bib 和 platform.bib（共享上这两个
  文件读写都被拒），镜像仍按旧布局出来，关掉它们再复制一次才对；
  **romimage 把区的 .bin/.nb0 写到第一个模块所在的目录**（NK.bin 落在
  release 目录只是因为 nk.exe 在那里），EXT 的第一个模块从 DocList.dll
  （release 目录）变成 mfcce300.dll 之后，EXT.bin 出现在
  `PLATFORM\CLASSPAD\FILES\legacy-mfc\`，PostRomImage.bat 现在按
  ext.modules.bib 第一项的目录把它搬回来。新镜像：NK.bin 实占 5,226,576
  字节，离 5 MiB 只差 16 KiB；EXT.bin 4.17 MiB，ROMHDR 的 CPU 字段随第一个
  模块变成 SH3（内核链接区时不看它）。
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
- SpreadCE 2.05（Pocket PC SH3 英文版，1.5 MB，/FIXED，无 .reloc）放在 EXT：
  `tools/sh3prep.py` 用 `pe4k.reconstruct_relocs_pdata` 从 .pdata、literal pool
  和 jsr/jmp 数据流（含沿控制流追踪的叶函数）重建 25556 个 fixup（人工复核，
  SHA-256 固定在 `sh3prep.RELOC_PINS`），再对齐、改 SH3CRT 导入（SH3CRT 新增
  `_nextafter`；EXE 的 SH3CRT IAT 放在 `.data` 末尾，因为 romimage 会把 EXE
  新加的可写节变成只读）。处理好的 exe 只在 VM 的 `FILES\spreadce`，见
  `FILES/spreadce/README.md`。模拟器里已能启动、计算整数公式。

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
驱动里加了转屏后前 96 次 blit 的日志（rop4、源/掩码格式、lookup/convert、
裁剪），重建后看到画图标的两步是：`rop4=8888 src=1bpp lookup=1`（掩码 AND，
经查找表），接着 `rop4=EEEE src=16bpp`（图像 OR）。OR 走 emulrotate 的 1616
例程没问题；AND 因为带 lookup 被 `EmulatedBltSelect16` 放回 GPE 的通用旋转路径，
而那条路径对它不起作用。没有再追 GPE 里的具体原因：驱动里对"1 bpp 源 + lookup
+ 旋转的目标 + SRCAND/SRCPAINT/SRCINVERT/SRCCOPY"自己逐像素做
（`RotatedMonoBlt16`，经 `GPESurf::GetPtr(x, y)` 寻址，处理裁剪，标脏行）。
重建后验证：转屏后 "My Device" 图标的 181 种颜色与未转屏时完全一致。

**转屏之后开始菜单的图标是黑色剪影**（真机反馈，模拟器里 Rotate 条图标 →
横屏 → 点开始复现，`scratchpad/rot`）。菜单项左边的小图标只剩掩码 AND 出来的
黑影，图像那一步没画上；桌面图标和 Start 按钮的 logo 正常。和上一条同源：
菜单图标来自 commctrl 的 image list，图像位图不是 16 bpp 直拷（带 lookup 的
4/8 bpp 或带 converter 的 16 bpp），emulrotate 的 `EmulatedBltSelect16` 凡带
lookup/convert/BLT_TRANSPARENT 的都不接，GPE 的通用旋转路径又画错。驱动里把
`RotatedMonoBlt16` 先扩成了覆盖 lookup/converter/透明色的 `RotatedLookupBlt16`，
但这只是又补一个洞：图案画刷、屏到屏拷贝（源也旋转）、拉伸、Alpha 仍走 GPE
的通用旋转路径，正确性未知，而且旋转表面上每个像素都经 `GetPtr` 虚函数，
横屏比竖屏慢好几倍。于是改为**在 GPE 之下旋转**（同日，重建后验证通过：模拟器里 Rotate 条图标 → 横屏开始菜单的图标全部正常、桌面四个图标正常、再点 Rotate 回到竖屏桌面完好，`scratchpad/rot2`）：

- 帧缓冲还是那 320×528×2 字节，但转屏后把它重新布局成**不旋转的** 528×320
  线性表面（`ClasspadSurf::Relayout`：stride 改 1056，`SetRotation(w, h,
  DMDO_0)`；GPESurf 的 stride 是 protected，所以要一个子类）。`IsRotate()`
  永远为假，GWES/GPE 画什么都是竖屏那套线性路径，emulrotate 和通用旋转例程
  不再参与；`RotatedLookupBlt16`、`RotateRectl` 换算全部删掉，脏行就是逻辑行。
  GWES 和应用不靠这个标志重排 UI：它们看到的是 `DRVESC_SETSCREENROTATION`
  的返回和 `GetModeInfo` 的新尺寸（GWES 据此改 SM_CXSCREEN、广播
  WM_SETTINGCHANGE、旋转触摸坐标），主表面是 `EngCreateDeviceSurface` 的设备
  表面，GWES 不直接碰位图。
- 旋转发生在推屏（`classpad_lcdrot.h`，驱动、翻译器、主机测试共用）：逻辑
  (lx, ly) 在面板上的位置沿用 GPE `GetPtr` 的约定（90°：(ly, 527−lx)；
  180°：(319−lx, 527−ly)；270°：(319−ly, lx)），所以触摸映射和之前一致。
  逻辑行 [top, bottom) 对应一个物理窗口（90/270 时是列带），`PushRows` 用
  `HC_LCD_PUSH` 的旋转形式（r7 = 0xA5000000 | 角度，r5/r6 = 物理行/列窗口）
  让翻译器按面板顺序转置读出并写入，每次 4 个物理行一段；没有翻译器时驱动
  自己转置写。`bsp/tests/test_lcdrot` 对 320×528 和 7×5 两种尺寸穷举校验映射
  与窗口，`dcsrot` guest 在模拟器里把一帧横屏图推出来，四个象限、标记、边框
  都落在预期位置。

**重建镜像上 Recalc 的剩余分布（Render 到 100% 约 60 s，采样 398 个）：**
Plotter-Z 自己的 `PaintCanvas16Bpp` 52%、驱动的 `ConvertedCopyBlt16` 8%、
FPU 模拟（`fpuemu_exec` + 辅助 stub + `dbt_fpu_c`）16%、GWES 字形 0%（字形
缓存起效了）、`pow` 本身 0.3%。于是把 Plotter-Z 的前端改了三处（见
`SRC\APPS\PLOTTERZ\README.md`）：16 位 DIB 建成 BI_BITFIELDS 565、按
`ps.rcPaint` 的矩形逐行转换（一字节四像素）并只 BitBlt 那块、进度条只失效
自己的矩形。预期 Recalc 剩下的主要是 FPU 模拟。待重建测量。

**FPU 寄存器搬运就地翻译（`translate.c tr_fpu_inline`，默认开，
`-DDBT_NO_FPU_INLINE` 关）。** `-DDBT_FPU_HIST` 统计 Recalc 期间的 FPU 指令：
73.5 万条里 `fmov FRm,FRn` 占 31%、`lds/sts fpscr/fpul` 占 30%（CE 编译器每做
一次双精度就切一次 PR）、fldi0/1 和 flds/fsts 再占 5%，算术只有约 10%。这些搬运
不碰内存、不出异常，原来每条都走辅助 stub（保存整套 guest 上下文、解码、
约 650 条宿主指令）。现在在 SR.FD 清零（否则仍由 stub 抛 FPU-disable 异常，
内核的惰性 FPU 上下文切换靠它）且 fmov 的 FPSCR.SZ 清零时，直接对 fpuemu 的
寄存器文件操作，约 15 条宿主指令；FR 位选 bank（+64 字节），T 经 DSP 槽保存
恢复，慢路径重新登记 spill 记录以便中断时恢复。顺带修了解码器的一个老问题：
`lds Rm,FPSCR/FPUL` 的寄存器在 bit 8–11，解码器一直按 bit 4–7 记 touched/defs
（辅助路径保存全部寄存器所以没出事；就地翻译选临时寄存器时撞上了，导致启动
黑屏）。guest 测试 `fpu`（加了 bank 切换、T 保持、SZ=1 成对搬运的用例）和
`fpudis` 通过。用每秒一帧的屏幕快照（`HLE_FBDUMP_EVERY=1`）对 Render 到曲面
出现之间的模拟指令数计量（`recalc_cost.py`，与宿主速度无关）：全部走 stub 时
10.19 亿条，就地翻译后 9.07 亿条，少 11%，和估计一致。

重建后的镜像（Plotter-Z 的 565 后台缓冲 + 只重绘变化矩形，字形缓存 64 KiB）
上同样的计量：9.07 亿条降到 2.56 亿条（每秒一帧的分辨率，误差约 ±0.4 亿），
Render 到曲面出现从模拟器里的 60 s 缩到 19 s。曲面区域逐像素和之前的镜像一致，
只有状态行与任务栏时钟不同。Recalc 剩下的部分按之前的采样是字形光栅化、
GPE 的 blit 和 Plotter-Z 自己的算术，各自都不再占大头。

**Plotter-Z：拖动和首次画完（2026-11-09 下午，同一镜像）。** 用户反馈首次
画完和拖动旋转仍然慢。量法：`HLE_HB=2` 心跳、`-DDBT_TRACE_BLOCKS`、
`-DDBT_WATCH_GPC=<Redraw 或 PaintCanvasToWindow 的入口>`（每次进入打印一行，
夹在心跳之间就是指令数时间戳），`HLE_CB_TOUCHSEQ` 的七次点按之后接 41 个
首尾相接的条目（每 5000 tick 右移 3 像素，条目之间 1 tick 的抬笔触摸驱动看
不到）当作拖动。注意 CE 5.0 的 exe 都装在 0x10000，`cesym` 会把 Plotter-Z 的块
记到 filesys.exe 头上，exe 范围的 gpc 要直接对 PlotterZ.map。

- 拖动：每次 `Redraw` 约 3700 万条指令，81% 是开头的整画布清屏
  （`FillRectCanvas` 逐像素调 `SetPixelCanvas`，15 万次 `jsr @reg`/`rts`），
  画线约 5%。而且 41 步拖动期间 `PaintCanvasToWindow` 一次都没进：`Redraw`
  比触笔采样间隔慢，队列里总有鼠标消息，WM_PAINT 排不上，笔抬起才画。
- Recalc 共 2.56 亿条：点 Render 到进度条首帧 5200 万（清屏 3000 万），20 列
  每列约 800 万（约六成是 `pow` 的浮点模拟，约 15% 是进度条的逐像素填充和
  反色），最后的 `Redraw` 加绘制 4000 万（又一次清屏）。
- 翻译器侧看到的：`_dbt_lki_ts` 占拖动采样的 33–41%。`HASH_TAG=1`
  （`DBT_USER_IC_HASH_HIT`）下用户态哈希命中不回填内联缓存，`switch_asid`
  又会清缓存，于是用户态的每次间接跳转都走约 45 条指令的哈希探测。
  已改（`USER_IC_REFILL=1`，docs/04 "User inline caches refilled from the
  hash"）：同一镜像、同一个旧 PlotterZ.exe，拖动时 `Redraw` 间隔从 3740 万
  降到 1770 万条，stub 的采样占比从四成多降到 2%，Recalc 从 2.18 亿到 1.90 亿。

Plotter-Z 改了五处（`SRC\APPS\PLOTTERZ\README.md`）：`FillRectCanvas`/
`InvertRectCanvas` 裁剪一次后按行按字节做，画线和字形循环就地写像素，
`WM_MOUSEMOVE` 重画后立刻 `UpdateWindow`，相机角度的 sin/cos 查
`pz_trig_table.h`（整数度），求值器里 -8 到 8 的整数次幂改连乘。画布图元在
Mac 上与旧实现逐字节对照过（20 万组随机输入），整个程序用 clang 对 CE 5.0
SDK 头文件做了语法检查。**还没在 VM 里构建，效果待重建镜像后测量。**

**合并 `claude/plotter-z-performance`（2026-11-09）：** Plotter-Z 的重绘与
重算改动（按字节的填充/反色、相机 sin/cos 查表、整数次幂用乘法）加翻译器的
用户 IC 回填（`USER_IC_REFILL=1`）。模拟器里同一套 Saddle Surface 计量：
合并后的翻译器配旧镜像 2.71 亿条，配重建镜像 **0.96 亿条**（每秒一帧的
分辨率下误差约 ±0.4 亿，已接近量程下限），Render 到出图约 5 s；最终帧的
曲面区域与之前逐像素一致。对调前的 10.19 亿到现在约十倍。NK.bin 因 trig 表
涨 2 KB，空洞余量 12 KiB。

**Math Xpander 的函数绘图和几何绘图只用屏幕左边 240 像素**（真机反馈，
2026-11-09；模拟器复现：Graph 面板的坐标轴到 x≈230 为止，右边 80 像素空白，
右上角残留一条灰线，`scratchpad/xp1`）。程序按 HP Xpander 的 240×320 屏幕写死了
三处尺寸，用 `sh-elf-objdump -m sh3` 反汇编加上这些 DLL 导出的 C++ 修饰名
（`??0GraphicalDomainC@...@NNNNNNGG@Z`、`?OnCreate@RectPlaneViewC@...`，
coredll 导入按序号经 coredll.def 还原）定位：

- GraphFrame.dll 和 GeometryPane.dll 各 4 处 `mov #127,rN; add #104,rN` = 231：
  平面视图的布局矩形和 GraphicalDomainC（世界坐标到像素的映射）的像素宽度
  （高 161 / 211 不动）。SH-3 立即数最大 127，231 这样拼出来，所以按 32 位字
  扫描 240/231 一无所获。改成 `mov #78,rN; shll2 rN` = 312（两条指令能拼出的
  最宽值，320 − 312 = 8，与原来 240 − 231 = 9 的边距相当）。
- GraphicsPlane.dll `RectPlaneViewC::OnCreate` 用 `CreateCompatibleBitmap(dc,
  240, 320)` 建后台缓冲，平面上所有绘制经它再贴到窗口，所以只改前两处时
  坐标轴居中到 156 却仍在 x=240 被切。改成 320×512（`mov #80,r5; shll2`、
  `mov #2,r6; shll8`；512 是两条指令能拼出的不超过屏高的最大值）。

`tools/xpander_widen.py` 按指令模式找这些点并核对数量（三个 DLL 共 9 处），
stage.py 在 sh3imports 之后调用它，VM 里 FILES\xpander 的拷贝已就地打过；
`--check` 只报告。模拟器验证（直接把补丁打进 EXT.bin 的记录并重算校验和，不等
makeimg）：Graph 的坐标轴从 x=1 到 308，两端的 −11.5 / 11.5 标签都在，y 轴居中
156；Geometry 面板的残留灰线消失。下次 makeimg 生成的 EXT.bin 与这份一致。

**电池电量驱动与 NK 腾空间（2026-11-09，待 VM 重建验证）。**

- Casio OS 02.01.7003 的做法（静态反汇编）：电池在 A/D 通道 AN2（ADDRC
  0xA4610084，10 位值 = 寄存器 >> 6），`0x8002A340` 连读两次取平均；
  `0x800B0354` 按电池类型（"Battery Settings" 的选择，RAM 标志 0x8C1BE9F8）
  分四档并单向锁存（只变差不回升）：标志 0 为 >475 / >429 / >383 / 其余，
  标志 ≠0 为 >465 / >447 / >429 / 其余；`0x800813EA` 是更低的开机门限
  347 / 402。按"1 个读数 ≈ 10 mV 电池组电压"换算，前者是每节 1.19 / 1.07 /
  0.96 V、门限 0.87 V（碱性），后者 1.16 / 1.12 / 1.07 V、门限 1.00 V
  （镍氢）。比例和"标志 0 = 碱性"都是从阈值推断的，没有量过分压。
- CE 侧：`SRC\DRIVERS\BATTDRVR\battif.c` 是公共电池 MDD（battdrvr_lib）的 PDD，
  轮询转换（不开 A/D 中断），逻辑在 `classpad_battery.h`（`bsp/tests/test_battery`
  覆盖阈值边界、锁存、百分比单调）。上报：BatteryFlag HIGH（Casio 1–3 档）/
  LOW（4 档）/ CRITICAL（门限以下），BatteryLifePercent（Casio 第 1 档即 100%，
  往下分段线性：第 2 档下沿 67%、第 3 档下沿 33%、门限 0%），BatteryVoltage，
  BatteryChemistry。CE 没有内建的碱性/镍氢切换，
  类型放在 `HKLM\Software\ClassPad\Battery` 的 `Type`（0 碱性、1 镍氢），
  `MilliVoltsPerCount` 可校正比例；档位变化时把原始读数打到调试输出，便于和
  Casio OS 的显示对照。类型改变时锁存重新开始。`SRC\APPS\BATTERY`
  （classpad_battery.exe，开始菜单 Programs\Battery）显示百分比、电压、状态并
  提供 Alkaline / Ni-MH 两个按钮写这个注册表值；注册表在 RAM 里，选择只保持到
  下次启动 CE，默认值在 platform.reg。需要 SYSGEN_BATTERY（winceos.bat：
  coredll 加 battery 组件才有 GetSystemPowerStatusEx2，CE_MODULES 加 battdrvr
  才 sysgen 出 battdrvr_lib，还带出控制面板的 Power 小程序）；PB 的 catalog
  视图里找不到这一项，所以直接写在 `classpad.bat` 里（`set SYSGEN_BATTERY=1`）。
  Power Manager 保持 Minimal 即可。
- 腾空间：NK 在空洞里只剩 12 KiB。`FILES\extmove.txt`（每行一个 findstr
  正则，和 romexclude.txt 同样的写法）列出要从 NK 改到 EXT 的微软条目：
  WordPad（pword、pwwiff、office、pwd_res、riched20、模板和链接）和两个纸牌
  游戏（pegcards、solitare、freecell 及链接、帮助页），共 16 条约 0.9 MB；
  PostFmergeBib.bat 新增一步把这些行改写为 `名字 路径 EXT 标志` 再交给原来的
  EXT 抽取。EXT 的 DLL 代码范围从 1 MiB 放宽到 3 MiB
  （02100000..02400000，NK 改为 02400000..04000000）。EXT 不在时桌面的
  WordPad 快捷方式和游戏菜单项随 .lnk 一起消失，和 Xpander 一样。

上面两项的 VM 构建与模拟器验证（2026-11-10）：NK.bin 实占 4,372,980 字节
（空洞余量约 850 KiB），EXT.bin 4.83 MiB、34 模块。构建中踩到的坑：增量 sysgen
不重建 `coredll_ALL.lib`（def 已含电池导出，链接缺 6 个符号，删掉它重来）；
sysgen 不提供 battdrvr.def，common.bib/common.reg 也没有电池驱动的条目，都由
BSP 自带；romimage 以区里第一个模块定 CPU 类型，EXT 以 SH3 的 mfcce300 开头时
拒绝 SH4 模块，所以挪过去的条目排在最前；备用电池要报 UNKNOWN，报 NO_BATTERY
任务栏每次开机弹 "Backup Battery Very Low"。模拟器（电池固定 4.70 V）：驱动
读数 470，碱性第 2 档 96%，Battery 程序切到镍氢后第 1 档 100%；WordPad 从 EXT
启动正常；无弹窗。Low / Critical 两档只有主机测试覆盖。

**真机电池读数与时钟校准（2026-11-10）。** 电池：第一版 PDD 照搬了 Casio 开机时的
A/D 设置（ADCUST = 0x8000、ADCCSR = 0x1CF，那是多通道用途的），真机上转换永不
结束（"No reading"）；改成 Casio 读电池前每次做的序列（0x8002A710：ADCCSR 清
bit 13、ADCUST = 0、ADCSR = 0x42 再启动）后真机读数正常。casio-emu 只模拟 ADCSR，
两种写法在模拟器里没有区别。采样间隔 2 s 改 30 s，Battery 程序 10 s 刷新一次。
时钟：TMU0 的计数率原来是常数（Pphi 29.4912 MHz / 4），而 Pphi 取决于进 CE 之前
时钟发生器的设置（超频工具会改）。`InitClock` 现在开机时用 RTC 的 128 Hz 计数器
R64CNT（独立的 32.768 kHz 晶振）量一次：TMU0 自由计数 16 个 RTC 计数（125 ms），
得到每毫秒计数，RTC 没开、不走或结果超出标称值的 1/8..8 倍时退回标称值；恢复
（R-standby 后再次 InitClock）沿用开机时的结果。tick、GetTickCount、Sleep、
性能计数器、OALStall 都跟着这个值。`bsp/tests/test_timer50` 模拟了五种频率和三种
失败情形。没有跟着校准的：触摸 I2C 的时钟分频、LCD 时序这些按 Pphi 算的硬件
定时。待 VM 重建后在模拟器和真机验证。

**USB 供电检测与系统帮助页（2026-11-10，待重建验证）。** 电池 PDD 的
ACLineStatus 现在取 USB 功能控制器的 VBUS 输入电平（INTSTS0 0xA4D80040 bit 7
VBSTS，连读三次一致才算）；Casio OS 自己也是这么读的（0x80016742 就是
`return (INTSTS0 & 0x80) != 0`）。只读这一位，不碰 USB 的其他部分；控制器模块
时钟沿用 Casio OS 留下的状态，没有在真机上验证过停钟时读到什么。casio-emu 的
ClassPad 配置固定报未插线。Battery 程序在插线时显示 "USB power connected"，
底部那行寄存器诊断已去掉。系统帮助：开始菜单的 Help 运行
`\Windows\peghelp.exe file:wince.htm`，我们的 PegHelp 在，但帮助页本身在微软的
bib 里挂在它的 IE 版查看器的条件下（`WCESHELLFE_MODULES_PEGHELP ||
IE_MODULES_HTMLHELP`），所以镜像里一页都没有。platform.bib 现在直接从 PUBLIC 树
列出 wince.htm、ctpnl.htm、copy.htm、rnetw.htm 和它们引用的 9 个 .2bp 图标
（NK，约 155 KB），word.htm 跟 WordPad 进 EXT；`test_peghelp` 的解析器过了这
几页的 478 个链接。

**Xpander 平面宽度 312 改 316；USB 读取加保护（2026-11-10）。** 用 Function Plot
的 Sine 类型画 y = sin x（Type 下拉 → Sine → 公式，OK）：312 宽时曲线到第 313 列为止，
离右边框（318）还有 5 像素空着，这就是"函数画不到最右侧"；平面从第 2 列开始，
所以 316（`mov #79; shll2`）正好填满，重画后曲线到第 317 列。casio-emu 的
ClassPad 配置没有 USB 控制器，电池 PDD 每次读 VBUS 都在 device.exe 里抛异常
（任务栏每秒轮询两次，拖慢了整个界面，注入的点击都对不上时间）；PDD 现在第一次
读用 `__try` 包住，读不到就不再读并报电池供电。时钟实测去掉了对 RCR2.START 的
检查（casio-emu 里该位为 0 但 128 Hz 分频照走）。

**重建后的验证；帮助目录页 mainhelp.htm（2026-11-09 晚）。** VM 重建的镜像在
casio-emu 里：OAL 报 `TMU0 counts 1772920 Hz by the RTC (nominal 7372800)`
（另一次 1625320）：casio-emu 的 RTC 走宿主机时间而 TMU 走模拟指令，翻译器下的
机器比实时慢约四倍，量出来的就是这个比值，说明实测生效；副作用是模拟器里系统
时钟比以前快四倍，对话框还没画完点击就到了（注入序列要拉长间隔），空闲约两分钟
墙上时间就关屏，取帧要取关屏前的。USB：device.exe 里只有第一次读的一个异常，
随后 `no USB controller to read, AC line reported offline`。Xpander（VM 构建的
EXT）：正弦曲线到第 317 列。帮助：Start > Help 打开了 PegHelp，但报
`Cannot open the help file mainhelp.htm`：explorer 的 IDS_HELPFILE 是
`mainhelp.htm#Help_Contents`。全树（PUBLIC、PRIVATE）没有这个文件：它是微软的
查看器运行时生成的（`PUBLIC\IE\OAK\HTMLHELP\helpstm.cpp` GetContentsPage）：枚举
`\Windows\Help\*.lnk`，快捷方式的名字作一行文字，目标加 `#Main_Contents` 作链接。
我们的 PegHelp 现在照做（`hp_lnk_target`、`hp_contents_page` 在 hpcore.c，主机测试
覆盖；peghelp.c 对 mainhelp.htm 和 help:contents 不读文件而是现生成，按名字排序，
页面上只有标题和链接）。先前写的静态 `FILES\mainhelp.htm` 已从仓库删除。微软的
快捷方式（wincehlp、ctpnlhlp、rnetwhlp、copyrt、wordhlp.lnk）同样挂在那个条件下，
platform.bib 现在列出它们，另加 `FILES\xpander\mathxpanderhlp.lnk`；platform.dat
建 `\Windows\Help` 并按微软的名字放入六个快捷方式，WordPad 和 Math Xpander 的在
EXT，没有 EXT 时目录里就没有这两行。VM 重建后在 casio-emu 验证（2026-11-09 夜，
镜像含合并进来的 SpreadCE）：Start > Help 出 "Help Contents"，六行（Copyright
Information、Math Xpander、Remote Connections、Settings、Windows CE Basics、
WordPad），点 Math Xpander、Windows CE Basics、WordPad（EXT）都进到各自的页，Back
回到目录。没有逐条点的：Settings、Remote Connections、Copyright Information。
KDEV 表按新 nk.exe 重新生成
（与加了 SpreadCE 的镜像算出的表相同）。注意：两个会话都往同一个
`PLATFORM\CLASSPAD` 暂存，`stage_bsp50.py` 会把另一边改过的文本文件盖掉
（这次盖了 SpreadCE 那边的 platform.bib、platform.dat、sh3crt.c/.def，已按那边
的工作树恢复，platform.bib 另带 mainhelp 一行）。

**USB 检测在真机上不生效；SpreadCE 慢在哪（2026-11-09 夜）。** 真机插 USB 不显示
外接电源。Casio OS 的 USB 断电例程（0x8000C8FE、0x8000CB90）写 SYSCFG = 0、
UPONCR（0xA40501D4）= 0、MSTPCR2 bit 20 置位、USBCLKCR（0xA4150014）bit 8 置位：
不用 USB 时模块是停的，停着的模块 INTSTS0 读出 0，而且没插线时本来就该停着。
没有找到单独的检测引脚（Casio 的上电路径 0x80008A60 附近同时设 USB 中断优先级，
靠模块自己的 VBUS 中断）。电池 PDD 改为采样（`UsbSample`）：每 5 秒按 gint 的上电
顺序恢复（USBCLKCR.CLKSTP = 0、MSTPCR2 bit 20 = 0、UPONCR = 0x0600、SYSCFG.SCKE），
等 2 ms，读 VBSTS（三次一致），再把四个寄存器原样写回；本来就开着的控制器只读
不动；USBE 始终不置。第一版（常开 PHY）按用户意见撤掉。寄存器原值和读到的
INTSTS0 写到注册表 `UsbDiag`（有变化才写），Battery 程序底部两行显示（真机没有
调试输出）。未在真机验证：时钟恢复 2 ms 后 VBSTS 是否已有效。只把 battif.c 和
battery.cpp 写到了 VM：VM 上另有一个会话加的 FUGUEDSK 存储驱动（classpad.bat、
platform.reg、platform.bib、DRIVERS\dirs），按用户意见不并入本分支。

**USB 采样按 Casio 的打开流程对齐（2026-11-09 深夜）。** 读了 OS 02.01.7003 的
USB 打开 0x8000CFD4：上电 0x800089E8，随后 0x80018F50 经 0x80016742 读一次 VBSTS，
为 0 返回 3；断电 0x80008A82。上电顺序：MSELCRA（0xA4050180）清 bit 7:6、MSELCRB
（0xA4050182）清 bit 15:14；USBCLKCR 清 bit 8；数 RTC R64CNT bit 0 的 14 次翻转
（约 110 ms）；MSTPCR2 清 bit 20；UPONCR = 0x0600；读写 0xFE00807C / 0xFE00808C
（清 0，不明）；屏蔽 USB 中断并设优先级。读 VBUS 前没有置 SYSCFG.SCKE，也没有
另外的延时。断电反过来：SYSCFG = 0、UPONCR = 0、MSTPCR2 置位、USBCLKCR 置位、
MSELCRB 置 0xC000、MSELCRA bit 7:6 = 01。`UsbSample` 现在照这个顺序做并按断电顺序
逐个恢复原值；因为有 110 ms，采样放到 PDD 自己的线程里（每 5 秒），
BatteryPDDGetStatus 只取结果。没照做的：中断控制器（不用中断）和 0xFE0080xx 两个
不明寄存器（gint 的 USB 驱动在这台机器上不碰它们也能用）。多做的一步：VBSTS 读到
0 时再置 SCKE 读一次；两次读数都进 `UsbDiag`。未在真机验证。

**Casio OS 怎么发现插线（2026-11-09 深夜，续）。** 上面读的 0x8000xxxx 至
0x8001xxxx 那一套在镜像最前面，它的打开函数带一个最长约 5 分钟的等 VBUS 循环
（数 R64CNT bit 5 的 1201 次翻转），应是引导/更新程序的 USB 代码；OS 主体里有同样
代码的另一份：上电 0x8061E63C（同样的 14 次翻转延时）、断电 0x8061E6FA、读 VBSTS
0x8062014E（经 0x80620DA4），"有线则连接" 0x80623060。OS 的做法是轮询，没有找到
单独的检测引脚或在模块停着时能用的中断：
- 0x80073BE8 是一个周期性的处理函数（进出保存 MACH/MACL，先查 SR 的中断屏蔽）。
  通信设置字节（0x8C1F6F45）为 2 且标志为 1 时，用一个计数器每 5 次调用一次状态机
  0x8007399C，其他模式每次都调用。
- 状态机先看软件状态（0x800A606A：RAM 里的标志，11 忙、8 已连接、3 未连接，不碰
  硬件）。未连接时调用 0x80072630，它走到 OS 的打开函数 0x800A634C：初始化、上电
  0x8061E63C、读一次 VBSTS（0x80623060），有线就继续连接，没有就走清理路径返回。
所以 Casio 也是定期把控制器开起来读一次。没有查清的：0x80073BE8 的调用周期（它的
入口 0x8003A49A、0x8003A4A0 和 0x80072038 在镜像里没有静态引用）；打开失败的清理
路径（0x800A61CC、0x800A61F2）是否就是断电。工具：scratchpad 的 xref.py（字面量
和 bsr/bra 引用）、walk.py（向上走调用链）。

**USB 供电检测在真机上生效（2026-11-10）。** 按 Casio 顺序采样的镜像在真机上插线
后 Battery 程序显示 "USB power connected"。两次读数（不置 SCKE / 置 SCKE）哪一次
读到的没有记录。Battery 程序底部的诊断行随后去掉，驱动仍把寄存器值写到注册表
`UsbDiag`。

**休眠期间时钟不走（2026-11-10）。** 真机上休眠后系统时间没有前进。rtc.c 早有补时
（`ClasspadRtcSuspend` / `ClasspadRtcResume`：休眠前后各读一次硬件 RTC 的日历，差值
加到 `g_baseSeconds`），但 `ReadHardwareSeconds` 要求 RCR2.START = 1，而 CE 启动时
日历计数器是停的（START = 0，ClassPad 自己没有时钟；只有 64 Hz 分频器在走，时钟
实测靠的就是它），所以每次读取都失败，补时被跳过；`OEMSetAlarmTime` 也因为同一个
读取而一直失败。现在 OEMInit 调 `ClasspadRtcStart`：START 为 0 时启动日历计数器，
日历里有合法日期就原样启动，否则先设成 2000-01-01 00:00:00；RCR2 的周期中断设置
保留。只用它的差值，CE 的日期不取自它。casio-emu（重建后的镜像）：启动报
`CLASSPAD RTC: calendar counters started (RCR2 was 00, date kept)`；空闲约 180 秒
墙上时间后自动休眠（现在报 "Clear/ON or RTC alarm wake"：内核闹钟能设上了），
86 秒后注入 Clear 键（码 20）唤醒，任务栏时钟休眠前 12:06，唤醒后第一次重画就是
12:07，10 秒后 12:08；清醒时模拟器里每 22 秒墙上时间走一分钟，不补时的话 12:07
要到唤醒后 10 秒才出现。补上的量约一分钟，分钟分辨率下与 86 秒大致相符，没有更
精确的读数。真机未验证。KDEV 表按新 nk.exe 重新生成（kern.map 多了一个函数）。

**老面板（2026-11-10）。** fx-CP400 有两种 DCS 面板，Casio OS（02.01.7003）的每个
LCD 例程都按面板 ID 分路（RDID 0xDA，一次空读后读 ID，0x8006407A）。ID 为 0x16 或
0x52 的是一类（我们原先唯一支持的）；其他 ID 是另一类，区别：
- `LCD_SetDrawingBounds`（0x80038A10）把列窗口两端各加 40，行不变；
- 背光亮度不是 0x51 / 0x52，而是寄存器 0xB9 四个参数里的第二个：设置时先读回四个
  参数再原样写回并替换第二个（0x80038550），读取取第二个参数（0x800385E0）；
- 关屏（0x8003809C）：DISPOFF、等 60、SLPIN、等 120，不发 0x51。
我们的改动：`dbt/rt/lcd_io.h` 的 `dbt_lcd_col0`（读一次 ID）用于 HC_LCD_PUSH 的两种
形式、面板控制台和休眠诊断条纹；`classpad_lcd.h` 的 `LcdReadId` / `LcdGetBacklight`
/ `LcdSetBacklight`，`classpad_ddi.cpp` 启动时读 ID、`SetWindow` 加偏移、休眠和恢复
按面板类型处理背光（老面板 CE 不进深度待机，所以也把亮度清零）。casio-emu：
`dbt/emu-patch/add_classpad_oldpanel.py`（`HLE_CP_LCD_ID=<hex>`：非 0x16/0x52 时
显示 GRAM 列 40..359，保存 0xB9 的参数并打印写入）。验证：`GUEST=dcsrot` 在 ID 16
和 ID 30 下截图逐字节相同；当前 CE 镜像加新翻译器在 ID 30 下桌面和开始菜单正常。
驱动里的背光和 SetWindow 改动要等 VM 重建才能测。真机未验证；老面板的具体 ID、
0xB9 其余三个参数的含义不明。

**休眠功耗高（新面板也是），调查到的差距（2026-11-10）。** Casio OS 进待机的入口
0x80020E4C 在设好引脚后调用 LCD 断电 0x80038A9A，它比 CE 多做三件事：面板深度待机
（新面板 0xB9 FF 52 98、0xBD 00、0xB1 51；老面板 0xB1 01）、PHDR（0xA405012E）
bit 5 清零、0xA44C0020 bit 2 清零（0x80081390 是把它置 1 的对应例程）。CE 只做到
SLPIN。深度待机后面板要完整重新初始化（Casio 的初始化例程在 0x800379xx 和
0x80037Fxx 附近，各有几百条指令），所以一直没照搬。另外 0x800813A0 的 ADC 设置把
PHDR bit 4 置 1，含义不明。这三步各占多少电流没有数据，也没有在真机上量过
R-standby 本身的电流。下一步的想法：用一个小的 SH-4 解释器跑 ROM 里的初始化例程，
把两种面板的上电命令序列抄出来，CE 休眠时照 Casio 的做法断电、恢复时重放。

**休眠一晚耗尽四节电池：先量再改（2026-11-10）。** 四节电池一晚上用完，平均约
100 mA，这是机器在运行的电流，不是 R-standby 的，也远超面板只睡眠不深度待机能
解释的量级。两种可能：(a) 内核一断电就被唤醒，`OEMPowerOff` 的循环
（`RStandbyOnce` 返回 0 就再进一次，仿 OS 的 0x80020ED2）整夜空转，2026-11-06 的
第三次真机测试就见过"一进待机立刻醒"；(b) 根本没进 R-standby，走了 CPU 睡眠的
后备路径。真机没有调试输出，所以加了统计（`classpad_pwrstat.h`）：OAL 记每次休眠
的 RTC 时长、R-standby 进入次数、无理由唤醒次数，以及第一次和最后一次无理由唤醒
时的唤醒源寄存器（RCR1、RCR2、INTREQ00、INTMSK00、KIUINTREG、KIUDATA0、
KIUSTATEREG、CMSTR、CMCSR、RWTCSR），经 `IOCTL_HAL_CLASSPAD_SUSPEND_STATS` 给
Battery 程序显示。RTC 启动时不再保留周期中断设置（PES = 0）。

与 Casio 对照，CE 的待机入口还缺的硬件步骤（都没做，等统计结果再定先后）：
- 0x800810CC 在进待机前后各调一个引脚配置例程（0x80081198 / 0x80081234：一串
  PULCR 上拉和引脚功能寄存器），0x80020E4C 里另有 PFC 的几步（docs/06 表 3.1 的
  2、9）；
- LCD 断电 0x80038A9A 的面板深度待机、PHDR bit 5、0xA44C0020 bit 2；
- KEYSC 中断优先级清零（表 3.1 的 5）。

**照 Casio 的关机流程做硬件断电（2026-11-10）。** 工具：`tools/sh4trace.py`，一个小的
SH-4 解释器，直接执行 NOR 转储里的 OS 例程并按顺序记录它对硬件的读写（读回值分别
填全 0 和全 1 各跑一遍，合并出每次读改写的 AND / OR 掩码；LCD 口按 RS 区分命令和
参数，0xDA 返回指定的面板 ID）。`tools/casio_poweroff_tables.py` 用它生成
`SRC/INC/classpad_pwrseq.h`。跑出来的 Casio 流程（顶层 0x800810CC）：
- 关机侧：0x800812C4 引脚表（26 次写：PULCR、MSELCRA/B、HIZCRA、PSELx、PACR..PVCR；
  与开机侧 0x800812CE 只差 PULCR、PHCR、PJCR、PRCR 四处）；0x800813A0 ADC（含 PHDR
  bit 4 置 1）；0x80081380 面板熄灭（0x51 = 0、DISPOFF、SLPIN）；RCR1 &= 0x0F、
  RCR2 &= 0x0F；0x80099A96 停 TMU1；0x8006F7DA 把 IRQ1 设成优先级 0xC 并解除屏蔽
  （待机的唤醒源之一，CE 不用）；然后 0x80020E4C：PFC 几步、LCD 断电 0x80038A9A
  （新面板 B9 FF 52 98 / BD 00 / B1 51，老面板 B1 01，随后 PHDR bit 5 清零、
  0xA44C0020 bit 2 清零）。
- 开机侧：引脚表、ADC、0x8008138C（LCD 接口：片选时序 FEC10014/34、LCD 引脚、
  MSTPCR2 bit 6、0xA44C0004/06/20）、面板冷启动 0x80038018：六个 0xFF、PADR bit 4
  拉低、**PHDR bit 5 拉高**、5 ms、PADR bit 4 拉高、120 ms，再是整套寄存器初始化
  （新面板 204 个码，老面板 162 个）。由此确认 PHDR bit 5 是面板电源、PADR bit 4 是
  面板复位；casio-emu 把 0xA44C0020 标为显示时钟控制。256 个面板 ID 逐个跑过：序列
  只分 0x16/0x52 与其他两种。亮度例程 0x80038280 另外处理（新面板 0x51；等级 0 还改
  0xA4050138 bit 4 和 B2/DF 的末参数）。
CE 这边（`power.c` 的 `HardwareOff` / `HardwareOn`，只在进了 R-standby 的路径上、
且显示驱动经 `IOCTL_HAL_CLASSPAD_PANEL` 报过面板类型时做）：屏蔽全部 IRQ 引脚；
按表设引脚（逐个先读后写，唤醒后倒序写回）；PFDR bit 7、0xA4050162 bit 2；SLPIN
后等 120 ms 发深度待机命令，PHDR bit 5 清零，0xA44C0020 bit 2 清零；RCR1 &= 0x0F。
唤醒后恢复引脚和 0xA44C0004/06，重放面板冷启动到命令 0x38 为止（不发 DISPON，0x51
的亮度写 0），再恢复 0xA44C0020 和 IRQ 屏蔽；之后显示驱动照旧 SLPOUT、恢复亮度、
重画、DISPON。与 Casio 不同的地方：IRQ1 不留作唤醒源；没有照做 ADC 那一步和
PHDR bit 4；片选时序寄存器没有重写（现有镜像唤醒后 LCD 能用，说明它们保住了）。
casio-emu：`dbt/emu-patch/add_classpad_lcdpower.py`（深度待机后帧为空白，必须先见到
冷启动的寄存器初始化再 SLPOUT 才恢复；`HLE_CP_LCD_LOG=1` 打印命令流；补上
0xA44C0006）。尚未在 VM 编译，模拟器和真机都未验证。

**IRQ1 / IRQ2 和"ADC 那一步"是什么（2026-11-10）。**
- Casio 待机前布置两个外部中断引脚作唤醒源（0x8006F7DA）。IRQ1 总是开
  （0x8006F63A：清请求、优先级 12、解除屏蔽；下降沿，ICR1 由 0x80062100 设），它的
  处理函数 0x8006F6D8 把 USB 插线状态机（0x8007399C）用的变量 0x8C1F6F48 清零并投递
  事件：IRQ1 是 **USB 插线**。这也回答了之前的问题：Casio 平时不是靠轮询控制器发现
  插线，而是靠这个引脚的中断，状态机随后才给控制器上电读 VBUS。IRQ2 只在通信设置
  满足 0x8006F78C 的条件时才开（0x8006F668：ICR1 bit 11:10 清零、优先级 5），是
  **串口通信唤醒**（Link 设置里的 Wakeup）；醒来后 0x80062156 重新屏蔽它。
- 对 CE 的含义：CE 从 Casio 继承了"IRQ1 未屏蔽、优先级 12"，却没有处理函数。只要
  INTREQ00 里锁存过一次下降沿（插过 USB 线）而没人清，它就是一个一直挂着的未屏蔽
  中断，R-standby 每次一进就醒，`OEMPowerOff` 的循环整夜空转。这与"一晚耗尽四节
  电池"相符，是目前最可能的主因；`HardwareOff` 已在进待机前屏蔽全部 IRQ 引脚。
- "ADC 那一步" 0x800813A0 其实是两件事：A/D 转换器（ADCCSR &= 0xDFFF、ADCUST =
  0x8000、0xA461008E 高 4 位 = 8），和 **触摸控制器断电**：PHDR bit 4 置 1 后等
  40 ms。触摸驱动模块在 0x8008A0xx..0x8008ABxx（IIC0）：断电 0x8008AA1C（0xA405019E
  bit 5 清零、PH4 = 1），上电 0x8008AA5C（PH4 = 0、等 20 ms、发单字节命令 0x02、
  0xA405019E bit 5 置 1）。CE 的触摸驱动休眠时只停了 IIC，控制器一直通着电。
- 改动：`classpad_touch.cpp` 的 PowerHandler 休眠时照 Casio 断电；恢复时由采样线程
  （能 Sleep）上电、发 0x02（`TouchBusCommand`，新加的单字节写事务），期间
  `g_suspended` 保持置位。OAL 的 `HardwareOff` 加上 A/D 那三次写，恢复时写回
  0xA461008E。未编译、未验证；触摸控制器对 0x02 的要求只来自反汇编。

**更正：IRQ 引脚早就被屏蔽了；USB 插线的电平在 PUDR bit 1（2026-11-10）。**
- 上一段说"CE 继承了未屏蔽的 IRQ1，是耗电的最可能主因"，查证后不成立：启动器的
  接管代码（`dbt/hhk/takeover.S`，默认 `IRQPINS=mask`）在离开 Casio OS 时已经把
  INTMSK00 写成全屏蔽，交付的镜像都是这样构建的。所以 IRQ1 在 CE 运行和待机期间一直
  是屏蔽的（除非被屏蔽的引脚请求也能结束 R-standby，这一点没有依据）。主因仍然未知，
  要看休眠统计。OAL 里对 IRQ 引脚的屏蔽（先在 `HardwareOff`，后挪到 `RStandbyOnce`）
  既然多余，已按用户意见全部去掉；快照里仍读 INTREQ00 / INTMSK00 供诊断。
- USB 插线的电平：Casio 用 0x8006F942 读 `(PUDR & 2) != 0`（PUDR = 0xA4050162，
  Port U bit 1，高 = 插着），调用者是 USB 状态机 0x8007267A（已连接状态下线拔掉的
  判断）、USB 打开的等待循环 0x80076E72、电池测量 0x8002A5CA，以及 0x80146FC4。
  0x8006F8F4 是它的去抖版本（6 次、间隔 15）。引脚表里 PUCR = 0x01DD，PU1 是输入。
  IRQ1 是同一事件的边沿中断，引脚不同。
- 电池驱动暂时没有改用它：`UsbDiag` 现在写"pin x controller y"，Battery 程序底部
  显示，一次真机测试就能看出两种读法是否一致；一致的话可以去掉每 5 秒唤醒控制器的
  采样，直接读这一位。

**VM 重建后的模拟器验证（2026-11-10）。** 镜像含：休眠统计、Casio 流程的硬件断电、
触摸控制器断电/上电、A/D 步骤、老面板驱动、USB 引脚诊断。空闲后自动休眠，注入
Clear（码 20）唤醒，再用触摸打开 Battery 程序：
- 新面板（默认 ID 16）：`CLASSPAD POWER: panel kind 1`；休眠时模拟器报
  `deep standby`，唤醒后 `out of deep standby`（即 SLPOUT 之前收到了冷启动的寄存器
  初始化），`CLASSPAD TOUCH: sensor powered up, start command sent`；桌面恢复，触摸
  可用；Battery 显示 `Suspend 1: 155 s, woke by Clear` / `Standby x1, stray wakes 0,
  hw off 1`（日志里 152 s 进、307 s 醒）。
- 老面板（`HLE_CP_LCD_ID=30`）：`panel kind 2`；休眠前 `B9 01 00 02 03`（亮度清零，
  其余三个参数原样），唤醒后初始化写 `B9 00 7f ff 08`，驱动随后写回亮度
  `B9 00 80 ff 08`；同样恢复，Battery 显示 `117 s` / `Standby x1, stray wakes 0,
  hw off 1`。
模拟器不模拟：引脚配置的电气效果、面板和触摸控制器的真实供电、无理由唤醒。KDEV 表
按新 nk.exe 重新生成。真机未验证。

**真机：Clear 唤不醒；原因在引脚表与内存待机的冲突（2026-11-10）。** 带硬件断电的
镜像在真机上休眠后按 Clear 没有反应。Casio 的引脚表在关机侧把 PULCRBSC
（0xA40501C3）写成 0x02、把 PHCR 里 PH6（bit 13:12）从 01（输出）改成 00；这两处属于
内存待机：翻译器的待机代码在 SLEEP 前照 OS 的 0x8002095E 做 `PULCRBSC |= 0x12`、
`PHCR &= 0xCFFF`，唤醒后把睡前**保存的值**写回。以前保存到的是运行值（PH6 = 输出），
现在保存到的是关机值，PH6 恢复成了"功能"，SDRAM 回不来，恢复代码跳进失效的内存。
OS 自己的恢复 0x800209C4 不依赖保存值：`PHCR = (PHCR & 0xCFFF) | 0x1000`、PHDR bit 6
清零、PULCRBSC 清 0x36，另外还写 PFCR &= 0xC003、PUDR |= 0x10、PUCR 的 PU4 设成
输出、0xA4050194/195/19E 几位（解释器在它的 R64CNT 等待处停下，前面是手读）。修法在
OAL：`HardwareOff` 跳过 PULCRBSC 那一项，PHCR 那一项保留原来的 PH6，这两处仍由翻译器
的待机代码处理，和改动之前一样。模拟器不模拟 SDRAM 自刷新和这些引脚，所以上一轮的
模拟器验证发现不了这个问题。这是推断出的原因，未在真机验证。

**背光怎么调（2026-11-10，只是调查）。** Casio 的亮度例程 0x80038280(level)，用
`tools/sh4trace.py` 对 0..13 各档和两种面板都跑了一遍：
- 14 档（0..13），超出范围按 7 处理；开机初始化里写的也是第 7 档。档位对应的数值
  两种面板相同：05(档 0，老面板 0B) 0F 14 1A 1F 26 2D 34 3B 42 49 52 5B 67。
  最大只到 0x67（255 的四成）。
- 新面板：命令 0x51 写该数值。老面板：寄存器 0xB9 的第二个参数写该数值（先读回四个
  参数），另外 C1 / C3 的第一个参数在档位 ≥ 1 时为 1、第三个参数 0x1F（档 0 为
  0x2D）；其余参数读回原样（解释器里读回是 0，所以表里显示 00）。
- 每次都先发 0x38（退出 idle 模式）。档位 ≥ 1：写完数值等 100 ms，把 0xA4050138
  bit 4 置 1（按端口排列是 PN4，引脚表里 PNCR = 0x0100 即 PN4 输出）。档 0 是特例：
  这一位清 0，新面板还把 B2 和 DF 的末参数从 9D 改成 FD。这一位的含义不明，看起来是
  背光驱动的一个开关或电流档。
- 读亮度 0x800385E0：新面板 0x52，老面板 0xB9 的第二个参数。
CE 现状：显示驱动只在休眠时把亮度清零、恢复时写回休眠前读到的值；读不到可用值时用
0xFF（`LCD_BRIGHTNESS_DEFAULT`），远高于 Casio 的最大档 0x67。没有调节界面。
`classpad_lcd.h` 已有 `LcdSetBacklight(isNew, level)`。要做调节：显示驱动加一个
DrvEscape 自定义码（与推送线程用同一把锁串行化 LCD 口），一个小程序或 Battery 程序
里加一组按钮调用 ExtEscape；档位表照搬上面 14 个值，档 0 的特例（PN4 和 B2/DF）
可以先不做，最低只到档 1。

**真机仍唤不醒：各步改成可选，默认全关（2026-11-10）。** 去掉 PULCRBSC / PH6 两项后
真机休眠后按 Clear 仍无反应，原因的推断不成立或不完整，不再靠推断：
- `IOCTL_HAL_CLASSPAD_SUSPEND_OPTIONS`（`classpad_pwrstat.h`）设一个掩码，默认 0：
  PINS（引脚表、PFDR bit 7、PUDR bit 2、RCR1）、PANEL（深度待机、面板电源和显示时钟、
  冷启动重放）、ADC、TOUCH（触摸控制器断电，触摸驱动从注册表
  `HKLM\Software\ClassPad\Suspend` "Options" 读自己的位，采样线程每 64 次轮询读
  一次）、PROBE（无理由唤醒时多读 INTREQ00、INTMSK00、CMT、RWDT；这些寄存器 CE 以前
  从没读过，默认只读 RCR1、RCR2、KIUINTREG、KIUDATA0）。
- 默认状态下休眠流程回到这几步加入之前的样子（另有计数和已证明安全的读数）。Battery
  程序底部五个复选框逐项打开；掩码在 RAM 里，重启后全关。统计行显示实际执行的步骤掩码。
- 没有在真机上验证过的假设：默认状态确实能唤醒。如果默认也不行，问题在这些步骤之外
  （统计、RTC、显示或触摸驱动的其他改动）。

**背光亮度进系统设置（2026-11-10）。** CE 5.0 的"显示属性"有 Backlight 页（cplmain 的
CPL_Screen：Background、Appearance、Backlight；镜像里有 cplmain.cpl），只管超时；
`screen.cpp` 在 `HKCU\ControlPanel\BackLight` "AdvancedCPL" 指了 DLL 且它导出
`BacklightAdvApplet(HWND)` 时显示 Advanced 按钮。`SRC\APPS\BKLCPL` 的
classpad_bkl.dll 就是这个 DLL：一个不用资源脚本的小窗口（Darker / Brighter / OK），
经显示驱动的两个 DrvEscape 码（`classpad_bkl.h`：100500 读、100501 设）调 1..13 档，
数值表照 Casio。驱动启动时把面板里读到的亮度归到最近的一档；设档时写面板并记为
休眠恢复用的值；LCD 口加了一把锁（推送线程与设档互斥）。读不到亮度时的默认值从
0xFF 改为 0x34（第 7 档），可用下限从 0x10 改为 0x05。档 0 不提供。未编译、未验证；
写 VM 时 platform.bib 和 platform.reg 带着另一会话的改动，只追加了本分支的几行。

**默认状态也唤不醒：改用条纹诊断（2026-11-10）。** 各步全关的镜像在真机上休眠后
Clear 仍无反应，所以问题不在五个可选步骤里。上一个确认能唤醒的是 2026-11-10 00:13
交付的那版（RTC 日历启动）；之后进镜像的还有：休眠统计、显示驱动（面板 ID、背光读写
改走 `LcdGetBacklight` / `LcdSetBacklight`、亮度默认值和下限、LCD 口的锁、亮度控制码）、
触摸驱动的电源处理重排、电池驱动的 USB 引脚诊断、翻译器的 `dbt_lcd_col0`、亮度 DLL、
以及另一会话的存储驱动和 FATFS。不再逐个猜：发了 `STANDBY_DIAG=1` 的 hh3
（`dbt/build-dev50diag/wince-diag.hh3`，NK / EXT 不变），唤醒路径每过一个阶段画一条
66 行的条纹（docs/06 8.1）。casio-emu 里用当前镜像验证：红、黄、绿、青、蓝、白、
钢蓝、品红八条依次出现，然后回到桌面。

**真机条纹结果：待机一进就醒，循环不止（2026-11-10）。** 用 `wince-diag.hh3`：
Shift + Clear 休眠后条纹立刻出现，最后一条灰色（OAL 认为唤醒无理由，再睡）；再按 Clear
约 1 秒后最后一条变品红，CE 恢复。唤醒后 Battery 程序：
```
Suspend 4: 27 s, woke by Clear
Standby x13, stray wakes 12, steps 00
1: 08 01 00 00 4800 0000 0000 0000 0000 0000
n: 08 01 00 00 4800 0000 0000 0000 0000 0000
USB pin 0 controller 0 (ints 0000 0000)
```
- 27 秒里进了 13 次待机，12 次是无理由唤醒：不是松键造成的一次性唤醒，而是每次一进
  R-standby 就被唤醒（诊断版每圈要画约 2 秒条纹，所以是 13 次；普通版每圈几十毫秒，
  整夜空转）。这就是一晚耗尽四节电池的原因，现在是实测，不再是推断。唤醒链路本身
  完好（八个阶段都过，内存没丢）。
- 快照（PROBE 没开，后六项是 0）：RCR1 = 0x08（AIE 置位，AF 和 CF 都是 0，不是闹钟）、
  RCR2 = 0x01、KIUINTREG = 0x4800（事件标志为 0）、KIUDATA0 = 0（Clear 没按着）。
  唤醒源不在已读的这几项里；下一步让用户勾 Probe 再做一次，读 INTREQ00、INTMSK00、
  KIUSTATEREG、CMSTR、CMCSR、RWTCSR。
- 普通 hh3 下"唤不醒"与诊断版能唤醒的差别还没有解释：两者 NK 相同，诊断版的恢复
  代码每圈都自己唤醒面板并把背光开到 0xFF。
- 亮度：Advanced 里显示第 13 档，用户说这远低于实际最大亮度（此前 CE 用过 0xFF）。
  档位表加了 14..20 七档到 0xFF（`classpad_bkl.h`），1..13 仍是 Casio 的值。

**唤醒源：CMT 的比较匹配标志（2026-11-10，待真机确认）。** 勾 Probe 后的快照，
第一次和最后一次相同：
`08 01 00 00 4800 0000 0001 0000 8005 0000`，即 RCR1 08、RCR2 01、INTREQ00 00、
INTMSK00 00、KIUINTREG 4800、KIUDATA0 0000、KIUSTATEREG 0001、CMSTR 0000、
**CMCSR 8005**、RWTCSR 0000。R-standby 的唤醒源是 NMI、IRQ、CMT、KEYSC、RTC
（docs/06 第 2 节）：IRQ 没有请求，KEYSC 没有事件且扫描空闲，RTC 的 AF / CF / PEF 都是
0，看门狗没开；只有 CMT 的 CMF（bit 15）置位。来源：Casio 的延时函数 0x80070020 →
0x800412F0 是 CMT 单次计时（开 MSTPCR0 bit 14 的时钟、CMCSR = 5、CMCOR、启动、轮询
CMF、停止，原先时钟是关的就再关上），结束后 CMF 留着不清。CE 从不碰 CMT，所以 Casio
最后一次延时留下的 CMF 永远置位。Casio 自己进待机前也刚调过延时，CMF 同样置位，它不
被唤醒应当是因为那时 CMT 的模块时钟是关的；CE 下这个时钟门的状态没有读过。修正放在
翻译器（`rt.c` `dbt_standby`，只换 hh3）：睡前开 CMT 时钟、CMSTR = 0、清 CMF 和 OVF、
关 CMT 时钟（MSTPCR0 bit 14），唤醒后把时钟门恢复原状。casio-emu 里休眠唤醒照常
（124 s、Standby x1、stray wakes 0）。CMF 在 CMR = 00（不请求中断）时是否真的能结束
R-standby，没有手册依据，只有"它是唯一异常的唤醒源"这一条；要看真机上 stray wakes
是否变成 0。
INTMSK00 读出 00：启动器接管时写过 0xFF，要么这个寄存器读回不是屏蔽状态，要么
R-standby 之后被复位；INTREQ00 是 0，所以这次不是 IRQ 引脚，先不处理。

用户同时报告（诊断版 hh3）：勾 Touch 能睡能醒，但醒后触摸不能用；勾 Panel 醒后屏幕
不亮。两项默认都是关的，原因未查：触摸控制器上电后只发 0x02 不够（Casio 的触摸任务
启动 0x8008AB3E 还做了别的），面板冷启动的重放在真机上没有把面板带起来。

**不是 CMT；改查 KEYSC 的中断使能（2026-11-10，待真机确认）。** 带 CMT 清标志的
hh3 在真机上：stray wakes 仍不为 0，灰色条纹照旧；Probe 快照
`08 01 00 00 4800 0000 0001 0000 0005 0000`：CMCSR 已是 0005（CMF 清了），所以 CMT 的
标志不是唤醒源（清标志和停时钟的代码留着，无害）。
剩下与 Casio 有据可查的差别在 KEYSC：fx-CP400 自己的 KEYSC 初始化 0x80070F52 是
KIUCNTREG &= 0x7FFF、KOUT = 0x0FFF、KIN = 0x00FF、等 10、KIUCNTREG |= 0x8000、
KIUINTREG = 0x00FF 再 **0x0800**、KIAUTOFIX = 0x8042、KIUINTERVAL = 0x00C8、
KIUWSET = 0、KIUMODE = 0x0200、0xA40501C6 = 0xFF。我们的 `KEYSC_INT_ENABLE` 是
0x4800，来自 fx-CG100 的 OS；多出的 0x4000 按 `classpad_keysc.h` 自己的说明是"扫描完成
也发中断"。待机时控制器自主扫描（间隔 0xC8），每扫完一遍就结束一次 R-standby，且不留
按键事件标志，与现象一致。唤醒后 Casio 的检查 0x80070EB8 是 KIUMODE 0 → 0x0200、
KIUINTREG |= 0x7F（写 1 清标志）。
修正：翻译器 `dbt_standby` 睡前写 KIUINTREG = 0x00FF、0x0800（只换 hh3 就能验证）；
OAL 的 `RStandbyOnce` 也改用 `KEYSC_INT_STANDBY`（0x0800），下次重建生效，已写到 VM。
运行时键盘驱动仍用 0x4800。按用户要求没有等模拟器预检就发出了 hh3。

**也不是 KEYSC 的 0x4000 使能位；改试模式寄存器的复位（2026-11-10，待真机确认）。**
写 0x0800 的 hh3 在真机上：`Standby x51, stray wakes 50`，快照
`08 01 00 00 0800 0000 0001 0000 0005 0000`（KIUINTREG 已是 0800、无标志）。休眠后仍可
按 Clear 唤醒，所以 Clear 的唤醒不靠 0x4000 那一位。用户问诊断版是不是根本没真睡：每圈
内核确实断电，条纹在唤醒之后才画；但每圈醒着约两秒、背光最亮，所以整体几乎一直醒着。
第一次唤醒发生时诊断代码还没运行，诊断本身不是唤醒的原因。
下一个有据可查的差别：Casio 每次进待机前（0x80070E78）写 KIUMODEREG = 0、等 1 ms、
再写 0x0200；每次唤醒后（0x80070EB8）KIUMODEREG = 0、0x0200、KIUINTREG |= 0x7F。
OAL 一直只写 0x0200，从不先写 0。如果待机扫描的按键唤醒请求是锁存的、只有退出该模式
才清，那么 CE 启动后按过的任何键（包括发起休眠的 Shift + Clear）都会让每次待机一进就
醒。改动：翻译器 `dbt_standby` 睡前写 0、等 R64CNT 两个边沿、写 0x0200、
KIUINTREG |= 0x7F；OAL `RStandbyOnce` 也在写 0x0200 前先写 0 并等 2 ms（已写到 VM）。
按用户要求未预检直接发出。目前已排除的唤醒源：RTC 闹钟、CMT 的 CMF、KEYSC 的 0x4000
使能位、IRQ 引脚请求（INTREQ00 = 0）、看门狗。

SpreadCE（casio-emu，心跳采样每 2M 指令一个，`-DDBT_TRACE_BLOCKS` 把宿主 PC 映射回
客户机块）：启动约 2.8 G 指令，敲三个数加回车（6 个键）约 1 G 指令且全程不空闲，
点四个单元格的 41 秒里大半空闲。敲数阶段：间接调用/返回查表（`_dbt_lki_ts`）
约 20%，翻译新块约 17%（不带跟踪的那次 16-23%），LCD 推送（`lcd_pixels`）3-9%
（带跟踪 17%），SpreadCE 自己的代码约 10%。启动阶段翻译新块 20-25%，LCD 推送
1-5%。到启动结束代码缓存已清空 29 次。结论：绘制不是主要瓶颈；主要是翻译器开销
（1.5 MB 的 SH3 代码反复被淘汰重译，以及大量经寄存器的调用）。脏区只按行记一个
区间，上下两处同时变（公式栏和状态栏）就整屏推送，是次要项。模拟器的系统时钟
现在快四倍，推送线程的频率相对指令数也高了四倍，所以 LCD 一项在模拟器里偏高。

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

### Wake-source search built into the translator (2026-11-10)

The KEYSC mode cycle did not stop the stray wake-ups either (third single
guess to fail on the calculator). Note that the OAL masks every INTC source
(IMR0..12 = 0xFF) before the standby, so no on-chip peripheral interrupt
should be able to end it; what the INTC masks do not cover are the IRQ pins
(INTMSK00, which the OAL's snapshot reads as 00) and, by design, the KEYSC
standby scan. `dbt_standby()` now steps through these within one suspend:
each standby that ends in under 7 s adds a measure to the next entry
(1: INTMSK00 = 0xFF; 2: KEYSC programmed as the OS does; 3 and 4: KEYSC
silenced), with an RTC alarm 20 s (10 s) ahead as the safety wake-up. The
verdict is written to CMCSR bits 2:0, which the Probe snapshot shows:
0 none, 1 / 2 the step that held, 4 held with KEYSC silenced, 7 ended at
once even then. Result on the calculator: pending.

Result of the first search run on the calculator: verdict 7. With the IRQ
pins masked and KEYSC silenced through its registers (no rows, no columns,
no enables, mode 0) the standby still ended within 7 s; Clear kept waking
the machine. The search now stops KEYSC's module clock as well in its probe
step, the resume stub records the RTC time, NMIFCR, INTREQ00, RCR1,
KIUINTREG and STBCR first thing after the wake-up (save block EV0..EV2), and
the STANDBY_DIAG build prints them with the time the core was off (1/128 s)
over stripe 2 for 3 s.

### The OS's standby is a loop that never leaves ROM (OS 02.01.7003 0x80020ED2)

Read while looking for what the OS does differently around SLEEP:

    80020ed2  [fd800000] = 0, [fd800010] = 0
    80020ede  SR = 0x70000000, STBCR = 0x21, jsr 0x80085E74   ; BAR = 0x80085EC2, registers to RS memory, SLEEP
              (wake-up lands at 0x80085EC2 in ROM: SR.RB cleared, STBCR &= ~0x20,
               BAR = 0xA0000000, RWTCSR = 0xA507, registers back, rts)
    80020ee6  jsr 0x80070EB8   ; KEYSC: if KIUSTATEREG bit 0 is set -> 0 (only KIUINTREG |= 0x7F);
                               ; else read KIUDATA0..5, r0 = Clear (word 0 bit 1), mode 0 -> 0x0200
    80020ef2  jsr 0x8006F886   ; INTREQ00 bit 6 clear -> 1, bit 5 set -> 2
    80020efc  if ((keysc ? 4 : 0) | pins) & 7 == 0 -> back to 80020ed2: SLEEP again

So the OS itself expects wake-ups that are nobody's business and answers
them from ROM with its stack in RS memory: the SDRAM stays in self-refresh,
no cache, no pins, a few dozen instructions, SLEEP. The memory resume
(0x800209C4) only runs once the loop has a reason. Our snapshot at the stray
wake-ups reads KIUSTATEREG = 0001, which is exactly the OS's "nothing, sleep
again" case. Our path per wake-up is the opposite: SDRAM out of self-refresh
(with a wait of more than two R64CNT steps), caches, MMU, the translator, the
OAL's loop, and all the way down again. Whatever the rate of these wake-ups
turns out to be (the dt figure of the diagnostic build), the answer to them
has to move into the resume stub, as the OS has it.

Also read on the same pass (2026-11-10), all without effect on the hardware:
0x80020414 switches the stack to RS memory (0xFD802000), 0x80072856 stores
two state words, 0x80020BE0 only saves the INTC (IPRA..IPRL, IMR0..IMR12)
and the BSC registers to 0xFD80001C. So the OS goes to sleep with its
ordinary interrupt masks: KEYSC stays unmasked at the INTC, the RTC is
taken out by its priority (IPRK &= 0x0FFF). The OAL does the opposite: every
IMR is 0xFF (the alarm's bit open if an alarm is armed), KEYSC masked. If a
masked KEYSC cannot end R-standby, the Clear wake-up seen so far is only the
OAL's loop reading the key between stray wake-ups; to be checked once a
standby holds.

From the stripe timing of the earlier runs (13 entries in 27 s with 8
stripes of 0.25 s each) the core is off for well under 0.1 s per entry: the
wake condition is already there when SLEEP executes.

Search steps from this build on: 1 IRQ pins masked, 2 module clocks as the
OS had them at the launch (CE's drivers start I2C, A/D and at times USB),
3 and 4 KEYSC silenced with its clock stopped, 5 KEYSC as the OS programs
it. Emulator: the stub's evidence capture and the on-screen text work
(suspend 51 s, dt 0x1984, KIUINTREG 0x080F at the wake-up, resumed by Clear).

### The standby loop in the resume stub (2026-11-10)

`dbt/rt/standby.S`: the resume stub, entered at BAR in RS memory, now does
what the OS's loop does before it brings anything of the machine back. It
records the wake evidence, then goes on with the resume only for the RTC
alarm flag (RCR1.AF), Clear down (KIUDATA0 bit 1) or a KEYSC event flag
(KIUINTREG low byte), the OAL's own three reasons. Otherwise it counts the
wake-up (save block + 0x80; EV0 and EV1 of the first one at + 0x88),
treats KEYSC as 0x80070EB8 does (mode 0 and 0x0200 again unless
KIUSTATEREG bit 0 is set, KIUINTREG |= 0x7F), writes RWTCSR = 0xA507,
BAR, STBCR = 0x21, SR = 0x70000000 and executes SLEEP. The SDRAM stays in
self-refresh, no cache, no pins. `dbt_standby()` switches the loop on
(save block + 0x84), opens KEYSC at the INTC for the standby (IMCR5 bit 7,
a priority in IPRF if there was none) as the OS has it, and puts both back
after the wake-up. `make STANDBY_SEARCH=1` builds without the loop and with
the stepping wake-source search instead; it is off otherwise. The
STANDBY_DIAG build prints `rs <count> sec <seconds>` and the first stray
wake-up's evidence at the real wake-up.

Emulator (ce5v image, diagnostic build): idle suspend, another key 25 s
later does not wake, Clear 50 s later does ("resumed by Clear/ON"), rs 0,
sec 0x33. The emulator never produces a stray wake-up, so the SLEEP-again
branch itself has only been read in the disassembly, not run. Calculator:
pending. The loop does not remove the source of the stray wake-ups; if they
arrive back to back the core still cycles, only without the SDRAM, caches
and CE in between. rs / sec from the calculator says how often.

Calculator, first run with the loop: `rs 00000001 sec 0019`. One stray
wake-up in 25 s: they do not come back to back. Each full entry is followed
by one, and after the stub's SLEEP the standby holds. This was the first
time the machine really stayed off for seconds, and it showed a second
fault: after the wake-up it sometimes reset into the OS, sometimes came up
frozen with touch and keys dead. Both look like RAM that did not keep its
contents.

What the way down did differently from the OS around the memory standby
(0x8002095E: RTCSR, SDCR = 0, PLLCR bit 1, PULCRBSC |= 0x12, PH6 released):
the OS is out of the SDRAM before it (stack in RS memory since 0x80020414,
code in ROM) and spends a long time between it and SLEEP (INTC save, pins,
the panel's power-off with its waits); `standby.S` kept fetching from the
SDRAM and executed SLEEP a few instructions after the PHCR write. Changed:
the memory standby and the SLEEP now run from RS memory (a tail at the end
of the stub's blob), with a wait of more than 4 R64CNT steps in between;
on the way up the pins come back in the OS's order (PULCRBSC, PHDR bit 6
low, PHCR). The STANDBY_DIAG build sums the guest's 8 MiB before and after
and prints `mem <before> <after> OK|BAD`. Emulator: suspend 50 s, Clear,
"mem D2495524 D2495524 OK", resumed. Calculator: pending; whether the wait
is what the SDRAM needed is a guess until the mem line says OK there.

### The OS's standby does not keep the RAM; self-refresh instead (2026-11-10)

Calculator with the wait before SLEEP: the wake-up painted the stripes up
to cyan (SDRAM controller set up, about to jump into the translator) and
reset. The translator's code in the SDRAM was gone. The wait was the wrong
idea; reading on showed why.

After its standby loop the OS does not return to its caller. 0x80020DF0
ends in 0x800204A8, which runs the memory resume 0x800209C4, switches to a
fresh stack (0x8C2361D4) and enters the main loop 0x8006FA54 for ever: a
warm restart. And 0x800209C4 is a cold SDRAM initialisation: PH6 driven,
a settling wait, RTCSR, SDCR = 0xA09, RTCNT/RTCOR, a mode register set
(0xFEC15040), RTCSR. Its counterpart on the way down (0x8002095E:
RTCSR = 0xA55A0000, SDCR = 0, PLLCR bit 1 = CKO off, bus pulls, PH6
released) stops the refresh and the clock. Nothing in it puts the SDRAM into
self-refresh. The OS's power-off does not need the RAM's contents; what CE
needs, it never did.

That `standby.S` copied this "instruction for instruction" went unnoticed
because every standby ended within 0.1 s (the stray wake-up), and DRAM
cells hold that long, and mostly for tens of seconds, without refresh.

The OS does have self-refresh, for its clock changes (0x80020F60,
0x80020FC4): 0x80020F1C clears SDCR bit 9 (power-down) and sets bit 10
(RMODE), 0x80020F3C does the reverse. The classic SH BSC layout (SDCR bit
11 RFSH, 10 RMODE, 9 PDOWN). `standby.S` now does that from RS memory before
SLEEP, writes it again in the standby loop, and on the way up leaves SDCR
alone until the controller is set up (the SDCR = 0xA09 write ends the
self-refresh). The OS's other three steps are optional (save block + 0x94:
bit 0 CKO off, bit 1 bus pulls, bit 2 PH6): whether the SDRAM survives them
is not known, so the STANDBY_DIAG build adds one per suspend (v 0, 1, 3, 7)
and other builds stay at 0. EV2 is now SDCR and PLLCR as the stub finds
them at the wake-up (is the BSC kept across R-standby?).

Emulator: suspend 55 s, Clear, "v 0 ... mem OK", resumed. Calculator:
pending. Open: whether CKE stays low while the core is off; if the mem line
says BAD or the wake-up resets with v 0, it does not, and R-standby cannot
keep CE's RAM on this board at all.
