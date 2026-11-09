# CE 6.0 在 ClassPad 上踩过的坑：CE 4.2 移植清单

整理于 2026-11-07。结论：CE 6.0 R3 不适合 fx-CP400 现有硬件，改为移植
Windows CE .NET 4.2。**2026-11-08 更新：实际选定 Windows CE 5.0**（PB 5.0 已装
好）。5.0 与 4.2 同属一个内核模型，本文的「→ 4.2」对 5.0 同样适用；逐条核对
的结果和 5.0 的 BSP 见 `docs/08-ce50-port.md` 和 `bsp/classpad50/`。本文列出 CE 6.0 期间对系统镜像（内核、OAL、驱动、
`.bib`/`.reg`、随镜像的程序）做过的全部改造，并在每条后用「→ 4.2」标出
换版本时要重新确认的地方。

「→ 4.2」里的判断都是从 6.0 的结论推出来的，**还没有拿 4.2 的源码或
Platform Builder 4.2 核对过**，开工时逐条验证。

## 0. 资料来源

很多工作还没合并到 `main`，引用时写成 `分支:路径`：

| 分支 | 内容 |
|---|---|
| `main` | 翻译器、OAL、显示/键盘/触摸驱动、内核补丁、XIP、eActivity 窗口（docs/03-05、`bsp/`、`notes/ce/kernel-sh-notes.md`） |
| `claude/ce-5-memory-pressure-7462c3` | 集成分支：DocList.dll、旧 MFC 运行库、EXT 区、R-standby、FSRAMPERCENT 3.1% |
| `claude/xpander-shell` | Math Xpander 进 EXT（XIP）、pe4k 重对齐、重建重定位、SH3CRT.dll、按键映射、8 MiB RAM 布局、裁掉网络栈 |
| `claude/wince-plotter-z-integration-9ea3e0` | 在 xpander-shell 之上加 Plotter-Z；是目前最全的一条线 |
| `claude/shell-performance-issue-6298c3` | 翻译器性能：CE6 页表遍历回填 UTLB、KDEV、按 ASID 分哈希等 |
| `claude/xpander-help-button-crash-e7096c` | 工作区里**未提交**的 PegHelp.exe（`bsp/classpad/SRC/APPS/PEGHELP`） |

## 1. 与 CE 版本无关的硬约束（4.2 照样存在）

- SH7305 固定为大端，CE 只有小端：靠同 ISA 二进制翻译器（`dbt/`）运行，
  所有 16/32 位访存都要字节交换，OAL 和驱动的 MMIO 必须预交换。
- 没有 FPU：见第 3 节。
- 16 MiB SDRAM 由 CE 和翻译器分用：目前 CE 8 MiB、翻译器 8 MiB。
- 闪存只有两块能放镜像：Casio OS 镜像里的空洞 0x80EE0000（5 MiB，
  `main` 上写作 0x81000000 起 3.875 MiB），以及 eActivity 区 0x83860000
  （7.625 MiB，每次回到 Casio OS 都会被格式化）。
- 翻译器的超级调用约定（F2 代码变更、F9 TLB 变更、F3 调试输出、LCD push、
  `HC_STANDBY`）与 CE 版本无关，OAL 仍要按它来写。

## 2. 内核源码

1. **WaitOneMore 中断事件 proxy 泄漏**（`kcalls.c`，QFE 2013-03）。
   带超时的 `WaitForSingleObject` 去等中断事件时，如果超时恰好落在两次
   KCall 之间，`exitfunc` 不会把 proxy 摘掉，之后每次等待都返回
   `ERROR_INVALID_HANDLE`。翻译器把首次翻译的耗时算进 guest 时间，所以
   每次运行都会触发。修法是 `bsp/kernel/kcalls-waitonemore.patch`，
   驱动也不再对中断事件做超时等待。
   → 4.2：查 4.2 的 `kcalls.c`/`schedule.c` 有没有同样的路径；不管有没有，
   驱动的这条规则都保留。
2. **loader 拒绝 SH3 PE**（`loader.c`，`#ifdef` 链里没有 SHx 分支）。
   `bsp/kernel/loader-sh3-images.patch` 让它接受 0x1A2/0x1A3。
   踩过的坑：只改 `loader.c` 不够，必须重新编 `nkmain.lib`
   （`build -c`）并重新链接 kern.dll（Sysgen）。2026-11-06 的 IDE 构建
   就因为 `nkmain.lib` 比补丁旧而加载不了 mfcce300。
   → 4.2：4.2 的 SH4 loader 是否同样检查 CPU 类型要看源码；ROM 里的
   MODULES 用 `P` 标志可以跳过检查（第 5 节）。
