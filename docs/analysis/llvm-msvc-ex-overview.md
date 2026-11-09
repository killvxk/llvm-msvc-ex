# llvm-msvc-ex 全景分析（混淆子系统 + 构建 + 移植评估）

> 分析日期：2026-11-09。方法：10 路并行只读 scout + 主线程锚点复核。文中每个事实均带 `路径:行号` 锚点，锚点校验脚本随文档生成（见文末附录说明）。

## 1. 摘要

llvm-msvc-ex（上游 `github.com/killvxk/llvm-msvc-ex`，构建产物版本串实证 `clang version 777.3.1git (https://github.com/killvxk/llvm-msvc-ex.git c2e7bfa4a7c450f0191c9781f8da53f6fd162a30)`，由 `build-release-64/Release/bin/clang.exe --version` 实跑输出）是 LLVM 18 代 monorepo 的 MSVC 构建定制 fork，相比上游多出三块：**(1)** `llvm/lib/Obfuscation/` 完整 IR 混淆子系统（30 个 .cpp = 26 个 pass + MBAMatrix 辅助类 + Utils/CryptoUtils/Plugin 三件公共件，经 `add_llvm_pass_plugin(Obfuscation)`（`llvm/lib/Obfuscation/CMakeLists.txt:1`）+ `LLVM_OBFUSCATION_LINK_INTO_TOOLS=ON`（`build-release-64/CMakeCache.txt:1177`）静态链入 clang/opt 本体）；**(2)** SVM 轻量 VM 保护链（`-x-svm`，字节码 per-instruction 加密已双端落地，`docs/svm_instruction_encryption_plan.md:14-22` ↔ `llvm/lib/Obfuscation/SVM/VMCodeGen.h:99-100,228-231`）；**(3)** 围绕它的文档/测试资产（`docs/plan/`、`docs/memory/`、`tests/test_svm.c`）。版本号 `777.3.1git` 为人为值（`llvm/CMakeLists.txt:24-32`），真实基线 LLVM 18 代（opaque-pointer API + 新 PM `OptimizationLevel` 回调签名）。

## 2. 构建体系与可重放命令

事实来源：`release_build.bat`（唯一记录 configure 参数的文件）、`build-release-64/CMakeCache.txt`（实际生效值）、`build-release-64/install_manifest.txt`（安装落地证据，3821 行，尾段 `:3813-3821` 均落 `C:/llvm-msvc_X86_64/`）。

### 2.1 关键变量（cache 实证）

| 变量 | 值 | 锚点 |
|---|---|---|
| CMAKE_GENERATOR | `Visual Studio 17 2022`（平台 X64，toolset 空） | `build-release-64/CMakeCache.txt:1860,1864,1866` |
| 编译器 | MSVC 14.44.35207（VS 生成器经 MSBuild 解析，cache 无 CMAKE_CXX_COMPILER 条目；有效编译器由 `CMAKE_ASM_COMPILER` 同目录 cl.exe 佐证） | `build-release-64/CMakeCache.txt:280` |
| LLVM_ENABLE_PROJECTS | `clang;lld;lldb` | `build-release-64/CMakeCache.txt:965` |
| LLVM_TARGETS_TO_BUILD | `X86` | `build-release-64/CMakeCache.txt:1218` |
| CMAKE_BUILD_TYPE | `release`（UNINITIALIZED，VS 多配置生成器忽略；真正生效的是 msbuild `-p:Configuration`） | `build-release-64/CMakeCache.txt:316,321` |
| LLVM_ENABLE_ASSERTIONS | `OFF` | `build-release-64/CMakeCache.txt:853` |
| CMAKE_INSTALL_PREFIX | `C:/llvm-msvc_X86_64` | `build-release-64/CMakeCache.txt:420` |
| LLVM_OBFUSCATION_LINK_INTO_TOOLS | `ON`（注释 "Statically link Obfuscation into tools (if available)"） | `build-release-64/CMakeCache.txt:1177` |
| LLVM_USE_CRT_RELEASE | `MT` | `build-release-64/CMakeCache.txt:1575` |
| 其他 | `LLVM_ENABLE_RPMALLOC=ON :970`、`LLVM_INCLUDE_TESTS=OFF`、`LLVM_INCLUDE_EXAMPLES=OFF`、`LLDB_ENABLE_PYTHON=OFF :689`、`LLVM_ENABLE_LIBXML2=OFF :917`、`LLVM_ENABLE_ZLIB=OFF :1006`、`CXX/C FLAGS=/utf-8 :324,345` | |

### 2.2 可重放命令（三步，`release_build.bat:1-8` 还原）

```bat
:: 1. 环境（release_build.bat:1）
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"

:: 2. configure（release_build.bat:4-6，源树=llvm/）
mkdir build-release-64 && pushd build-release-64
cmake .. -G "Visual Studio 17 2022" -A X64 ^
  -DLLVM_ENABLE_RPMALLOC=ON -DCMAKE_CXX_FLAGS="/utf-8" -DCMAKE_C_FLAGS="/utf-8" ^
  -DHAVE_STD_IS_TRIVIALLY_COPYABLE=0 -DLLVM_TOOL_LLVM_SHLIB_BUILD=off ^
  -DLLDB_ENABLE_PYTHON=OFF -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF ^
  -DLLVM_ENABLE_PROJECTS="clang;lld;lldb" -DCMAKE_INSTALL_PREFIX=C:/llvm-msvc_X86_64 ^
  -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_TARGETS_TO_BUILD=X86 ^
  -DLLVM_OBFUSCATION_LINK_INTO_TOOLS=ON -DCMAKE_BUILD_TYPE=release ^
  -DLLVM_USE_CRT_RELEASE=MT %userprofile%/llvm-msvc-ex/llvm

:: 3. build+install（release_build.bat:7，INSTALL.vcxproj 依赖 ALL_BUILD 一步完成）
msbuild /m -p:Configuration=release INSTALL.vcxproj
```

### 2.3 三个 bat 的分工

