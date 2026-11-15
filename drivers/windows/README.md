# WindowsiOS ARM64 GPU 驱动验证工程

**开发中，ARM64 WDK 编译已通过，尚未完成驱动签名或 Windows 加载验证，不是可安装的完整 3D 驱动。**

`kmdod/` 基于 Microsoft Windows-driver-samples 的显示专用 miniport 示例，保留微软原始源码声明和 [MS-PL 许可证](kmdod/LICENSE)。原始文件的 blob 信息见 `kmdod/UPSTREAM.json`。这些文件只用于独立 Windows 驱动，不编入 GPL 的 iOS/宿主程序。共享 ABI 和新增传输组件使用 MIT 许可，见 `../../gpu/LICENSE`。

本阶段增加：

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