3. **内核直接访问 CCN 的 P4 寄存器**（EXPEVT/INTEVT/TEA/TRA、
   PTEH/PTEL/PTEA/TTB/MMUCR、CCR）。走交换路径后值整个反过来：API 调用
   认不出、ASID 写不进硬件。没有改内核，而是由翻译器对 kernel.dll 做
   设备感知翻译（高字节 0xFF 不交换）。范围要取 kernel.dll 的代码节，
   不能用 load..load+vsize（Release 版 text 在加载地址之下）。后来
   KDEV 用特征码在 kernel.dll 里定位 25 个可能访问 P4 的函数，只给它们
   保留运行时检查（`tools/kernel_devfuncs.py`、`rt/kdev.c`）。
   → 4.2：4.2 的内核和 OAL 一起链接进 nk.exe，范围要换成 nk.exe，KDEV
   特征码要重做。OAL 对 0xFFxxxxxx 的访问（例如读 INTEVT）在这个范围内
   不能再用预交换的访问函数，否则会交换两次。
4. **bank 1 里一直存着内核状态**（r4 = pCurThread，r5 = 上下文指针，
   r7 = 0xFF000000）。翻译器的异常桩必须保存 bank 1。
   → 4.2：对照 4.2 的 `shexcept.src` 重新核对。
5. **OEMInit 在 BL=1、bank 1 下运行**：这时 `trapa` 会触发手动复位，调试
   输出只能写 RAM 环形缓冲区，由翻译器取走打印。
6. **API 调用约定**：6.0 是跳到 ≥ 0xFFFEFE03 的奇地址，触发地址错误
   （EXPEVT 0x0E0）。
   → 4.2：陷阱区间不同（CE 5 之前在 0xF000xxxx 一带），按 4.2 的
   `nkshx.h` 确认。翻译器的“不可取指 pc → 合成地址错误”机制是通用的。
7. **TLB 回填走捷径**（`shell-performance` 分支 `TLB_REFILL=1`）：翻译器
   照抄 CE6 `TLBMissHandler` 的页表遍历直接回填 UTLB，前提是 VBR+0x400
   处的代码与 CE6 逐字相同。
   → 4.2：4.2 的页表结构和 slot 模型不同，这条要重写或关掉。

## 3. FPU

1. **硬件没有 FPU，CE 预编译的 SH4 二进制里全是 FPU 指令。**翻译器把每条
   FPU 指令翻成对 `fpuemu`（纯整数 IEEE 754、SH-4 语义）的 helper 调用，
   每条 50-100 周期，陷阱方式要 500-800。FIPR/FTRV/FSCA/FSRRA 没有实现
   （编译器不主动生成）。解码器在进入 FPU 子解码前先按操作码分组拦截
   （FPU gate），否则拖慢所有指令。
2. **惰性 FPU 上下文切换依赖 SR.FD。**线程首次用 FPU 时内核靠
   EXPEVT 0x800/0x820 走 `SwitchFPUOwner`。`fpuemu` 原本不看 FD，所有
   线程会共用一套寄存器，所以翻译器模拟了 FD（`fpudis` 测试）。
   默认 FPSCR 是 0x40000（DN=1）。
3. **BSP 代码禁止用 float/double**：OAL 里 FPU 是关的；驱动也遵守 C89、
   无浮点。
4. **SH3 程序的浮点调用约定和 SH4 coredll 冲突。**Pocket PC 2000 的 SH3
   程序调用 coredll 的 `_adds/_addd/_dtoi/_stod/...` 以及 `sin/pow/atof`
   等带 double 的 C 函数。CE6 的 SH4 coredll 导出了同名函数，但用的是 SH4
   FPU 约定（fr4/dr4 传参、fr0/dr0 返回），而 SH3 调用方把 float 放在
   r4/r5、double 按地址传。结果 Math Xpander 的数学键盘算每次点按落在哪个
   键时都得到“没点中”，算出的数字也不可信。
   修法：`SRC/APPS/SH3CRT`（SH3CRT.dll）用 SH3 约定重新导出这些名字（只用
   整数和指针参数，所以 SH4 编译器的整数约定与 SH3 一致）；
   `tools/sh3imports.py` 给每个 SH3 模块加一个导入 SH3CRT.dll 的描述符，
   把对应的 COREDLL IAT 引用改指过去（`stage.py` 自动执行）。
   **还没覆盖**：`printf` 系列的 `%f`（可变参数 double），以及
   `sh3crt.def` 之外任何收发 float/double 的 coredll 函数。