- `release_build.bat:1-8` — vcvars→configure→msbuild INSTALL 一条龙；`:9-13` 注释掉的 `-T "LLVM-MSVC_v143"` + compiler-rt 备用变体（未启用）。
- `only_build.bat:1-4` — 增量重装入口（vcvars→`pushd R:\build-release-64`→`msbuild INSTALL`）；R: 构建树当前不存在，属历史盘位。
- `only_build_dbg.bat:1-3` — 指向不存在的 `build-RelWithDebInfo-64/`，遗留脚本。
- 仓库根**无**顶层 `CMakeLists.txt`；顶层源目录是 `llvm/`（`release_build.bat:6` 末尾源路径）。

### 2.4 构建产物实证

- `build-release-64/Release/bin/`：`clang.exe`/`opt.exe`/`llc.exe` 三件套齐全；`build-release-64/LLVM.sln` 存在。
- `build-release-64/lib/Obfuscation/`：`Obfuscation.vcxproj` + `obj.Obfuscation.vcxproj`；`obj.Obfuscation.dir/Release/` 下 30 个 .obj 与源 30 个 .cpp 一一对应（含 `Plugin.obj`/`Utils.obj`/`CryptoUtils.obj`）。
- 静态库 `build-release-64/Release/lib/Obfuscation.lib` 存在；`Release/bin/*Obfus*.dll` 零命中 → 无动态插件，静态链入自洽。
- `clang --version` 实跑：`clang version 777.3.1git (https://github.com/killvxk/llvm-msvc-ex.git c2e7bfa4a7c450f0191c9781f8da53f6fd162a30)`，Target `x86_64-pc-windows-msvc`。

## 3. 仓库拓扑（混淆/构建相关）

| 目录 | 职责 | 关键锚点 |
|---|---|---|
| `llvm/lib/Obfuscation/` | 混淆子系统本体：26 pass + 公共件，30 个 .cpp 全部在 `CMakeLists.txt:2-31` | `llvm/lib/Obfuscation/CMakeLists.txt:1-38` |
| `llvm/lib/Obfuscation/SVM/` | SVM VM 生成工具头（8 个 .h/.hpp，无 .cpp）：`FuncRedirector.hpp`/`GlobalInit.hpp`/`HandleCallTool.h`/`IRVMModuleTool.h`/`PhiSimplifyTool.h`/`SVMInterpreterGen.h`/`SwitchToIf.h`/`VMCodeGen.h` | glob 实证 |
| `llvm/lib/Obfuscation/vmlib/` | `vm_routine.c`（66.8KB，用户侧 VM 运行时，`-x-vmobf` 外部运行时；**不在** CMake 源列表） | `llvm/lib/Obfuscation/vmlib/vm_routine.c:1` |
| `llvm/lib/Obfuscation/Eigen/` | vendored header-only 线代库，MBA 系依赖（`MBAObfuscation.cpp:2-3` include） | `llvm/lib/Obfuscation/Eigen/src/Core/util/Macros.h:110` |
| `notcode/` | 未接入主构建的参考/原型代码（详见 §3.1） | §3.1 |
| `xVMPInterpreter/` | 独立 VM 解释器原型 + 孤儿 `vm.h`（详见 §3.2，已排除出移植） | §3.2 |
| `docs/` | `svm_instruction_encryption_plan.md` + `plan/`（4 文件）+ `memory/`（13 文件）设计史 | §6 |
| `tests/` | 端到端基线：`test_svm.c`/`test_svm.bat`/`test_svm.o` | §5.3 |
| `build-release-64/` | VS2022 构建树（含 LLVM.sln 与已编译产物） | §2.4 |

### 3.1 notcode/（档案区，零构建关联）

- 与主构建零关联实证：`llvm/lib/Obfuscation/` 全目录对 `notcode|VMP.h` 的唯一命中是注释（`llvm/lib/Obfuscation/SmallVmpPass.cpp:13` "User code must include VMP.h"）；`llvm/CMakeLists.txt`、`llvm/lib/CMakeLists.txt`、`llvm/cmake/`、根级 `*.bat`、`docs/`、`tests/` 对 `notcode|OmCTF|Polaris` grep 全部无命中。不进 CMake、不被编译。
- 三条血缘线：
  1. `notcode/VMP.h`（715 行，纯 C 单头）= pass 内联生成解释器的 ABI 孪生头：`uint64_t vm_exec(const uint8_t* code, uint32_t code_size, void** vm_args, uint64_t vm_num)`（`notcode/VMP.h:352`，与 `SVM/SVMInterpreterGen.h:87-98` 签名逐参一致）；弱符号 `handle_call` 默认实现（`:207-211`）；`IRVM_SECTION` 节宏 `__attribute__((section(".irvm")))`（`:8`，与 `SVM/GlobalInit.hpp:186` 产段 `.data.irvm`/`.rodata.irvm` 同族）；trace 基建（`vmp_trace_init_from_env` 读 `VMP_TRACE` 环境变量，`:21-31`）；FCmp 16 谓词全实现（`:327`）。供 `GenerateInterpreter=false` 的用户手动携带场景。
  2. `notcode/OmCTF-2025/services/polyphonia/obfuscator_src/` = 主树 `vm_def.h`/`vm_routine.c`/`VMObfuscatorPass` 的来源原型：`vm_defs.h:8-53` opcode 枚举与主树 `vm_def.h:10-60,90` 逐项同值；`vm_runtime.c`（1755 行，Linux 原版）↔ 主树 `vmlib/vm_routine.c`（1877 行，加 `_WIN32` 分支 `:11-17`）；共享弱符号接口 `vm_func_table`/`vm_data_table`/`vm_data_is_enc`（OmCTF L19-27 ↔ 主树 L26-43）；其 `tests/varargs/`（6 个）可当移植验证 fixture。
  3. `notcode/Polaris-Obfuscator/`（github.com/za233/Polaris-Obfuscator 快照，基于 LLVM 16.0.6，README:3,9-28）= A/B 组 IR pass 上游：主树头部直接署名（`llvm/lib/Obfuscation/AliasAccess.cpp:2`、`CustomCC.cpp:2-3` "Ref to ... Copied by root on 12/11/2024"）；主树 `CombineFunctions.cpp` 对应其 `MergeFunction`；未吸收增量：`BogusControlFlow2.cpp`、`Pipeline.cpp`（插件注册模板）、`X86RubbishCode.cpp`（唯一 MIR 级，`insertDirtyBytes` L153-156）。命名空间差异：Polaris 侧 `namespace polaris`，主树已改 `namespace llvm`。
