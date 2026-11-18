# MicPatch 虚拟麦克风驱动（开发版）

驱动目标是 Windows 11 x64 的单一 WaveRT 录音端点，硬件 ID 为 `ROOT\MICPATCHMIC`。通话程序从标准 Windows 录音端读取；MicPatch 通过专用设备接口写入 48 kHz、单声道、PCM16 的最终混音。驱动不提供播放端点，也不依赖 VB-Audio。现有 VB-Audio 路径不受影响。

## 构建

使用具备内核工具集、配套 SDK 和 Inf2Cat 的 Visual Studio 2022 + WDK 环境。从仓库根目录运行：

```powershell
powershell -NoProfile -File .\driver\build.ps1 -Language zh-CN
```

也可用 `-Language en`。驱动包输出至 `driver/out/x64/zh-CN`（英文为 `driver/out/x64/en`）。命令加 `-PrepareOnly` 可只校验固定上游归档、应用补丁并生成项目和本地化 INF，不需要 WDK，也不会产出 `.sys`。构建基线固定为微软 [Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples/tree/d49bb7d72f3651952b2b6e4ad28d14fe2247eb69/audio/sysvad) 的 `d49bb7d72f3651952b2b6e4ad28d14fe2247eb69`；许可证见 [LICENSE](LICENSE)。普通应用构建不需要 WDK。

构建步骤不安装驱动或更改启动策略。输出是开发用驱动包，不代表具有生产签名。根枚举设备需要在隔离测试机上用受支持的方式创建设备实例，并使用符合该机签名策略的包；仅把 INF 暂存到驱动存储并不等于创建了设备。**不要为试用在日常使用的机器上改 Secure Boot 或启动签名设置。**

## 联调检查

1. 在隔离测试系统安装合规签名的驱动并创建设备实例。检查声音设置中只有 MicPatch 录音端，没有对应的 MicPatch 播放端。关闭应用时录音应为静音。
2. 刷新 MicPatch 设备，选择「麦克风（声搭） · 直接输出」，添加麦克风或文件后启动。通话软件里选择带 MicPatch 名称的输入。中文和英文系统的设备显示格式可能不同。
3. 可用应用工程生成的 `route_probe` 回采：

   ```powershell
   .\build\Release\route_probe.exe micpatch:virtual-mic 'MicPatch' 'D:\test\tone440.wav' --tone440
   ```

   `MicPatch` 必须唯一匹配录音端；无参数可先列出完整端点 ID。测试音应为持续 440 Hz。检查能收到音频、总静音后回采下降并在解除后恢复。驱动清空的是尚未交付的 PCM，已经进入 Windows 或通话客户端缓冲的帧不能撤回。
4. 发布前还须检查并记录：多个共享模式录音客户端、第二写入者被拒绝、畸形请求、写入进程退出或挂起后的静音、挂起恢复、启停和卸载，以及 Driver Verifier 与通话兼容性。本机未做这些内核或设备验证。

## 供数权限

协议定义见 [virtual_mic_protocol.h](../virtual_mic_protocol.h)。当前开发版的设备 ACL 允许 SYSTEM、管理员及经过身份验证的用户访问，并通过文件对象限制单一写入者；不检查进程名、签名或会话，拥有相同设备权限的其他程序也可能抢先打开接口。录音端使用标准 Windows 录音访问路径。
