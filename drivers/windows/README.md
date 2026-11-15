# WindowsiOS ARM64 GPU 驱动验证工程

**开发中，ARM64 WDK 编译已通过。独立诊断驱动的嵌入测试签名、目录签名及目录成员哈希已验证，尚未在 Windows 加载，不提供 Direct3D 加速。**

`kmdod/` 基于 Microsoft Windows-driver-samples 的显示专用 miniport 示例，保留微软原始源码声明和 [MS-PL 许可证](kmdod/LICENSE)。原始文件的 blob 信息见 `kmdod/UPSTREAM.json`。这些文件只用于独立 Windows 驱动，不编入 GPL 的 iOS/宿主程序。共享 ABI 和新增传输组件使用 MIT 许可，见 `../../gpu/LICENSE`。

新增 `diagnostic/` 是独立 MIT 许可的 KMDF PCI 诊断驱动；`common/` 是不依赖显示驱动的传输实现。当前探针通过 SetupAPI / CreateFile / DeviceIoControl 查找该设备，不再依赖活动显示器或 POST 显示所有权。旧 `kmdod/` 仍作为显示驱动参考保留，但不加入新诊断安装包。

本阶段增加：

- `diagnostic/wingpudiag.inf` 作为系统类设备，仅匹配同一实验 PCI ID；通过功率管理队列串行执行固定 GPU 测试，并在 D0 切换时停用/恢复通路。设备接口限系统和管理员访问。
- `package-diagnostic.ps1` 只在临时 Windows 构建环境创建不可导出私钥的测试证书，嵌入签名后重建目录、再签目录并验证哈希成员关系；仅公开 CER，不能替代生产签名。
- 只绑定实验 `PCI\VEN_1B36&DEV_FF10` 的 INF，不接管其他厂商或所有显示类设备。
- `wingpu_transport.cxx` 映射实验设备 BAR、提交固定 GPU 测试批并读取完成结果；不会接收用户指定的任意内核地址。
- `probe/` 构建 ARM64 探针，只选择实验设备并触发固定离屏清屏/三角形测试，不修改显示模式、安装驱动或更改 Windows 启动选项。
- `build.ps1` 在已有受支持的 Visual Studio/WDK 环境中编译，不签名、安装或执行。
- `.github/workflows/windows-gpu.yml` 可在独立开发分支用 Windows runner 和固定版本 WDK/SDK 编译。

在配置好的 Windows Developer PowerShell 中：

```powershell
.\drivers\windows\build.ps1 -CheckOnly
.\drivers\windows\build.ps1
```

NuGet/云端编译使用微软当前示例的 SDK/WDK `10.0.28000.2526` 与 VS 2026 runner。已有本地安装版 WDK 可用匹配版本的 VS；示例显示 DDI 版本固定为 Windows 8 级别，避免将当前 WDK 最新渲染接口误宣称为本驱动已支持的接口。

完整 ARM64 内核驱动、INF/目录文件和 ARM64 探针已在 [Windows/WDK 构建](https://github.com/ZCloud211/WindowsiOS/actions/runs/43701541533)中生成。驱动和探针均为 0 警告、0 错误；WDK 接口验证和 INF/目录生成通过。已取回文件并核验 ZIP 哈希、PE ARM64 机器类型、驱动 Native 子系统和探针 Console 子系统。代码版本为 `95103d0d087c986da2d94230c54ec42a6ad44a92`。

驱动未签名，也未在 Windows 加载；这次编译通过不能证明 GPU 已能加速 Windows。继承的显示所有权、复位、电源/睡眠等行为仍需验证和适配，暂不提供安装步骤。

P0 诊断传输为了定位问题使用有超时的同步等待；最终渲染调度需要异步围栏、中断与多帧并行，不沿用此等待。超时仍有 GPU 工作时，诊断驱动保留少量完成页，防止迟到写入破坏被重新分配的客体内存；这也需要在最终复位协议中替换。

最终目标和验收要求见 [WINDOWS_GPU_PLAN.md](../../docs/WINDOWS_GPU_PLAN.md)。

2026-11-15，独立 KMDF 诊断驱动与静态运行库 ARM64 探针在 [构建 43705234338](https://github.com/ZCloud211/WindowsiOS/actions/runs/43705234338) 中通过，均为 0 警告、0 错误。源码提交 `1320050f287a310aa47dd86f3445200bc6915d91`。`Inf2Cat /os:10_GE_ARM64` 通过；嵌入签名和目录签名在临时构建机信任库中用 SignTool 验证成功。证书 SHA1 指纹 `D740D65EE80346F64319E1B461E357EE025555EE`；这是用于识别证书的指纹，不是包完整性 SHA256。产物 ZIP SHA256 为 `ce450eb9985099738b85b68d01923c79c5dee587d47ce34c0b4b3c564750d524`。Windows 加载和 GPU 命令执行仍待用户手动验证。测试步骤见 [diagnostic/TESTING.md](diagnostic/TESTING.md)。