- 零复用项：`notcode/SmallVmp/` = 纯 LLVM 15.0.0 检出（`notcode/SmallVmp/llvm/CMakeLists.txt:18-26`，无任何 SmallVmp pass 代码，名字误导）；`notcode/llvm-project/` = 纯 LLVM 21.1.8 检出（`cmake/Modules/LLVMVersion.cmake:4-10`）。

### 3.2 xVMPInterpreter/（孤儿工件，已排除出移植）

- 入口 `void vm_interpreter(void)`（`xVMPInterpreter/xVMPInterpreter.c:512`，声明 `.h:131`）：无参、全局状态、不可重入；参数经 `gv_data_seg` 固定槽（`xVMPInterpreter.c:591-594`），返回值写 data_seg 偏移 0（`return_handler`，`.c:454-467`）。
- 跨函数调用未实现：`call_handler` 空函数体（`.c:469-471`），未知 opcode 静默退出（`.c:575-576`）——单函数体原型。
- `build_ir.py:11-33` 把 `xVMPInterpreter.bc`（生成命令 `build.md:2`：`clang -O0 -Xclang -disable-O0-optnone -emit-llvm -c`）转 hex 写进 `vm.h`（`std::string binary_ir`，34184 字节）。**该 `binary_ir` 在整棵树零消费者**：`llvm/lib/Obfuscation/`、`SVM/`、`vmlib/`、`notcode/`、`tests/` 定向 grep 全空；两份 vm.h（`xVMPInterpreter/vm.h` 与 `llvm/lib/Obfuscation/vm.h`）逐字节一致（34184 字节、BC 魔数 `\x42\x43\xc0\xde`），互为冗余拷贝。
- 现行 SVM 链路不依赖它：解释器由 `SVMInterpreterGen` 内联生成（`llvm/lib/Obfuscation/SVM/FuncRedirector.hpp:46-61` `getOrCreateVMExec`，ABI `intptr_t vm_exec(const uint8_t* code, uint32_t size, void** args, intptr_t num)`，`FuncRedirector.hpp:54-55`、`SVMInterpreterGen.h:87-98`）。移植排除 `xVMPInterpreter/` 不损失现行功能。

### 3.3 工具痕迹（非构建输入，移植不携带）

- 根级 `.codemap/`（`meta.json` 1.7MB + `graph.json` 107MB）与 `llvm/lib/Obfuscation/.codemap/`（graph.json 105,735 行 + slices/Eigen.json 102,300 行）— CodeMap 图谱生成物，不进 CMake、非源码；`llvm/lib/Obfuscation/docs/memory/`（含 `2026-02-25-codemap-scan-obfuscation.md`）为 Obfuscation 目录内嵌 agent 记忆子树（记录图谱扫描，373 文件/3 模块），同样纯工具痕迹。
- A4 补漏结论：`clang/include/clang/AST/Decl.h:2494`（`isMSVCRTEntryPoint`，上游既有 API）、`clang/lib/Basic/TargetInfo.cpp` hunk、`lld/COFF` 四文件补丁均 grep `Obfu|vmp|x-svm|obf|svm` 零命中——属上游/MSVC 构建兼容噪声，与混淆子系统无关，移植不必携带；`copyndk.py`/`commit.ps1`/`.idea/`/`.vscode/`/`skills/`/`.claude/` 同类杂项。混淆代码面唯一入口仍是 `llvm/lib/Obfuscation/**`。

## 4. 混淆子系统地图

### 4.1 注册与门控机制

- 注册入口：`llvm/lib/Obfuscation/Plugin.cpp:33-92` `getObfuscationPluginInfo()` — 三段 EP 回调 + CodePicPass 两条 pipeline-parse 回调（`:37-45`）；`llvmGetPassPluginInfo()` 被 `#ifndef LLVM_OBFUSCATION_LINK_INTO_TOOLS` 包裹（`:94-99`），静态链入时排除、EP 回调经 `process_llvm_pass_plugins` 直连工具。
- 门控三态（`llvm/lib/Obfuscation/Utils.cpp:113-137`）：declaration→false（`:118`）；`available_externally`→false（`:123`）；注解含 `no<attr>`→false（`:130`）；含 `<attr>`→true（`:135`）；否则跟随全局 cl::opt flag（`:140`）。
- EP 段内 pass 顺序（逐行照抄自 `Plugin.cpp`）：PipelineStart = SmallVmp→VmObfuscator→Split(B)→BCF→Sub→MBAObf→LinearMBA→Fla→CodePic(M)→CodePic(F)（`:49-59`）；OptimizerEarly = StringObf→DataObf→xvm→CombineFunc→FlaEnh→VarRot→AliasAccess→GvEnc→CustomCC（`:64-72`）；OptimizerLast = ConstObf→IndCall→Ingv→VmFla→Linearize→EasyCfg→IndBr→AntiIDA（`:79-86`）。

### 4.2 Pass 总表（26 pass + 辅助 + 公共件）

