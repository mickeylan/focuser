# 固件还原与协议扩展

本文件记录从 `野草电调焦固件V1.001.hex` 还原出的固件行为、重建出的 C 源码，以及新增的
FP1 协议与主机端桥接。每一处都标注了证据等级：

| 标记 | 含义 |
|---|---|
| **已确认** | 由反编译代码或 Ghidra 反汇编直接读出 |
| **推断** | 由已确认的行为 + STM32 参考手册推导，原源码不可得 |
| **新增** | 本次重写所写，原固件没有对应实现 |

## 1. 目录

| 路径 | 内容 | 可在本机编译 |
|---|---|---|
| `src/port/focus_port.h` | 设备操作接口（协议层与硬件解耦） | 是 |
| `src/protocol/focus_protocol.{h,c}` | **新增** FP1 协议解析器（表驱动） | 是（已测） |
| `src/recovered/focus_motor.{h,c}` | **还原** 电机状态机（连续/计数两种模式） | 是（已测） |
| `src/recovered/board_stm32f1.{h,c}` | STM32F103 适配器，平台初始化/USB hook 由外部实现 | fake HAL 编译/集成通过；非实板构建 |
| `src/recovered/main_recovered.c` | **还原** main()（原为空循环） | 否（需 CubeIDE） |
| `host/focus_bridge.py` | **新增** FP1 → ASCOM Alpaca 桥接（含 Alpaca 发现广播） | 是（已测） |
| `host/test_focus_bridge.py` | 标准 HTTP/发现/FP1 回归（19 项） | 是 |
| `host/AscomFocuser/REAFocusDriver.cs` | **新增** 自研 ASCOM 驱动（IFocuserV2/V3/V4，net472） | 是（已测） |
| `host/AscomFocuser/REAFocusDriverTest.cs` | 离线托管驱动测试（37 项，假 TCP 设备，无 COM/注册表） | 是 |
| `test/test_focus_protocol.c` | 协议响应、分帧与 int32 边界回归 | 是（Clang/WSL GCC） |
| `test/test_focus_motor.c` | 电机行为/脉冲/位置边界（18 项） | 是（已测） |
| `test/test_focus_board.c` | 真实板级适配器+协议+电机，fake HAL | 是（已测） |
| `host/test_firmware_bridge.py` | HTTP/Python→真实 C 协议/电机（3 项） | 是（已测） |

## 2. 还原映射表

下表保留初版重建的原函数映射（包括 `MX_*` 名称），不是当前树提供完整初始化实现的声明。
当前初始化统一由外部 `focus_board_platform_init()` 承接；ADC/版本/脉冲适配函数现为
`voltage()` / `version()` / `pulse()`。原 HAL/USB/CubeMX 工程未交付。

| 原函数（地址） | 原语义 | 还原到 | 证据等级 |
|---|---|---|---|
| `FUN_08007b58` @0x08007b58 | main：HAL/时钟/USART3/I2C/ADC/TIM×2 初始化后进入空循环 | `main_recovered.c` → `focus_board_init()` | 已确认 |
| `FUN_080042e0` / `FUN_08004c14` | usart.c：USART3 初始化（IRQ39，RCC bit0x40000） | `MX_USART3_UART_Init()` + `USART3_IRQHandler` | 已确认 |
| `FUN_08004ad0` | i2c.c：OLED（I2C1）初始化 | `MX_I2C1_Init()` | 已确认 |
| `FUN_08004890` / `FUN_08001e1c` | adc.c：ADC 通道配置与采样 | `MX_ADC1_Init()` / `board_voltage_mv()` | 已确认（换算比推断） |
| `FUN_08004b20` @0x08004b20 | tim.c：定时器 A 初始化（预分频 71、比较 99、ARR 预装载关） | `MX_TIM3_Init()` | 已确认（字段语义推断） |
| `FUN_08004b98` @0x08004b98 | tim.c：定时器 B 初始化（同参数、ARR 预装载开 0x80） | `MX_TIM4_Init()` | 已确认（字段语义推断） |
| `TIM3_IRQHandler` @0x08005800 | 连续步进：方向 ∈{-1,0,+1}，位置 ±1，STEP 拉高→短延时→拉低 | `focus_motor_tick_continuous()` + `focus_step_pulse()` | 已确认 |
| `TIM4_IRQHandler` @0x08005864 | 计数步进：剩余步数符号表方向，计数到 0 自动停表 | `focus_motor_tick_counted()` | 已确认 |
| `FUN_08003c80` | TIM2 作为主时钟向 TIM3/TIM4 输出 TRGO | `MX_TIM3_Init()` 内部主从配置 | 已确认 |
| `FUN_08003d52` | 通道配置：OC 模式 0x1000（关闭从模式）+ CCR | `MX_TIM*_Init()` | 已确认 |
| `FUN_080019c4` | **不是**协议应答：在 OLED 上渲染 `V1.001`（8×16 点阵字体 + 6px 比例字体） | `board_version()`（版本字符串常量） | 已确认（此前误判为"应答组包"，已更正） |
| `FUN_08006410` | USB CDC 类请求处理（厂商栈） | 未重写（标准 USB 栈） | 已确认 |
| `FUN_08006830` | usbd_conf.c：USB 设备初始化 | `MX_USB_DEVICE_Init()` | 已确认 |