5. **自己写的程序尽量避开浮点**：Plotter-Z 用上游的 `USE_FIXED_POINT`，
   逐顶点投影不走 FPU。

→ 4.2：

- 4.2 的 SH4 映像同样满是 FPU 指令，fpuemu 和 FD 模拟照用。
- 4.2 的 SH4 coredll 是不是也用 FPU 约定导出 `_adds` 等，要检查；如果是，
  SH3CRT 照搬。
- **值得评估的另一条路**：PB 4.2 还支持 **SH3** 目标。整个 OS 按 SH3 编
  就没有 FPU 指令（软件浮点），Pocket PC 2000 的 SH3 程序原生兼容，也不需要
  SH3CRT 和 loader 补丁。
  代价不是“模拟一套 MMU”。SH4 这条线上翻译器也没有模拟 MMU：内核对
  PTEH/PTEL/MMUCR/EXPEVT/TEA 的访问在设备感知路径上不交换、直接打到真实
  硬件，`ldtlb` 真的执行，TLB miss 是硬件异常，翻译器只是旁观（ldtlb
  钩子、PTEH 跟踪），合成异常时也是直接写真实的 EXPEVT/TEA。
  SH3 内核的区别在于寄存器布局不同，所以要在同一条设备感知路径上加一层
  **重映射**：
  - 寄存器地址：SH3 的 TRA/EXPEVT/INTEVT/MMUCR/PTEH/PTEL/TTB/TEA/CCR 在
    0xFFFFFFD0-0xFFFFFFFC，SH-4A 在 0xFF0000xx；
  - 字段含义：MMUCR（SH3 的 RC/TF 对应 SH-4A 的 URC/TI）、CCR 的位不同；
    PTEL 基本兼容（SZ=0 在两边都是 1 KiB 页，SH-4A 也支持）；
  - 地址冲突：SH3 内核用的 0xF0/0xF1（缓存阵列）、0xF2/0xF3（4 路组相联
    TLB 阵列）在 SH-4A 上分别是指令缓存和 **ITLB** 阵列，按条目刷 TLB 的
    写入必须拦截，改成 SH-4A 的 UTLB 关联写（0xF6000080）或整体失效；
  - 翻译器自己合成异常时，要写到内核读取的那个 SH3 地址（即影子寄存器），
    而不是 0xFF000024。
  是一个有限的重映射层，量级大致与当初做设备感知翻译相当，不是完整的
  MMU 模拟。前提是先读 4.2 SH3 内核源码，列全它碰的 P4 地址。

## 4. 镜像布局、config.bib、构建

- **HARP 的布局装不下**：它从 0x8C001000 开始，RAM 到 0x90000000；所有
  记录都必须落在翻译器给 guest 的窗口里。
- **两个 XIP 区**（`xpander-shell` 起）：NK 在 eActivity 区 0x83860000
  （7.625 MiB），EXT 在空洞 0x80EE0000（5 MiB），makeimg 分别输出
  NK.bin 和 EXT.bin，`wince.hhk`/`nkflash.hhk` 按窗口表烧写。romimage
  踩过的坑：
  - `AUTOSIZE=ON` 且 NK 写在前面时，romimage 把 RAM 挪进了 EXT（“Ram start
    overlaps rom binary”）。区要按地址顺序写，并设 `AUTOSIZE=OFF`。
  - 多区共用默认 DLL 范围会报 “intersecting DLL ranges”。要按区写
    `DLLLOWADDR <区> <地址>`、`KDLLLOWADDR ...`；不开 `DLLADDR_AUTOSIZE`
    时不能用全局的 `=` 写法。
  - 翻译器整体刷新代码缓存时，要把 EXT 的块和 NK 的一样保留下来
    （`rom2_lo/hi`）。