| # | Pass 类名 | flag | 注解 | 文件(.cpp 行数) | 锚点 |
|---|---|---|---|---|---|
| 1 | AntiIDAPass (M) | `ida-obfus` | `ida-obfus` | AntiIDA (94) | flag `AntiIDA.cpp:23` 门 `:39` |
| 2 | BogusControlFlowPass (F) | `bcf`(+`bcf_prob` :142,`bcf_loop` :149) | `bcf` | BogusControlFlow (693) | flag `:139` 门 `:686` |
| 3 | CombineFunctionsPass (M) | `x-combine` | 正则注解默认 `combine_func` | CombineFunctions (340) | flag `:43` 正则 `:307-310` 门 `:331` |
| 4 | ConstObfuscationPass (F) | `const-obfus`(+`const-times` :33) | `const-obfus` | ConstObfuscation (447) | flag `:30` 门 `:442` |
| 5 | DataObfuscationPass (F) | `data-obfus`(+`data-prob` :38,`data-times` :42) | `data-obfus` | DataObfuscation (419) | flag `:34` 门 `:414` |
| 6 | EasyCfgPass (M) | `x-cfg`(+`x-cfg-iterations` :16) | `x-cfg` | EasyCFG (161)，头 **EasyCFG.hpp** | flag `:19` 门 `:141` |
| 7 | FlatteningPass (F) | `fla` | `fla` | Flattening (240) | flag `:35` 门 `:228` |
| 8 | FlatteningEnhanced (F) | `x-fla-enh`(+`x-fla-ender` :25) | `x-fla-enh` | FlatteningEnhanced (337) | flag `:23` 门 `:326` |
| 9 | GlobalsEncryption (M) | `gv-enc` | 无（纯 flag） | GlobalsEncryption (209) | flag `:47` 门 `:52` |
| 10 | IndirectBranch (F) | 无 | `ind-br`（注解独占） | IndirectBranch (156) | 门 `:53` |
| 11 | IndirectCallPass (F) | `ind-call` | `ind-call` | IndirectCall (169) | flag `:31` 门 `:164` |
| 12 | IngvObfuscationPass (M) | `ind-gv` | `ind-gv` | IndirectGlobalVars (179) | flag `:37` 门 `:46` |
| 13 | Linearize (M) | `x-linear` | 无（纯 flag） | Linearize (906) | flag `:22` 门 `:874` |
| 14 | LinearMBAPass (F) | `x-mba`(+`x-mba-prob` :24) | `x-mba` | LinearMBA (289) | flag `:20` 门 `:284` |
| 15 | MBAObfuscationPass (F) | `mba-subs`(+`mba-times` :34,`mba-prob` :38,`linear-mba-terms` :43) | `mba-subs` | MBAObfuscation (398) | flag `:30` 门 `:393` |
| 16 | SmallVmpPass (M) | `x-svm` | `x-svm`（函数级门在 SVM/IRVMModuleTool.h:180） | SmallVmpPass (100) | flag `:26` |
| 17 | SplitBasicBlockPass (F) | `split`(+`split_num` :34) | `split` | SplitBasicBlock (127) | flag `:31` 门 `:120` |
| 18 | StringObfuscationPass (M) | `string-obfus` | `string-obfus` | StringObfuscation (594) | flag `:33` 门 `:412` |
| 19 | SubstitutionPass (F) | `sub`(+`sub_loop` :44) | `sub` | Substitution (556) | flag `:40` 门 `:550` |
| 20 | VariableRotationPass (M) | `x-var-rot` | 无（纯 flag） | VariableRotation (286) | flag `:40` 门 `:280` |
| 21 | VmFlatObfuscationPass (F) | `vm-fla`(+`vm-prob` :64,`vm-fla-level` :70) | `vm-fla` + `x-full`(`:767`) | VMFlatten (813) | flag `:46` 主门 `:805` |
| 22 | VmObfuscatorPass (M) | `x-vmobf` | 无（纯 flag） | VMObfuscatorPass (1632) | flag `:18` 门 `:1205` |
| 23 | xvmPass (M) | `x-vm` | `x-vm` | xVMProtect (1088) | flag `:45` 门 `:1076` |
| 24 | AliasAccess (M) | 无 | `alias-access`（注解独占） | AliasAccess (246) | 门 `:62` |
| 25 | CustomCC (M) | 无 | `custom-cc`（注解独占；命中随机选 Obfu1/2/3 调用约定，CC 表 `:44-45`） | CustomCC (84) | 门 `:49` |
| 26 | CodePicPass (M+F 双 run) | `x-pic`(+`xpic-memcpy` :45,`xpic-memset` :49,`xpic-global` :53) | 无（纯 flag） | CodePicPass (612) | flag `:41` 门 `:585,600` |

辅助与公共件：`MBAMatrix`（195 行，`namespace ollvm` 线性矩阵化简器，`MBAMatrix.cpp:10` `simplify()`，无门控）；`Utils.cpp`（307 行，`toObfuscate :113-160`、`readAnnotate`、`fixStack`、`INIT_CONTEXT/TYPE_I32/CONST_I32/RANDOM` 宏 `Utils.h:8-17`）；`CryptoUtils.cpp`（1154 行，AES-CTR PRNG + SHA256 改编自 libtomcrypt `CryptoUtils.h:11-13`，全局单例 `cryptoutils` `CryptoUtils.h:34`，`-aesSeed` 固定种子 `CryptoUtils.cpp:52`，Windows 走 `WinDevRandom` `:628`）；`Plugin.cpp`（99 行，§4.1）。

门控三态归纳（实测更正：xvmPass 的 `-x-vm` 旗标在源码 `xVMProtect.cpp:1076` 被硬编码 `toObfuscate(false,...)` 绕过，实为注解独占型，归 16→15 + 注解独占 3→4）；toObfuscate 型 15 个；纯 flag 型 6 个（GlobalsEncryption/Linearize/VariableRotation/VmObfuscatorPass/CodePicPass + CombineFunctions 主门）；注解独占型 4 个（IndirectBranch/AliasAccess/CustomCC/Xvm，`no<attr>` 同样可关）。

## 5. 数据流：源码 → flags → IR passes → 后端/LTO

### 5.1 flag 透传链与 EP 触发（全链路实证）

**端到端链路**：`clang -mllvm -x-mba a.c` → 驱动 `clang/lib/Driver/ToolChains/Clang.cpp:7348-7359`（`Args.filtered(options::OPT_mllvm)` 逐个 `A->render` 透传给 cc1，唯一特判 `-disable-llvm-optzns` 改写裸 flag；`:8883-8886` 同选项再转发 LTO 链接调用）→ 选项定义 `clang/include/clang/Driver/Options.td:4488-4491`（`Separate` + `MarshallingInfoStringVector<FrontendOpts<"LLVMArgs">>`，收集走生成 marshalling 代码，故手写 grep `OPT_mllvm` 在 CompilerInvocation 零命中）→ `clang/include/clang/Frontend/FrontendOptions.h:527-528` `LLVMArgs` → **cc1 解析点 `clang/lib/FrontendTool/ExecuteCompilerInvocation.cpp:222-231`**（注释 "AFTER plugins have been loaded"；fake argv0 = `"llvm-msvc (LLVM option parsing)"` fork 指纹 `:226`；`cl::ParseCommandLineOptions` `:230`，时点在 FrontendAction 之前，cl::opt 全局先于建管线就绪）。辅助路径：cc1as `clang/tools/driver/cc1as_main.cpp:300,664-672`；显式排除点 `ModuleDepCollector.cpp:123`（LLVM 选项不参与模块 hash）。