**未还原**：STM32 HAL 与 USB 设备栈（标准库代码，无业务价值）、OLED 字库与页面绘制
（`i2c.c` 调用链）、浮点电压格式化函数（`%.3fV` 的 printf 包装）。

## 3. 原固件已确认的运行机制（不是新适配器的逐项实现声明）

- **中断驱动**：main 是空循环，所有工作在 TIM3/TIM4（步进）、USART3（命令）、
  USB CDC（命令）中断里完成。
- **步进时序**：每个定时器中断产生一个完整 STEP 脉冲（引脚 bit8 → 例如 PA8）：
  拉高 → 约 22 次空循环延时 → 拉低。TMC2225 走 STEP/DIR 模式，DIR 由方向变量决定。
- **两种运动模式**：TIM3 = 连续（方向变量控制），TIM4 = 计数移动（走完自动停表）。
- **主从定时器**：TIM2 作为主定时器给 TIM3/TIM4 提供 TRGO，步进频率由主定时器决定。
- **电压采样**：ADC 通道读原始值后按分压比换算成毫伏（板载可选 10K/2K 分压，
  比例 6× 为**推断**，原代码的换算常数尚未定位）。

### 3.1 新适配器的平台契约与差异

- 不提供完整可烧录工程，不猜测 CubeMX 时钟/引脚/USB 生成代码。平台必须实现
  `focus_board_platform_init()`，并导出实际初始化的 `huart3/hadc1/htim3/htim4` handles；
  旧版私有静态 handles 与生成代码脱节的问题已移除。
- FP1 仅暴露计数运动，使用 TIM4 **base update** 回调发 STEP，不再在任意 TIM IRQ 后发脉冲。
  TIM3/4 的实际 APB1 定时器时钟和 Prescaler 用于计算 ARR，拒绝超过 16 位范围的周期，
  不是原厂 TIM2/TRGO/OC 链路的完整复刻。配置 ARR preload 关闭，避免写入速率延迟生效。
- 初始步频 250；`RATE n>0` 设置速度而不启动运动。`HALT`/完成运动保留速度；`RATE 0`
  停止并禁用运动，必须重新设正步频才能移动。过低/过高、无法表示的步频回 `ERR STATE`，不截断。
- UART 使用单字节 `HAL_UART_Receive_IT`，完成回调重新挂接，错误时丢弃当前整行并重试接收。
  USB 的 `CDC_Receive_FS` 必须由生成项目显式调用 `focus_board_usb_receive()` 并完成 USB 重挂。
- 两个入口有各自的有界完整行队列；中断只收字节，`focus_board_tick()` 前台执行命令、ADC 与回传。
  溢出整行丢弃，回复 `ERR LINE`；不会执行被截断的 MOVE。回复只走请求所属通道。
- 平台实现 `focus_board_usb_send()`，在返回前复制/发送完数据，不能把临时缓冲指针留给 DMA/USB。
  UART 阻塞发送仅在前台运行。这是 FP1 的新增集成路径，不是已恢复原厂 DMA 发送的声明。
- PA8/PA9 与延时 22 次循环仍是重建假设，不能据此驱动真实电机。必须人工核对 PCB、脉宽、
  DIR 建立时间、驱动使能/电流、机械限位后，另行完成真实 HAL 构建与板卡验证。
- HAL callback/IRQ 每种只能有一个定义，生成项目须转接而不是重复定义。详见 `board_stm32f1.h`。

## 4. 原厂协议：传输层已确认，命令集未解码

这一节记录对**原固件自带的串口协议**的逆向结果（不是新写的 FP1）。