- **eActivity 区会被 Casio OS 格式化**：Fugue 文件系统按日志结构回收扇区，
  头部有效也照样会被擦。目前的办法是每次进 CE 前从 `\fls0\NK.bin` 重烧；
  要彻底解决只能给 Casio OS 打补丁。空洞区在 Casio OS 校验和覆盖范围内，
  但 02.01.7003 开机时没人检查，只在工厂测试菜单里显示 NG。
- **RAM**：CE 8 MiB（0x8C000000 起，启动器的第一个 MiB 也交给 CE），
  最后一页是调试环形缓冲区，第一页是 BOOTARGS。另有一个实验布局
  （`ram-layout`）：把翻译器桩放进 IL memory、热数据放进 XRAM/YRAM，
  CE 可得 11.3 MiB；R-standby 后要恢复这两块片上内存（SYNCO + ICBI）。
- **对象存储**：`FSRAMPERCENT` 从默认 50% 一路降到 12.5% 再到 3.1%
  （`08080808`）。默认值会让内核因内存不足拒绝显示驱动的数据拷贝，GWES
  起来时没有驱动。
- **`COMPRESSION=OFF`**：内核解压器在翻译器下太慢。
- **构建选项**：eboot、KITL、内核调试器全关，但不能开 ship build（会把
  RETAILMSG 编译掉）；Release 内核一个字符串都不打印，OAL 自己打
  “POSTINIT”和“first idle”两个标记。
- **按 catalog 拉进来的多余组件**：Standard Shell 在 catalog 里依赖网络栈，
  但 PE 导入扫描显示没人用。用 `FILES/PostFmergeBib.bat` +
  `romexclude.txt` 在 fmerge 之后从 ce.bib 删掉网络栈（约 740 KB）和
  18 个 .wav（没有音频驱动）。
- **IE 太大**：CE6 的 PegHelp 就是 IE 的 htmlhelp.exe，要带 mshtml/
  shdocvw/urlmon/wininet/crypto，加上后镜像约 14 MiB，所以自己写了
  PegHelp（第 5 节）。
- 有 shell 的完整镜像 6.95 MB，`main` 上的 3.875 MiB 空洞放不下，这正是
  后来改用 eActivity 区的原因。

→ 4.2：4.2 的镜像小得多，是换版本的主要收益。目标是 NK 带 shell 和 MFC
放进空洞，Xpander 放进 EXT 或 eActivity 区，彻底摆脱重烧。romimage 4.2
的多区语法（`DLLLOWADDR` 按区写）要重新确认。

## 5. Pocket PC 程序：Math Xpander、MFC、DocList、帮助

Math Xpander 1.0（Saltire，2001）取自 `Xpander.PPC300_10003.cab`：24 个
SH3 PE（CE 3.0，subsystem 9），依赖调查在 `notes/ce/math-xpander.txt`。

1. **SH3 PE 能加载**：loader 补丁（第 2 节）；ROM 里的 MODULES 用 `P` 标志
   （不检查 CPU 类型）。
2. **romimage 只认 4 KiB 对齐的 XIP 模块**，而 Pocket PC 工具链按 1 KiB
   对齐节。`tools/pe4k.py` 把节移到 4 KiB 边界，同时修补 RVA、每个
   HIGHLOW 重定位和 .reloc 表，再把原件和副本重定位到同一基址逐节比对。
3. **MathXpander.exe 用 /FIXED 链接、没有 .reloc**，既不能作为 XIP 模块
   重排，作为普通文件 CE6 也拒绝（“not a valid Windows CE application”：
   `ReadO32Info` 不接受没有页对齐的节）。`pe4k.reconstruct_relocs` 找回
   222 个 HIGHLOW 重定位（157 个字面量池、43 个虚表/消息映射指针、
   2 个 .data、20 个 .pdata），手工核对后用 SHA-256 锁定，再重对齐。
4. **mfcce300.dll（MFC CE 3.0，SH3）**：CE6 和 cab 里都没有（Pocket PC 2000
   放在 ROM 里），取自 eMbedded Visual Tools 3.0 CD2 的
   `POCKTSDK/.../MFC/LIB/SH3`（`FILES/legacy-mfc`，SHA256SUMS）。8 个
   Xpander 模块依赖它（460 个符号）。它导入的 coredll、commctrl 序号和
   AYGSHELL 序号 4/21/34/40/56/69/83/84 在 CE6 里都有；只缺
   `DocList.dll!DocList_Create`。现在只放 mfcce300 一个（olece300、
   mfcce211、olece211 留在仓库里不用），作为 NK 里的 XIP 模块。
