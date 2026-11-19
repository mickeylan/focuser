# 野草电调焦固件 V1.001 — 逆向分析产物

针对 `野草电调焦固件V1.001.hex`（STM32F103C8T6 电调焦固件）的静态逆向结果。
全部产物由本机 Ghidra 12.1.4 实跑生成，非推测。

## 1. 固件事实（从 HEX 与反编译得到）

| 项 | 值 | 来源 |
|---|---|---|
| 文件格式 | Intel HEX (I32HEX)，2537 条数据记录 | HEX 解析 |
| 区域 | 单一区域 `0x08000000–0x08009E87`，40584 字节 | HEX 解析 |
| flash SHA-256 | `29cce4d97f7f66c9cd194cd63d407bcea1f87cf20c6d132b075ec975a0fb22fb` | HEX 解析 |
| MCU | STM32F103C8T6（Cortex-M3 / ARMv7-M） | BOM + 59 项向量表 |
| 初始 SP | `0x200036D0`（RAM 0x20000000） | 向量表 +0x00 |
| Reset 向量 | `0x0800028D`（低位 1 = Thumb） | 向量表 +0x04 |
| 向量表 | 59 项（16 系统 + 43 IRQ），0x08000000–0x080000EB | 向量表 |
| 代码起点 | `0x080000EC`（59×4，与 HEX type-05 声明入口 0x080000ED 一致） | 向量表长度 |
| 导入语言 | `ARM:LE:32:Cortex`（Thumb，M-profile） | Ghidra headless 实测 |

### 外设与模块（从 `.symtab` 之外的固件内嵌证据）

| 模块 | 证据 | 说明 |
|---|---|---|
| USB CDC | `../Src/usbd_conf.c`、`CDC Config`、`STM32 Virtual ComPort`、`STMicroelectronics` | 设备枚举为虚拟串口 |
| USART3 | RCC APB1ENR bit18 (USART3EN)、`NVIC_EnableIRQ(0x27)`=IRQ39、`USART3_IRQHandler` @ `0x08005E84` | 命令接口（对应 BOM "串口插针"） |
| TIM | `../Src/tim.c`，两个定时器：PSC=71、Period=99、ARR=0x1000、AutoReloadPreload | 步进电机步进时序 |
| ADC | `../Src/adc.c`；固件内 `%.3fV` 格式串 | 供电电压采样（BOM 可选 10K/2K 分压） |
| I2C | `../Src/i2c.c` (`FUN_08004ad0`) | 0.96" OLED |
| 版本 | `V1.001` @ `0x08001A30`，由 `FUN_080019c4` 渲染到 OLED | 屏幕显示的版本号 |

> `../Src/*.c` 是 assert 宏里的 `__FILE__`，因此固件内嵌了源文件名，可据此划分模块边界。

### 已识别的中断处理函数（非默认处理器的条目）

`Reset_Handler` 0x0800028C、`NMI` 0x08004C98、`HardFault` 0x0800457C、`MemManage` 0x08004C94、
`BusFault` 0x08001788、`UsageFault` 0x08007AFC、`SVC` 0x080001F4、`DebugMon` 0x080019C0、
`PendSV` 0x08000238、`SysTick` 0x080056E0、`DMA1_Channel1` 0x08001964、`DMA1_Channel2` 0x08001974、
`DMA1_Channel3` 0x08001984、`USB_LP_CAN1_RX0` 0x08007A64、`TIM1_UP` 0x080057F0、`TIM3` 0x08005800、
`TIM4` 0x08005864、`USART3` 0x08005E84。其余均指向默认处理器 `0x080002A6`。

## 2. 产物清单

| 文件 | 说明 |
|---|---|
| `firmware.elf` | ARM ELF32/EM_ARM/ET_EXEC，entry=0x0800028C，`.vectors`(非执行) + `.text`(执行) + 20 个符号 |
| `firmware.bin` | 原始 flash 镜像（40 584 字节），供 Ghidra Raw Binary 导入，基址 0x08000000 |
| `make_elf.py` | 生成脚本（HEX → ELF/BIN/表），可改后重跑 |
| `vectors.tsv` | 向量表（地址/名称/向量值/模式） |
| `seeds.tsv` | `REA_GHIDRA_SEED_FILE` 格式的 function 种子 |
| `strings.txt` | 可打印字符串及地址 |
| `decompiled.txt` | **320 个函数的伪代码，8764 行，0 失败** |
| `ghidra_project/` | Ghidra 工程（可直接用 GUI 打开） |
| `ghidra_scripts/DumpFocus.java` | headless 反编译导出脚本 |

## 3. 在 Ghidra GUI 中继续分析

1. `D:\dev_tools\ghidra\ghidraRun.bat`（需 `JAVA_HOME=D:\Java\openjdk-23.0.2`）
2. File → Open Project → `ghidra_project\focus.gpr` → 打开 `firmware.elf`
3. 已是 `ARM:LE:32:Cortex` + Thumb 自动分析结果，可直接 Decompile
4. 若要从零重做：Import `firmware.elf`，Language 选 `ARM:LE:32:Cortex`，Compiler 选 `default`

## 4. 命令行重跑（headless）

```powershell
$env:JAVA_HOME = "D:\Java\openjdk-23.0.2"
& "D:\dev_tools\ghidra\support\analyzeHeadless.bat" "$env:TEMP\rea-focus\project" focus `
  -import "$env:TEMP\rea-focus\firmware.elf" -processor ARM:LE:32:Cortex -cspec default `
  -scriptPath "$env:TEMP\rea-focus\scripts" -postScript DumpFocus.java "$env:TEMP\rea-focus\decompiled.txt"
```