| 结论 | 证据 | 等级 |
|---|---|---|
| USART3 收发都走 **DMA**：RX = DMA1 通道 5，TX = DMA1 通道 4 | `FUN_080042e0`：两个 DMA handle 的 `Instance` 相差 0x14（相邻通道），`hdmarx`/`hdmatx` 挂在 UART handle 的 0x30/0x34 | 已确认 |
| 接收是**逐字节**：`HAL_UART_Receive_IT(huart, buf, 1)`，收到一字节就回一帧 | `FUN_08005ae4` 末尾调用 `FUN_0800403a(huart)` 重装接收 | 已确认 |
| **分帧靠空闲线**（IDLE，SR bit4），不是长度前缀 | `FUN_08005ae4` 的条件 `SR << 0x1b < 0` 即 32 位视角的 bit4 | 已确认 |
| 每次收到一帧后，**回发固定 50 字节** | `FUN_080043dc(huart, buf, 0x32)`，0x32=50；该函数即 DMA 发送（`FUN_08002334` = `HAL_DMA_Start_IT`） | 已确认 |
| 上电时也发同样的 50 字节帧（握手/自报） | main 的 `FUN_08007b58` 里 `FUN_080043dc(huart, buf, 0x32)` | 已确认 |
| NVIC：USART3 = IRQ39，优先级 5 | `FUN_08002c22(0x27,5,0)` / `FUN_08002c08(0x27)` | 已确认 |
| **命令字节落在缓冲区尾字节（偏移 0x30）**，回帧头 0..0x2F 由别处填充 | 只有 `FUN_08005ae4` 引用该缓冲区全局 | 推断 |
| 命令集与 50 字节帧的字段含义 | **未解码** | 未知 |

**为什么停在这里**：帧头的填充函数是通过**别名全局指针**访问的（Ghidra 明确提示
"Globals starting with '_' overlap smaller symbols at the same address"），要确定字段布局，
需要把整个 app 层调用图和 `.rodata` 常量表一起解出来，成本远超本次预算。

**可行的两条路**（二选一，都比继续逆向便宜）：
1. **抓包**：用 USB 串口助手或 Python 脚本录下原厂 PC 软件与设备之间的字节流，
   按"1 字节请求 → 50 字节应答"的节奏对齐，即可反推字段表；
2. **新固件不必兼容**：新增 FP1 + Alpaca 是现代客户端接入目标，但尚未完成客户端实测；
   原厂协议只在需要继续使用原厂 PC 软件时才有价值。

## 5. 新增：FP1 协议

行文本协议，命令一行一条（`\n` 结束，`\r` 忽略），应答一行以 `\r\n` 结束。
解析器是表驱动的（`src/protocol/focus_protocol.c` 的 `fp_commands[]`），
加第三方命令集只需加一条表项。

| 命令 | 应答 | 说明 |
|---|---|---|
| `PING` | `PONG <model> <version>` | 连通性 |
| `VER?` / `VERSION?` | `VER 1.001` | 版本 |
| `POS?` / `POSITION?` | `POS <steps>` | 当前步数位置 |
| `STATUS?` | `STATUS pos=… rate=… volt=… moving=0/1` | moving 由电机方向/活动状态提供，与步频独立 |
| `VOLT?` | `VOLT <mV>` / `ERR STATE` | 供电电压 |
| `MOVE <n>` / `STEP <n>` | `OK` | 相对移动（负数为内收） |
| `GOTO <n>` | `OK` | 本次上电坐标；到当前位置取消旧运动，无脉冲 |
| `RATE <n>` | `OK` / `ERR STATE` | 设置步频；0 停止禁用；超出定时器能力报错 |
| `RATE?` | `RATE <n>` | 读步频 |
| `HALT` / `STOP` | `OK` | 停机，保留正步频配置 |
| `ZERO` / `CALBEGIN` / `JOG n` / `MAXSET` / `LIMIT?` | `OK` / `LIMIT state=… max=…` / 错误 | DIY手动两点标定，详见 [CALIBRATION.md](CALIBRATION.md) |
| `HELP` | 命令列表 | |
| 其它 | `ERR UNKNOWN` | |
| 参数非法 | `ERR RANGE` | |
| 超长行 | `ERR LINE` | 整行丢弃，不执行半截命令 |

### 5.1 DIY 两点标定（后续新增）

普通 `MOVE/STEP/GOTO` 必须在本次上电确认零位、且已有最大行程后执行，固件强制检查两端。
人工标定只允许有界低速 `JOG`，不是自动寻找机械尽头。重启只加载最大行程，重新确认零位前拒绝移动。
新增 `focus_board_load_max/save_max` 平台持久化契约，真实存储实现仍未提供/验证，不能猜Flash存储区域。
独立按钮工具 `host/focus_calibrate.py`；详见 [操作及验证边界](CALIBRATION.md)。