5. **DocList.dll**：CE6 根本没有（AYGShell API Set 只加 aygshell.dll），
   CD 上的 doclist.dll 是 x86 模拟器版，SH3 的 DOCLIST.LIB 只是导入库。
   于是自己写了 `SRC/APPS/DOCLIST`：
   - 一个铺满父窗口客户区的子窗口：顶部是过滤组合框，下面是报表式列表
     （Name/Folder/Date/Size），列出 `\My Documents` 及其一级子目录；
   - 点按或回车发 `DLN_ITEMACTIVATED`（带完整路径），支持 SDK 的 `DLM_*`
     消息；
   - 按 Pocket PC 的序号 50-52 导出 `DocList_Create` 和两个 wave 辅助函数；
   - MFC 放进来的命令栏所占的顶部区域留空；
   - 红外、邮件、重命名/移动、右键菜单、文件变更通知都没有实现，返回
     “未处理”。
   Math Xpander 只用 `CSingleDocTemplate`，实际不会打开 DocList 窗口，
   只是加载时需要这个 DLL。
6. **浮点约定**：SH3CRT.dll（第 3 节第 4 条）。
7. **帮助**：Workbook.dll 和 Graphics.dll 的帮助命令只是
   `CreateProcess(L"PegHelp.exe", L"file:math1.htm#...")` 并忽略结果，
   `\Windows` 里没有 PegHelp.exe 就什么都不发生。CE6 的 PegHelp 依赖整个
   IE（第 4 节），所以自己写了 `SRC/APPS/PEGHELP`（只依赖 coredll，
   用 GDI 渲染帮助文件用到的 HTML 子集，`test_peghelp` 检查 217 个链接
   都能解析）。**这部分在 `xpander-help-button-crash` 工作区里还没提交。**
8. **输入法**：cab 原本在安装时运行 `regsvrce.exe` 注册 MathKeyPad.dll，
   改为在 `platform.reg` 里写入 DllRegisterServer 会写的键；SIP 用
   softkb.dll，fx-CP400 的 Keyboard 键用 `classpad_rotatekey.exe` 切换
   MathKeyPad 显示/隐藏。
9. **按键**：MathKeyPad 发的是纯 ASCII，所以键盘布局把 `= . , - /` 直接
   映射，`+ * ( ) ^` 用伪 VK 展开为 Shift + `= 8 9 0 6`（之前 `( )` 打出的是
   `[ ]`）。底部 7 个触摸图标仿 HP Xpander（这台机器是为它设计的）：
   Menu=HOME（开始菜单）、Main=MATH（前置或启动 Math Xpander）、
   Settings=MENU（点开前台程序的菜单栏）、Esc=ESC；Rotate 旋转屏幕。
10. **注册表/开始菜单**：`.mwb` 文件类型、开始菜单快捷方式
    （`platform.dat`）；快捷方式改指 `\Windows\MathXpander.exe`。
11. 界面按 240x320 设计，在 320x528 里能放下。

→ 4.2：

- 4.2 的内存模型（32 个 slot）与 Pocket PC 2000 的 CE 3.0 相同，SH3 程序
  的兼容性预计更好。
- PB 4.2 的 catalog 里有没有 AYGShell、带 MFC 的组件要查；mfcce300 和
  DocList 替身大概率仍然需要。
- pe4k 重对齐、重建重定位这两步取决于 4.2 的页大小，要重新确认。
  1 KiB 对齐来自 CE 3.0 SH3 的页大小，不是工具链的随意设置：同一张
  eVT 3.0 光盘上，SH3 版 mfcce300/olece300 的 SectionAlignment 是
  0x400，SH4 版 mfcce211/olece211 是 0x1000。所以 SH4 版 4.2（页大小
  4 KiB）大概率仍要重对齐；SH3 版 4.2 要看它的 `PAGE_SIZE` 还是不是
  1 KiB（查 PB 4.2 的 `nkshx.h`，再用 romimage 实际放一个 1 KiB 对齐的
  模块）。
- 如果走第 3 节的 SH3 OS 路线，SH3CRT 和 loader 补丁都不需要了。