**静态链入物理基础**：`add_llvm_pass_plugin(Obfuscation)` → `llvm/cmake/modules/AddLLVM.cmake:1068` 生成 option `LLVM_OBFUSCATION_LINK_INTO_TOOLS`（`LLVM_${name_upper}_LINK_INTO_TOOLS`）→ `:1080-1082` 静态时 `add_llvm_library(Obfuscation OBJECT …)` + `target_compile_definitions(... PRIVATE LLVM_OBFUSCATION_LINK_INTO_TOOLS)`（= `Plugin.cpp:94` `#ifndef` 判据，静态构建下动态插件入口 `llvmGetPassPluginInfo()` 整体裁除）→ `:1092` `set_property(GLOBAL APPEND PROPERTY LLVM_STATIC_EXTENSIONS Obfuscation)` → `process_llvm_pass_plugins :1112-1127` 挂进 `LLVMExtensions` 目标 → `:1149-1154` 生成 `Extension.def`（`HANDLE_EXTENSION(Obfuscation)`）。

**三个消费点**：`clang/lib/CodeGen/BackendUtil.cpp:890-895`（cc1 后端管线，`RegisterPassBuilderCallbacks(PB)` 展开；动态插件 `-fpass-plugin` 路径 `:879-887` 并存）、`llvm/lib/LTO/LTOBackend.cpp:187-192`（LTO 同注册）、`llvm/lib/Extensions/Extensions.cpp:9-13`（`extensions_anchor()` 仅引用防 GC）。grep `initializeExtensions` 于 `llvm/lib`+`clang/lib` 零命中——本 fork 用内联展开，非上游旧式统一注册函数。

**O0 触发结论：三段 EP 全部无条件触发**。`BackendUtil.cpp:919` `mapToLevel(CodeGenOpts)` → `:1018-1019` `buildPerModuleDefaultPipeline(Level)`（O0 无特判）→ `llvm/lib/Passes/PassBuilderPipelines.cpp:1492-1496` O0 早退 `buildO0DefaultPipeline` → `:2003/:2053/:2071` 三个 invoke 点均无 `OptimizationLevel` 条件（invoke 辅助 `:342-350,362-365` 仅透传 Level）。反例佐证：官方 ObjCARCExpand 的 O0 守卫写在回调体内（`BackendUtil.cpp:925-930`），而本 fork `Plugin.cpp` 三段 lambda 无任何守卫——O0 下混淆 pass 全进管线，是否实际变形由 `toObfuscate`+cl::opt 在 pass `run` 内决定。

**fork 特殊行为（移植须保留语义）**：CodePicPass 两条 pipeline-parse 回调（`Plugin.cpp:37-45`）**忽略 `Name`**——任何 `-passes=<任意名>` 都塞入 CodePicPass（模块级+函数级各一）。三段 lambda 的 `OptimizationLevel Level` 形参从不读取。

### 5.2 SVM 指令加密数据流（已实现，双端对称）

- 布局 `[seed u64][len u16][crc16 u16][payload]`：设计 `docs/svm_instruction_encryption_plan.md:14-22` ↔ 生成 `SVM/VMCodeGen.h:99-100`（seed）+ `:228-231`（len/crc）；解释侧逐字节重算 `SVM/SVMInterpreterGen.h:545-549`。
- key 链（xxhash-like，PRIME1-4 + rotl31 三轮 avalanche）：`VMCodeGen.h:161-176` ↔ `SVMInterpreterGen.h:406-424`；keystream `rotl64(key+PRIME5,17)*PRIME1`：`VMCodeGen.h:186-189` ↔ `SVMInterpreterGen.h:453-457`。
- 回退 key `hash(seed,ip)`（多前驱入口）：生成侧 `VMCodeGen.h:178-184,617-619` ↔ 解释侧 CRC 失败重试 `SVMInterpreterGen.h:277-281,301-303`。
- 在线解密不缓存明文（`docs/...plan.md:7,64-70`）：全部 handler 经 `readEncU8/U16/U32/U64`（`SVMInterpreterGen.h:366-378,461-507`）；ip 基准=含 seed 的 code base，首指令 ip=8（`SVMInterpreterGen.h:201-205`）。
- 范围外：`vmlib/`（-x-vmobf 外部运行时）grep `PRIME|crc16|seed|rotl` 零命中——该方案仅覆盖 SVM 链路（设计未承诺，非缺陷）。

### 5.3 端到端验证基线（tests/）

- `tests/test_svm.c`：`protected_add`（`:8-11`）/`protected_factorial`（`:14-21`，含循环回边）带 `annotate("x-svm")`；`normal_multiply`（`:24-26`）无注解对照组；main 打印三行 + 内建自检（`:37-49`），退出码即判据（`:51-55`）。
- `tests/test_svm.bat`：`set CLANG=C:\llvm-msvc_X86_64\bin\clang.exe`（`:4`）；Test1 注解路径 compile`:11`→link`:20`→run`:27`（errorlevel `:28-31`）；Test2 全局 flag `-mllvm -x-svm` compile`:36`→link`:43`→run`:50`；全过打印 `All x-svm tests PASSED!`（`:57-59`）；清理 `:71`。
- `tests/test_svm.o`（12.8KB COFF x64 MSVC ABI）为 VM 化工件实证：符号 `protected_add_code`/`protected_factorial_code` ↔ `SVM/IRVMModuleTool.h:132`；节名 `.rodata.svm` ↔ `IRVMModuleTool.h:141`；内联解释器 `vm_exec.2/.3` ↔ `SVMInterpreterGen.h:97-98`。
- `llvm/test/Obfuscation` 目录不存在——上游 lit 体系无混淆用例，端到端全靠手工 bat。
- `tests/` 全目录仅此 3 文件；唯一另一处混淆用法示例在 `README.md:160,168,181`（`[[clang::annotate("combine_func[tag]")]]` C++ 属性版）。

