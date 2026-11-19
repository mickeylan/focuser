# 上轮电调焦 review 修复记录

范围：`src/`、`host/`、`test/` 及对应文档。没有提交、推送、注册/注销 COM、
写注册表、枚举/打开真实串口或操作真实硬件。J 盘 REA 工具仓库只读检索，没有修改。

## 问题 → 修复 → 证据

| review 问题 | 处理 | 离线证据 |
|---|---|---|
| `&focus_device()` 非法取地址；私有 HAL handles 与生成工程脱节 | 正确传 `FocusDevice*`；使用平台共享 handles；明确外部初始化/USB hook 契约 | `test_focus_board.c` 编译并链接真实适配器，`main_recovered.c` 语法检查 |
| USART3 未启动接收/无完成回调/重挂 | 启动单字节接收、完成回调重挂、错误恢复；USB 有独立入口供生成回调调用 | 板级测试观察 207 次 RX 挂接；错误后整行丢弃 |
| ISR 内解析/阻塞发送，串口与 USB 可混合输入 | 各自有界整行队列，前台 `focus_board_tick` 解析和发送，回复跟随来源 | fake HAL 断言禁止 ISR 内 TX/ADC；独立分帧、队列溢出回归 |
| `moving=rate>0`，替身掩盖生产缺陷 | 增加必需 `is_moving` 回调；电机 direction 为运动状态权威；HALT 保留配置速度 | 板级运行开始/完成/HALT；Python→生产 C 模块→HTTP 回归 |
| STOP/remaining=0 仍发 STEP、零步抖动 | 所有停止/零步/完成边界不发脉冲，完成清运动状态；零步取消在途运动 | 电机 18 项，板级确认完成后无额外脉冲 |
| GOTO 差值/位置溢出；GOTO 当前不取消旧运动 | int64 中间值，范围检查后替换；位置边界停止且不发 STEP | 电机与板级 INT32 极值、零目标替换回归 |
| 协议 long→int32 截断 | 在 strtol 后统一限制 INT32 范围，不依赖主机 long 位宽 | Windows Clang 与 WSL GCC/LP64 均通过 |
| 定时器忽略 PSC、16 位 ARR 截断 | 使用 APB1 实际时钟/倍频及 Prescaler；拒绝不可表示速率；使用真实 update callback | fake HAL PSC=71/PCLK1=36MHz，100Hz→ARR9999；低/高速拒绝；非 update IRQ 不发 STEP |
| ASCOM write/read 并发错应答 | FP1 整个事务加锁，驱动会话统一协调命令/连接/断开 | 320 个驱动+120 个直接客户端并发事务，返回值逐次检查 |
| 握手失败遗失资源；超时/SocketException 逃逸 | 工厂异常关闭资源，连接成功才移交所有权；映射为 ASCOM 错误；超时毒化并关闭流 | 假 TCP 设备观测 EOF，读超时/拒绝连接/ERR 握手回归 |
| 自动探测使用错误波特率，访问其它串口 | 删除探测路径；必须显式 Port/TcpEndpoint，显式串口使用配置 Baud | 空配置连接失败，不枚举真实串口 |
| V4 声明返回版本2 | 返回4，并补齐官方规定的非阻塞 Connect/Disconnect、Connecting 错误/进度、在途 Dispose 清理 | V2/V3/V4 托管绑定；阻塞假握手验证 Connect 立即返回；异步失败及晚到资源回归 |
| Move 不检查自身范围；编造 StepSize | 0..MaxStep 先验验证；未知物理步长抛未实现；Position setter 复用 Move | 两端点通过，越界不产生 GOTO，未知步长异常 |
| 注册失败只 Trace 仍假成功 | 注册/注销错误显式抛异常 | 源码检查；**没有运行注册/注销或 Profile API** |
| Alpaca 实际是私有 JSON/POST | 标准成员路由与 PUT 表单；共同事务/错误外壳；独立 position/ismoving/move/halt/connected 等 | HTTP 真服务器回归；旧私有接口/错误设备号/错方法拒绝 |
| 丢 POST body、错误吞为默认位置0/移动成功 | 移除旧接口；PUT body 严格解析、大小写不敏感、重复参数拒绝；FP1 错误与不完整状态拒绝 | HTTP/FP1 错误、数值/事务边界、并发事务 ID 回归 |
| management/UDP 发现格式错误、端口0广播0 | 标准 management 路径、有效设备列表；发现只回复实际 AlpacaPort | 本机真实 UDP/HTTP，ephemeral port 断言及非法探测报文拒绝 |
| 测试固化错误版本/步长/私有接口/替身清rate | 按官方定义修正错误期望；增加板级+真实 C 跨语言测试，不再以局部 mock 宣称集成 | 全套入口及独立 LP64 构建通过 |
| 客户端、板级“已经可用”的过度声明 | README、RECONSTRUCTION、ASCOM README 按已验证/未验证分开 | 文档边界断言；下节明确剩余集成条件 |