## 6. Plotter-Z（`claude/wince-plotter-z-integration-9ea3e0`）

anderain 的 3D 函数绘图器，用它的 win32-native 前端，上游文件原样放在
`SRC/APPS/PLOTTERZ/upstream`（提交号见该目录 README），CE 适配代码放在
外层：

- `plotterz.c` 把所有源文件作为一个编译单元，并在上游 `WinMain` 之前调用
  `InitCommonControlsEx`：CE 1.0/2.x 的 shell 会替每个进程做这件事，CE6
  不会，否则 File > Refresh threshold 的 trackbar 建不出来。
- `afxres.h` 代替 MFC 的头文件（上游 eVC 资源脚本引用它，PB 没有）。
- `sources` 关掉 `WARNISERROR`，链接 coredll、commctrl、commdlg。
- 定点渲染避开 FPU。
- 在 Mac 上用 clang 对着 VM 的 SDK 头文件检查过，88 个外部符号都能在
  该 OS design 的导入库里找到；**还没在 VM 里构建、运行过**。

→ 4.2：这些适配基本照搬；确认 4.2 有 commdlg 和 trackbar。

## 7. OAL / BSP

- **样例 BSP 不能直接用**：SH4 样例针对 SH7750（片上模块在 0xFFDxxxxx），
  SH7305 是 0xA4xxxxxx 的 SH7724 式布局，要重写。
- **所有 16/32 位 MMIO 预交换**（`classpad_mmio.h`），C89、不用浮点。
- **StartUp**：设置 SR/VBR，MMUCR=0，刷新 TLB，不能依赖复位时的 MMU 状态。
- **时钟节拍**：TMU0，INTEVT 0x400。真机上 TMU 比 `bsp_cfg.h` 假设的
  快约 1.67 倍（FRQCR 为 0x18001102，不是模拟器的 fx-CG50 值），常数要以
  真机读数为准。可变节拍（`pfnUpdateReschedTime`，最长 1000 ms 一次）减少
  每次中断都要经过翻译器的往返开销。
- **INTC**：IPRA..L 步长 +4（初版写成 +2n 是 bug）；KEYSC 的 INTEVT
  0xBE0 对应向量 95，超出通用 OAL 64 项的 IRQ 映射表，只能在 OAL 里挂
  向量、映射到逻辑 IRQ 48；RTC 闹钟用 IRQ 49。ISR 里先屏蔽中断源，
  InterruptDone 再解除。
- **OEMCacheRangeFlush**：执行 `ocbwb/ocbp/icbi`，再发 F2（丢弃翻译）和
  F9（TLB 变更）。
- **调试输出**：F3 超级调用或 RAM 环形缓冲区。
- **其余小件**：软件 RTC（`OEMSetAlarmTime` 用硬件闹钟）；OEMIdle 用
  `sleep`；OEMNMI 返回 SYSINTR_NOP；IOCTL 表只保留最小集合。
- **电源**（`docs/06-deep-suspend.md`，ce-5 分支起）：
  - 照搬 Casio OS 的 R-standby 步骤：SDRAM 自刷新要按 ROM 0x8002095e 的
    顺序写 RTCSR、SDCR、PLLCR 等（第一次读错成 SDCR，SDRAM 掉电丢数据）。
  - SPC/SSR 在 R-standby 后会丢，必须先保存。
  - 唤醒后 TMU0 已复位，等待改用 R64CNT 计数，不能再用 OALStall。
  - 唤醒源只有 Clear/ON（KEYSC 自主扫描，以事件标志为准，不能只看电平）
    和 RTC 闹钟；SHIFT 后按 Clear/ON 关机，与 Casio 一致。
  - 背光由 DCS 0x51 控制，不是 SLPIN：关屏前先把 0x51 写 0，等约 60 ms
    再 DISPOFF/SLPIN，否则常白面板会闪白。
  - 模拟器不模拟 SDRAM 刷新，也保留了核心寄存器，这些坑只能在真机上暴露。
- → 4.2：OAL 结构不同。没有 OEMGLOBAL 和 kernel.dll 的拆分；IOCTL 集合
  （例如 POSTINIT）、可变节拍接口、`pfnInitClock` 都要按 4.2 重对。

## 8. 驱动