## 6. 与 upstream 的定制点清单

上游关系：`origin = https://github.com/killvxk/llvm-msvc-ex.git`（`.git/config:10-19`），`upstream = https://github.com/backengineering/llvm-msvc.git`（= aftermathlabs/llvm-msvc 改名前身，GitHub 重定向可达）；fork = 上游 dev + 315 ahead 提交、behind 0（merge-base 9a0b422 = 上游 dev tip，上游已被完整吞入）；上游同步模式 = 定期 `Merge backengineering|aftermathlabs:dev` + 升级前 `.orig` 快照。本地 reflog 已重建只剩一条（`.git/logs/HEAD:1`）；本地分支 `dev`/`pr-21`/`feat/neverc-obf-port`；语义 tag `svm-ok`（07455d8，SVM 验证标记）、`up-date-2026-1-4`（上游同步标记）。

**LLVM 核心侵入点仅 4 处**（其余全部自持在 `llvm/lib/Obfuscation/`）：

| # | 文件 | 改动 | 内容 | 锚点 |
|---|---|---|---|---|
| 1 | `llvm/include/llvm/IR/CallingConv.h` | +9 | Obfu1–8 = 108–115（`GRAAL=107` 与 `MaxID=1023` 之间插入），升级备份 `.upgrade-backup/CallingConv.h.orig:254-261` 与现文件逐行相同 | `llvm/include/llvm/IR/CallingConv.h:254-261` |
| 2 | `llvm/lib/CodeGen/TargetLoweringBase.cpp` | +9 | `isSuitableForJumpTable` 尊重 `no-jump-tables` 属性 → return false（提交 c4139fc，2025-12-11） | `llvm/lib/CodeGen/TargetLoweringBase.cpp:1685-1687` |
| 3 | `llvm/CMakeLists.txt` | 1/1 | 版本号 777/3/1 | `llvm/CMakeLists.txt:24-32`（原文全文见下） |
| 4 | `llvm/lib/CMakeLists.txt` | +1 | `add_subdirectory(Obfuscation)` | `llvm/lib/CMakeLists.txt:49` |

```c
// llvm/CMakeLists.txt:24-32 原文
if(NOT DEFINED LLVM_VERSION_MAJOR)
  set(LLVM_VERSION_MAJOR 777)
endif()
if(NOT DEFINED LLVM_VERSION_MINOR)
  set(LLVM_VERSION_MINOR 3)
endif()
if(NOT DEFINED LLVM_VERSION_PATCH)
  set(LLVM_VERSION_PATCH 1)
endif()
// :33-35 另有 LLVM_VERSION_SUFFIX git
```

**外围侵入**：clang 侧 `clang/include/clang/AST/Decl.h` +13/-2、`Type.h` +3、`clang/lib/Basic/TargetInfo.cpp` +14（注解/属性管线适配）；lld/COFF 四文件小补丁（`Config.h +1`、`Driver.cpp +4`、`Options.td +1`、`Writer.cpp +35/-8` = 伪造 Rich 头提交）。**治理面剥离**：删除上游全部 `.github/workflows/*`（20+）、CODEOWNERS、PR 模板、`.ci/*`、`.claude/skills/*`；仅留本地构建资产（`release_build.bat` 等）与自加文档/测试。`llvm/lib/Obfuscation/.codemap/`（graph.json 105,735 行等）为索引工件，非构建输入。

**移植关键推论（CustomCC 的隐藏依赖）**：`CustomCC.cpp:44-47` `ObfuCCs[]` 引用 `CallingConv::Obfu1..Obfu8`——这些枚举是 fork 注入（上游 merge-base raw@9a0b422 中 GRAAL=107 直接跳 MaxID，无 Obfu 条目）。组 B 移植必须随带 8 行 CallingConv.h 枚举补丁，否则编译失败。X86CallingConv.td 未改（不在 compare 改动面），Obfu1-8 在 ISel 层走默认降级路径，与源 fork 行为一致地工作。

## 7. 风险与缺口

### 7.1 Windows-only 依赖（真依赖仅一处）

- `CryptoUtils.cpp:623-627` — `#if defined(_WIN64)||defined(_WIN32)` → `<windows.h>`+`<wincrypt.h>`，`:624` 注释 `// sic! don't change include order`（include 顺序敏感，移植高危点）；`:677-680` 运行时分支选 `WinDevRandom`（CryptGenRandom），POSIX 侧 `:676` 读 /dev/urandom。
- `#ifdef _MSC_VER → <vcruntime_string.h>` 同构块 ×8（纯移植痕迹）：`AliasAccess.cpp:33-35`、`CombineFunctions.cpp:36-38`、`CustomCC.cpp:35-37`、`GlobalsEncryption.cpp:40-42`、`IndirectBranch.cpp:36-38`、`VMFlatten.cpp:34-36`、`VariableRotation.cpp:34-36`、`xVMProtect.cpp:34-36`。
- `SVM/`：`_WIN32|_MSC_VER|windows.h|io.h|direct.h` 零匹配——无可移植性风险。
- `vmlib/vm_routine.c` 自带跨平台护栏（`:11-17` POSIX 头 `#ifndef _WIN32` 包裹），可直接编译；`:1` `_CRT_SECURE_NO_WARNING` 拼写老宏（标准带 S 复数），MSVC 下不生效（无害噪音）。

### 7.2 未接入构建的源文件（唯一一个）

- `vmlib/vm_routine.c` — 不在 `CMakeLists.txt:2-31` 的 30 个 .cpp 中，设计上是用户侧运行时；移植时若误以为已随插件编译会被漏链（本移植按计划显式追加进 Obfuscation 目标源列表）。
- 其余：CMake 源列表与磁盘 30 .cpp 集合完全一致，无孤儿 .cpp；`SVM/` 仅 8 头无 .cpp；Eigen header-only。

### 7.3 死代码/大块注释（约 520 行，可原样带过）