> 注意：Ghidra 的批处理在中文路径下会乱码，工程/脚本请放在纯 ASCII 目录。

## 5. 下一步建议

1. **协议还原**：上位机 `RoboFocus(3.0.9)Setup.exe` 是 x86 PE，Windows 上 REA 的 Ghidra 通道
   支持 PE（`docs/providers/windows-ghidra-p0.md`）。抓它的串口收发字符串，
   与固件 `USART3_IRQHandler → FUN_08004120 / FUN_08005AE4` 的接收帧格式对照，即可还原命令集。
2. **协议入口**：`FUN_08004120` 是 HAL 的 `HAL_UART_IRQHandler`，真正的应用层接收处理是
   `FUN_08005ae4`（由 `USART3_IRQHandler` 调用）。已确认的传输层事实：USART3 收发走 DMA
   （RX=DMA1 Ch5、TX=DMA1 Ch4），逐字节接收、靠**空闲线**分帧，每收到一帧回发**固定 50 字节**
   （`FUN_080043dc(huart, buf, 0x32)`）。命令集与 50 字节帧的字段含义**尚未解码**，详见
   `docs/RECONSTRUCTION.md` 第 4 节。
3. **电机时序**：`FUN_08004B20` / `FUN_08004B98`（tim.c）两个定时器实例，ARR=0x1000，
   与 `TMC2225` 的 STEP/DIR 模式配合；改 ARR 即可调对焦速度。
4. **电压显示**：`%.3fV` 由 ADC 通道换算，采样电阻 10K/2K 未焊时无电压显示（见 BOM 说明）。

## 6. 已知限制

- 固件无符号表与调试信息，函数名除向量表外均为 `FUN_xxxxxxxx`；本次仅靠 `.symtab`
  补齐了 19 个向量处理函数名。
- 无 `$t`/`$a` mapping symbols，Thumb/ARM 边界无法由符号判定；本固件为 Cortex-M3
  （仅 Thumb），使用 `ARM:LE:32:Cortex` 全量按 Thumb 解码是正确选择。
- 数据区（.rodata/.data 混在 flash 内）会被自动分析当作代码，可能产生伪函数；
  以向量表与调用图为准判断真实代码。
- `%.3fV` 等字符串仅被 `FUN_08004e70/08004ec4` 间接引用，反编译器未还原为字面量比较。

## 7. 源码还原与协议扩展

反编译产物之外，提供了可在主机测试的 C 状态机/FP1 协议、STM32F103 板级适配器和两种主机接入实现。
**不是可直接烧录的完整 CubeMX 工程，也未认证任何实际天文客户端的兼容性。**

| 路径 | 内容与验证边界 |
|---|---|
| `src/port/focus_port.h` | 设备操作接口；运动状态不再由步频猜测 |
| `src/protocol/focus_protocol.{h,c}` | FP1 解析器；显式 int32 参数范围 |
| `src/recovered/focus_motor.{h,c}` | 连续/计数状态机；停止、零步、计数边界回归 |
| `src/recovered/board_stm32f1.{h,c}` | UART RX 挂接/重挂、USB 输入适配、前台解析、有界队列；fake HAL 编译及集成测试 |
| `host/focus_bridge.py` | Alpaca IFocuserV2 标准 GET/PUT、事务/错误包装、management 与 UDP 发现 |
| `host/AscomFocuser/` | 自包含 ASCOM V2/V3/V4 托管实现；离线测试不注册 COM、不访问真实串口 |
| `test/run_offline.ps1` | 全部离线测试入口（Clang、Python、.NET Framework 4.7.2/ASCOM 程序集） |
| `docs/RECONSTRUCTION.md` | 证据、平台集成契约、协议及验证边界 |
| `docs/REVIEW_FIXES.md` | 上轮 review 问题逐项处理和验证记录 |

新协议 FP1：`PING / VER? / POS? / STATUS? / VOLT? / MOVE / GOTO / RATE / RATE? / HALT`。
位置是**用户手动确认零位后的步数坐标**。新增“两点行程标定”：设零→电机点动→记录最大位，
固件统一软限位；上电未确认零位时禁止普通移动。无开关、编码器或自动 homing，不能检测丢步/手拧。
最大行程持久化通过平台存储 hook，仍需真实 Flash/EEPROM 驱动；不持久化可信当前位置。
按钮工具：`python host/focus_calibrate.py --port COM7`，详见 [标定操作与集成边界](docs/CALIBRATION.md)。
未知物理 `StepSize` 明确报未实现，不再编造 1 微米。

本轮离线验证：C 电机 18 项、协议响应/边界回归、板级生产路径/两点标定断言、Python 21 项、
Python→真实 C 协议/电机 3 项、ASCOM 37 项（含 V4 异步连接），均通过。另以 WSL GCC 验证了 LP64 参数范围。
**仍未验证**：真实 STM32 HAL 构建/链接、引脚和脉宽、USB/串口实板、COM 注册和实际 NINA 等客户端。
平台必须按 `board_stm32f1.h` 提供初始化、共享 HAL handles、USB 回传及行程存储 hook；不可直接烧录当前树。

```powershell
# 在此目录运行；默认复用 bin/Release 下已有 ASCOM 依赖，也可显式指定依赖目录
.\test\run_offline.ps1 -AscomDir "C:\path\to\ascom\assemblies"
```

