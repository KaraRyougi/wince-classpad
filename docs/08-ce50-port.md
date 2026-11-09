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

## 七、还没做的

- 在 VM 里构建（第一次构建会暴露 SDK 的差异：头文件、`WARNISERROR`）。
- `tools/windows/claude-build` 的 CE 5.0 版本（现在是 C:\WINCE600 的路径和
  kern.dll 重链接逻辑）。
- 真机：R-standby、面板初始化黑屏问题与 CE 版本无关，仍待验证。
- CE 5.0 下的性能测量（My Device 打开时间），以及 shell-performance 分支
  的翻译器优化对 CE 5.0 的适配。