- `ConstObfuscation.cpp:37-308`（旧版实现整段注释）；`DataObfuscation.cpp:47-57,158-407`（旧方法注释）；`StringObfuscation.cpp:251-261`；`VMFlatten.cpp:174-196`；`xVMProtect.cpp:1033-1042`；`Utils.cpp:76-96`。
- 11 个头文件的 `//#include <Obfuscation/PassRegistry.h>` 残留指向不存在文件（`AntiIDA.h:6`、`ConstObfuscation.h:6`、`DataObfuscation.h:6`、`EasyCFG.hpp:6`、`IndirectCall.h:9`、`IndirectGlobalVars.h:9`、`Linearize.h:6`、`StringObfuscation.h:6`、`VMFlatten.h:43`、`VariableRotation.h:6`、`xVMProtect.h:6`）——已是注释，不碍编译，移植时可直接删。
- `#if 1` 永久真条件 ×2（`VMFlatten.cpp:324`、`xVMProtect.cpp:413`）；TODO 陈旧注释 ×3（`Linearize.cpp:430,620,649`，后两处函数已实调）；`SVMInterpreterGen.h:18`/`VMCodeGen.h:25` 注释掉的调试宏。

### 7.4 文档层状态（影响移植决策的事实）

- `docs/plan/obfuscation-plugin-extraction-plan.md`（550 行，设计未实施 `:452-489` 全 `[ ]`）：把混淆子系统抽成 LLVM 16-21 多版本插件的完整设计——**API 差异表 `:78-118`**（opaque pointer `Type::getInt8PtrTy` vs `PointerType::get`、ConstantExpr 废弃、EP 回调签名 19+ 加 `ThinOrFullLTOPhase`、`Optional`→`std::optional`、`startswith`→`starts_with`）与 **Compat.h 全文 `:154-355`**、CMake 模板 `:359-444`，是跨版本移植的现成参考。其放弃清单（`:20-41`）：CustomCC（需改 CallingConv.h + X86CallingConv.td）、backend-obfu、no-jump-tables 增强、lld rich header——**CustomCC 在纯插件形态下不可移植**（依赖后端调用约定改动），静态链入形态则随工具链携带。
- 其余文档状态见 §6。

## 8. 移植建议（映射到 NeverC，B2 落地核查结论 + 实测适配清单）

### 8.1 NeverC 接线点核查结论（B2 五点）

| # | 核查点 | 结论 | NeverC 侧锚点 |
|---|---|---|---|
| 1 | lib 子目录挂载点 | `add_subdirectory` 列表 :3-24，无 Obfuscation；移植加一行即可 | `NeverC/llvm/lib/CMakeLists.txt:3-24` |
| 2 | `add_llvm_pass_plugin` | **存在且逻辑同源**（option 生成/OBJECT 库/LLVM_STATIC_EXTENSIONS），CMake 模板原样可用 | `NeverC/llvm/cmake/modules/AddLLVM.cmake:1057-1097` |
| 3 | 静态扩展消费链 | **休眠**：`process_llvm_pass_plugins(GEN_CONFIG)` 被 `LLVM_ENABLE_PLUGINS OFF` 门控关闭（cache `:192`；门 `NeverC/llvm/CMakeLists.txt:1099-1101`），且全树无 Extension.def 的 C++ 消费者（grep 仅命中 2 个 cmake 文件）→ 需 fallback 直连（见 8.2） | `NeverC/llvm/CMakeLists.txt:1099-1101`、`neverc/cmake/caches/NeverC.cmake:192` |
| 4 | `-mllvm` 透传 | **全链支持**：驱动转发（`neverc/lib/Invoke/ToolChains/NeverC.cpp:207-208`、`NeverC.cpp:4027-4030`、`CommonArgs.cpp:507-510`）→ cc1 解析（`neverc/lib/Compiler/Core/ExecuteCompilerInvocation.cpp:162-165` "Honor -mllvm"）→ LTO 后端（`neverc/lib/Linker/Core/Driver/CommonLTOConfig.cpp:84-91`）→ **开关门控零改动分支成立**（`neverc -mllvm -x-mba …` 与源 fork 等价）。NeverC 锚点以 clone 根为基准（`NeverC/llvm/...` = `<clone>/llvm/...`） | 如左 |
| 5 | cache 关键变量 | 生成器未硬编码（-G Ninja 可用）；`LLVM_ENABLE_PROJECTS=neverc` FORCE `:51`、`LLVM_TARGETS_TO_BUILD=AArch64;X86` FORCE `:52`、静态 CRT `/MT` `:167-169`、`LLVM_INCLUDE_TOOLS OFF`+`NEVERC_BUILD_TOOLS ON` `:62-64`；`-DLLVM_OBFUSCATION_LINK_INTO_TOOLS=ON` 经 `-D` 叠加生效（configure 实测打印 "Registering Obfuscation as a pass plugin (static build: ON)"） | `neverc/cmake/caches/NeverC.cmake` |

### 8.2 实际采用的接线（B4/B5，已全部落地并编译通过）

1. **文件落位**：A+B+C+D 全组 410 文件 → `NeverC/llvm/lib/Obfuscation/`（30 pass + 公共件 + SVM/ 8 头 + Eigen/ + vm.h/vm_def.h + VMP.h 取自 `notcode/VMP.h`）；排除 `.codemap/`、内嵌 `docs/`（§3.3 工具痕迹）；`xVMPInterpreter/` 全目录不复制（§3.2 孤儿工件）。
2. **CMake**：Obfuscation 目录照抄源模板 + `LINK_COMPONENTS Analysis/Core/Passes/Scalar/Support/TransformUtils`（NeverC 的 csupport 头挂在 `LLVMSupport` 的 PUBLIC include 上，`NeverC/llvm/lib/CSupport/CMakeLists.txt:151-154`——不声明组件则 `csupport/*.h` 全部 C1083，这是两树最大的隐性差异）；`NeverC/llvm/lib/CMakeLists.txt` 加 `add_subdirectory(Obfuscation)`。
3. **注册**：NeverC 静态扩展链休眠 → 计划 fallback：`neverc/CMakeLists.txt` `target_link_libraries(neverc PRIVATE Obfuscation)` + `neverc/lib/Emit/Backend/BackendUtil.cpp` 直连 `getObfuscationPluginInfo().RegisterPassBuilderCallbacks(PB)`（等价源 fork `BackendUtil.cpp:890-895` 的 HANDLE_EXTENSION 消费点；neverc 自家 runtime-linker pass 用同一 EP 机制，`BackendUtil.cpp:763-834`，管线触发有保证）。EP 回调 API 同为 18 代双参签名（`NeverC/llvm/include/llvm/Passes/PassBuilder.h:453-483`），Plugin.cpp 原样照抄零改动。
4. **开关**：-mllvm 透传成立 → 门控零改动（无 NEVERC_OBF_DEFAULT_ON 依赖）。