- **显示**（GPE，`classpad_ddi`）：
  - 面板是 DCS 命令集，不是 R61524：Port R bit 4 区分命令/数据，用
    CASET/PASET/RAMWR 写像素，320x528；首次推送后才发 DISPON。
  - RAM 帧缓冲 + 推送线程，每次超级调用最多推 4 行，让 guest 中间能被
    中断；跨总线切换前后加 SYNCO。
  - 链接上的坑：`GPEEnableDriver` 是 C++ 链接；`RegisterDDHALAPI` 留空；
    必须定义 `dpCurSettings`；`DllMain` 不能是 C 链接。
  - 支持旋转。
  - **真机接管时黑屏还没解决**：PanelInit 无条件发 SLPOUT/COLMOD，可能
    应该继承 Casio 已经配好的面板状态。
- **键盘**：
  - KEYSC 矩阵，扫描码 = 8×行+列，fx-CP400 键表（`Matrix=1`）。
  - 中断 IST 外加 20 ms 轮询线程，不对中断事件做超时等待。
  - Layout Manager 回调的第三个参数是 fKeyUp，要传 `KeyStateDownFlag`。
  - → 4.2：键盘驱动框架不同，MDD 部分按 4.2 样例重写。
- **触摸**：
  - IIC0 上的传感器（地址 0x40，命令 0x84 读 16 字节）；第三个字是 z1，
    不是接触标志，接触看 PRDR bit 5；由轮询线程负责落笔检测和采样，
    不依赖 0x700 中断。
  - 底部 7 个图标由 DDSI 处理成命名事件。
  - 校准数据写在注册表；没有面板的机型绝不碰 IIC0。
- **内核态/用户态驱动**：CE6 的 GWES 驱动要以 `K` 标志或 `k.` 前缀放进
  内核（例如 `k.touch.dll`）。
  → 4.2：4.2 没有这种拆分，驱动分别在 device.exe/gwes.exe 进程里。
- **启动项**：`classpad_hello` 不再开机启动（Launch60 和 servicesStart
  冲突）。

## 9. 移交环境（不在镜像里，但 4.2 一样要处理）

- 接管机器时必须屏蔽 IRQ 引脚，否则真机起不来；翻译器另有防中断风暴的
  保护。
- 面板调试控制台会让真机屏幕变黑，DEVICE=1 构建默认去掉它。
- HollyHock 在 Casio OS 运行时就填好 payload，0x8C800000 在板子上从没
  加载过，所以 payload 先放在 0x8C900000，接管后再搬到 IMAGE_BASE。
- USB、SCIF、DMAC 保持原样。

## 10. 性能：CE6 特有、4.2 可能缓解的部分

- CE6 内核/用户分离、每进程一个 ASID，导致 ASID 切换、TLB 回填、`ldc sr`、
  `rte` 都极其频繁；Shell 的工作集超过 8192 个翻译块。
- `shell-performance` 分支把“打开 My Device”从 26.97 s 压到 10.20 s
  （模型时间，帧逐像素一致），手段包括：二次机会回收代码缓存、KDEV、
  按 ASID 分哈希、照抄 CE6 页表遍历回填 UTLB、不经 C 的常用 helper、
  缓存布局调整（3.25 MiB、19200 个块头、32768 项哈希）。
- 其中 KDEV 特征码和 TLB 回填**绑定 CE6 的二进制**，换 4.2 必须重做；
  其余是通用优化，可以保留。
- 4.2 的 slot 模型和更小的组件集可能减少地址空间切换，但只有实测才能
  下结论。

## 11. 建议的 4.2 起步顺序

1. 先决定 SH4 还是 SH3 OS（第 3 节）：这决定 FPU、SH3CRT、loader 补丁，
   以及翻译器是否要加 SH3 寄存器重映射层。
2. 读 4.2 的 `KERNEL/SH*` 源码，核对第 2 节的每一条（bank 1、API 陷阱区间、
   P4 访问点、WaitOneMore）。
3. 无头内核镜像在 casio-emu 的 ClassPad 配置里跑到 idle（OAL 照搬，接口按
   4.2 改）。
4. 显示、键盘、触摸驱动，GWES；量一下带 shell 的镜像大小，决定 NK/EXT
   放哪里。
5. mfcce300 + DocList + Math Xpander + Plotter-Z。
6. 电源（R-standby）和真机验证。