下文早期review矩阵保留当时证据；当前Python21项、板级含两点标定（478次RX、11个STEP），全套离线回归通过。

## 6. 新增：主机端桥接（Alpaca 生态）

`host/focus_bridge.py` 把 FP1 转成 **ASCOM Alpaca** HTTP/JSON 接口：

```
GET /api/v1/focuser/0/connected
PUT /api/v1/focuser/0/connected  (form body: Connected=true)
GET /api/v1/focuser/0/position
GET /api/v1/focuser/0/ismoving
PUT /api/v1/focuser/0/move       (form body: Position=1234&ClientTransactionID=1)
PUT /api/v1/focuser/0/halt
GET /management/apiversions
GET /management/v1/description
GET /management/v1/configureddevices
```

用法：

```bash
python host/focus_bridge.py --port COM7 --alpaca-port 8090 --discovery
```

- GET 响应有 `Value`，PUT 成功不带 `Value`；均包装 `ClientTransactionID`、`ServerTransactionID`、
  `ErrorNumber`、`ErrorMessage`。设备错误不会变成位置 0 或成功移动。旧版私有 POST/action 接口已删除。
- 参数名大小写不敏感，重复/非法参数拒绝；路径与设备编号严格匹配。设备接口版本为 2；
  `StepSize`/`Temperature` 未知，返回未实现；`MaxStep`/`MaxIncrement` 默认 60000，可用 `--max-step` 配置。
- `--discovery` 回应 UDP 3220 的 `alpacadiscovery1`，只回复实际绑定的 `AlpacaPort`，含请求端口 0 的场景。
  默认仅绑定 loopback；跨机器发现需显式 `--host 0.0.0.0`，仅在可信网络使用（无鉴权）。
- `Connected=false` 是桥接服务的逻辑连接状态，不销毁服务拥有的物理传输；退出服务统一关闭传输。
  超时后关闭 FP1 链路，防止迟到应答串错；重连需要重启服务。不会自动扫描或打开其它串口。
- 串口需显式 `--port COM7`；TCP 用 `--tcp HOST:PORT`，两者互斥。真实 USB/USART3 仍未验证。

### 6.1 客户端接入（N.I.N.A. 等）

| 客户端 | 做法 |
|---|---|
| N.I.N.A.、Stellarium、SGP、Voyager | 仅为接入目标；本轮未运行这些客户端，不保证特定版本有 Alpaca/调焦菜单 |
| ASCOM COM 客户端/选择器 | 需要用户后续安装平台并注册驱动；本轮未注册、未验证 COM 激活 |
| Alpaca 客户端 | 本轮只验证标准路由、错误包装和本机 UDP 发现，不等同客户端兼容认证 |

### 6.2 关于"振旺盒子"（ZWO EAF）

- **振旺 = 苏州振旺光电 = ZWO**，他们的电动调焦产品是 **EAF**（红色小盒子）。EAF 正是
  通过 ASCOM 生态接入 N.I.N.A. 的，所以"支持振旺盒子"在客户端效果上等于"支持 ASCOM/Alpaca"，
  本项目实现的是独立的 ASCOM/Alpaca 接入目标，不代表与 EAF 软件兼容。
- EAF 的 **USB 报文格式没有公开**：公开资料只有官方 **C++ SDK**（`EAF_focuser` DLL）的 API
  （`EAFMove` / `EAFGetPosition` / `EAFStop` / `EAFIsMoving` / `EAFGetTemp` / `EAFSetMaxStep` /
  `EAFSetBacklash` / `EAFSetReverse` / `EAFGetSerialNumber` …），社区的 `zwoeafpy`、
  `py-zwo-eaf`、`pyobs-zwoeaf` 全都是对这个 DLL 的绑定，没有独立的命令表。
- ZWO 的 ASCOM 驱动还绑定了 **ZWO 的 USB VID/PID**。因此本项目**不做 EAF 线协议克隆**：
  一是闭源逆向，二是设备身份冒用。若确实需要 ZWO 自家软件（ZWO EAF App、ASIAIR 内置 EAF
  支持）驱动我们的盒子，合规路径是**向 ZWO 申请 SDK 授权**。

### 6.3 自研 ASCOM 驱动（`host/AscomFocuser/`）

除了 Alpaca 桥接，我们还提供**自己的 ASCOM 驱动**：不依赖任何桥接进程，驱动直接用
FP1 与盒子对话。