### 8.3 版本适配实录（B3，编译错误驱动；两树真实差异远小于 16-21 跨度预估）

| 差异点 | 适配 | 性质 |
|---|---|---|
| NeverC LLVM 头是纯 C `csupport` 库的薄包装（APInt/APFloat/StringRef 全量内联化，`llvm/lib/CSupport/`） | CMake LINK_COMPONENTS（见 8.2-2） | 构建 wiring |
| `CryptoUtils.h:196` 单字母宏 `S(x,n)` 与 NeverC 头形参 `S` 冲突（raw_ostream.h/SourceMgr.h） | 改名 `CU_S`（CryptoUtils.h:196-204，用点仅自身 6 处） | 宏改名，零逻辑 |
| `StringRef::lower()` 返回 `SmallString<256>`（上游 std::string），`+=` 链断裂 | Utils.cpp:61 显式物化 std::string | API 形状 |
| `llvm/Demangle/Demangle.h` 不存在 | Utils.cpp demangle→恒等（C 前端无 C++ 名，行为等价） | API 缺失 |
| `Local.h` 无 DemoteRegToStack/DemotePHIToStack | vendoring 源 fork `DemoteRegToStack.cpp` + Utils.h 声明 | 上游文件 vendoring |
| `Transforms/Utils/LowerSwitch.h`、`Scalar/Reg2Mem.h` 不存在 | vendoring 头（canonical 路径）+ .cpp（Obfuscation/ 内编），裁 legacy-PM 尾（NeverC 已删 legacy 注册机制） | 上游文件 vendoring |
| `Function::hasCXXEH/hasCXXSEH` 删除（C-only 前端） | AntiIDA.cpp:36、FlatteningEnhanced.cpp:67、VMFlatten.cpp:283 三处守卫按恒假化简 | 死条件化简 |
| `AllocaInst::isSwiftError` + `Attribute::SwiftError` 全删 | ConstObfuscation.cpp:321 守卫注释（NeverC IR 不可表示 swifterror） | 死条件化简 |
| `CallingConv.h` 无 Obfu1-8 | 补 8 行枚举（108-115，镜像源 fork `:254-261`；X86CallingConv.td 无需动——源 fork 也没动，ISel 走默认降级） | 核心头补丁 |
| `vm_routine.c` 依赖 ELF weak 符号（MSVC 不可表示） | 移出插件源列表（与源 fork 语义一致：它本就不进 CMake，是用户侧运行时）；`-x-vmobf` 用户运行时需 weak-symbol 目标 | 已知限制 |

### 8.4 逐组可行度（最终）

| 组 | 可行度 | 备注 |
|---|---|---|
| A 基础 IR（11 pass） | ✅ 全编通过 | Eigen 依赖零问题 |
| B 间接/数据（6 pass） | ✅ 全编通过 | CustomCC 经 CallingConv.h 补丁可用（§6 推论）；CodePicPass pipeline-parse 回调保留 |
| C CFG 变形（6 pass） | ✅ 全编通过 | LowerSwitch/Reg2Mem vendoring 覆盖其依赖 |
| D VM（SVM 链 + VMP.h） | ✅ 编译面全通 | SmallVmp/xVMProtect/VMFlatten/VMObfuscator 均过；vm_routine.c 留树不编（8.3 末行）；xVMPInterpreter 按决策排除 |

（端到端验证与回归结论见 B6 阶段输出：tests/port/smoke.c + check-neverc。）

### 8.5 端到端验证与回归裁决（B6 终验，2026-11-09）

**冒烟（tests/port/smoke.c，最终二进制）**：基线与混淆版（`-mllvm -string-obfus -fla -sub -bcf -const-obfus`）compile/link/run 全 rc=0，stdout 逐字节一致 `PORT_SMOKE_SECRET_MARKER_42=2681`；混淆版对象 5132B→196964B（38×），`EncryptedStringTable`/`dec*.str*` 解密器族仅存于混淆版（StringObfuscation 生效；基线明文缺席系 NeverC 内建 xorstr，属计划预警的功能重叠）。混淆版链接在 embed 重链后的编译器上耗时 525s（重负荷非挂死）。

**回归（check-neverc，10807 测试）**：移植树 70 失败 **全部在干净 dev 复现，0 个移植致因**——translate 族 55/55 定向复现；LinkerTest×8 在 dev 补 `neverc-embed-runtime-bitcode` 重链后复现（mimalloc 对 macho/nostdlib 目标默认注入，探针对象两树逐字节一致 390951B——NeverC 自身 bootstrap 生命周期缺陷）；LTOTest×3、I386×1、BuildBuiltin×2、PluginTargetSchemaDrift×1 均直接复现。裁决方法：定向 gtest 复跑 + 最小探针交叉（`--target=arm64-apple-macos13 -fno-lto -ffunction-sections -c`）。

**交付物**：`docs/neverc-obf-port.patch`（421 文件，7.3MB，dev 干净 worktree `git apply --check` rc=0）；源侧 worktree 归档提交 `dee2393b6ff4` + `69d0cf1f555b`（wt/llvm-msvc-port@feat/neverc-obf-port）。移植期修正：注册点从 neverc 驱动移至 `nevercEmit`（PRIVATE link-only 传播），CodePicPass pipeline-parse 回调改仅认 `"code-pic"`（源 fork 的吞名行为会破坏 neverc 的 parsePassPipeline 消费者）。