原桥接的 CurrentStep/TargetStep/Enabled 不是 IFocuser 标准成员，已随旧私有 API 移除，
不再以“每次查询把 TargetStep 改成当前位置”来猜测标准客户端语义。
Python/C# 各自保留独立 FP1 客户端，不引入强制桥接依赖；两者都拒绝 ERR/缺失 STATUS 字段。

## 语义澄清与未完成的平台集成

1. `Absolute=true` 表示按指定步数坐标寻址，不承诺跨上电保留机械坐标。当前零点仍是上电原点，
   文档明确无 homing、限位和掉电存储。未用伪造持久位置“修复”未知物理原点。
2. 正步频只是配置，不是运动；HALT 和移动完成保留速度。板级 `RATE 0` 停止禁用，
   必须设回正速率才接受非零移动。模块 API 没有设置定时器的能力，由板级统一执行。
3. **板级适配器不是完整可烧录工程。** 未提供真实 CubeMX 生成配置与 HAL/USB 库，
   `focus_board_platform_init`、真实共享 handles、`focus_board_usb_send` 必须由平台实现。
   USB 生成接收回调须显式转接。不能把 fake HAL 编译通过称作真实 HAL 构建通过。
4. PA8/PA9、脉宽、DIR 建立时间、驱动电流/使能、原厂 TRGO/OC 配置仍未实板验证。
   新 FP1 适配器用 base update 定时器/前台发送，不宣称完整复现原厂 DMA/IDLE 50 字节协议。
5. 未测试 COM 注册、ASCOM Conform、NINA/Stellarium/SGP/Voyager，也没有进行用户设备连接。
   客户端/硬件兼容结论仍须在单独授权后验证。

## 验证命令与实际结果

项目根目录 PowerShell：

```powershell
.\test\run_offline.ps1
# 或显式依赖目录（只用已有程序集，不安装新依赖）：
.\test\run_offline.ps1 -AscomDir "C:\path\to\ascom\assemblies"
```

全套入口执行结果：退出码 0，`ALL OFFLINE SUITES PASSED`。

- C 电机：18 项检查通过。
- C 协议：26 个响应比较及位置检查通过；Clang Windows 与 WSL GCC/LP64 均退出 0。
- 板级：真实适配器+协议+电机配 test-only HAL，207 次 RX 挂接、2 个 STEP，所有 assert 通过。
- Python HTTP/FP1/UDP：19 项通过。
- Python→生产 C 协议/电机：3 项通过；`@TICK` 仅存在测试设备中，不是生产 FP1 命令。
- .NET：net472 构建 0 警告/0 错误；37 项离线检查通过（含 440 个并发事务）。

LP64 补充命令（在 WSL 的项目目录中）：

```sh
gcc -std=c99 -Wall -Wextra -Wpedantic -O1 -o test/fp_test_linux \
    test/test_focus_protocol.c src/protocol/focus_protocol.c
./test/fp_test_linux
```

标准依据（用于路由/响应/能力语义核对，并非认证结果）：

- https://ascom-standards.org/api/AlpacaDeviceAPI_v1.yaml
- https://ascom-standards.org/api/AlpacaManagementAPI_v1.yaml
- https://ascom-standards.org/newdocs/focuser.html