| 项 | 值 |
|---|---|
| 实现的接口 | `ASCOM.DeviceInterface.IFocuserV2` / `IFocuserV3` / `IFocuserV4`（一个类同时实现三个 IID） |
| ProgID / CLSID | `REAFocus.Focuser` / `{7B2A5A64-0E0F-4C6E-9F2C-2E8B4A1D6C31}` |
| 传输 | 显式配置串口（默认波特率 115200）或 TCP；禁止自动探测其它设备 |
| 注册 | 脚本供后续人工使用；注册失败现在抛异常，不再仅 Trace 后假成功。本轮未注册/注销 |
| 设置 | `HKCU\Software\REA\Focuser`：`Port` / `Baud` / `TcpEndpoint` / `MaxStep` |

接口选型依据（**查证结论**，不是猜测）：

- ASCOM Platform 6.5 / 7（`ASCOM.DeviceInterfaces.dll` 6.0.0.0 / 7.1.2）里的调焦接口是
  `ASCOM.DeviceInterface.IFocuserV2/V3/V4`；官方 C# 驱动模板（Platform 6.5 SP1 开发者包内）
  引用的正是这个程序集。
- 经典的 `ASCOM.Interfaces.IFocuser2`（CurrentStep/TargetStep/Enabled）**已不在免费
  Platform 里**：Platform 6.5 SP1 全包内检索不到该接口，NuGet 上也没有对应程序集。
  因此我们实现的是当前平台真正认的接口；只认老接口的软件走 6.1 的 Alpaca 路径。
- ZWO EAF 的 ASCOM 驱动同样是平台上的一个驱动，说明"盒子以 ASCOM 出现"就是行业惯例。

验证：测试直接创建托管驱动对象，通过显式配置注入假 TCP 设备，不访问注册表/COM/串口。
37 项检查覆盖 V4 版本、范围与未知步长、440 个并发事务、握手失败/超时/拒绝连接及句柄释放。
V4 `Connect/Disconnect` 非阻塞，`Connecting` 报告进度及异步失败；`Dispose` 等待在途连接并释放晚到资源。
旧版 `Connected` 写入仍同步，但拒绝与在途异步连接交叉执行。
`DeviceState` 从一个 STATUS 快照投影，不再进行两个可能不一致的设备往返。
`Absolute=true` 表示按步数坐标寻址，不保证跨上电的物理绝对位置；没有新增自动 homing/当前位置掉电恢复；后续两点标定只保存最大行程（需平台存储 hook）。

## 7. 验证

| 检查 | 结果 |
|---|---|
| `test/test_focus_protocol.c` | Clang/Windows 和 WSL GCC/LP64 均通过 |
| `test/test_focus_motor.c` | 18 项通过 |
| `test/test_focus_board.c` | 真实板级+协议+电机配 fake HAL，断言通过（207 次 RX 挂接，2 个 STEP） |
| `host/test_focus_bridge.py` | 19 项通过 |
| `host/test_firmware_bridge.py` | 3 项跨语言测试通过（生产 C 模块，测试时钟） |
| `host/AscomFocuser/REAFocusDriverTest.exe` | net472 构建 0 警告/0 错误，37 项离线检查通过 |
| 真实 HAL 构建/链接、实板/脉宽、COM 注册、NINA 等真实客户端 | 未执行，不能以主机测试替代 |

全套离线入口：在项目根目录执行 `./test/run_offline.ps1`，可用 `-AscomDir` 指定已安装的依赖目录。
脚本不安装依赖、不注册 COM、不连接真实串口；每个子进程非零退出都立即失败。

WSL/GCC 协议构建（需随后执行生成的 `fp_test`）：

```bash
gcc -std=c99 -Wall -Wextra -Wpedantic -O1 -o fp_test \
    test/test_focus_protocol.c src/protocol/focus_protocol.c
```

## 8. 状态与后续

| 事项 | 结论 |
|---|---|
| ASCOM | 托管接口和 TCP 假设备回归通过；真实 COM/平台兼容待验证 |
| 振旺（ZWO EAF）协议 | 不做克隆；独立 ASCOM/Alpaca 路径不代表 EAF 软件兼容 |
| N.I.N.A. 等客户端 | 接入目标，未做实际兼容性测试 |
| MCU | STM32F103 适配器；仍需平台实现、真实 HAL 工程、引脚与时序验证 |
| 原厂串口协议 | 传输层已确认、命令集未解码（见第 4 节） |

