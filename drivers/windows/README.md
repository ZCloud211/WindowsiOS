# WindowsiOS ARM64 GPU 驱动验证工程

**开发中，尚未完成 WDK 编译或加载验证，不是可安装的完整 3D 驱动。**

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

只有命令编码器在 Mac 上通过 Windows ARM64 COFF 交叉编译；完整 `.sys` 依赖 WDK，不能由 Xcode 的成功构建推断。驱动继承的显示所有权、复位、电源/睡眠等行为还需要验证和适配，暂不提供安装步骤。

P0 诊断传输为了定位问题使用有超时的同步等待；最终渲染调度需要异步围栏、中断与多帧并行，不沿用此等待。超时仍有 GPU 工作时，诊断驱动保留少量完成页，防止迟到写入破坏被重新分配的客体内存；这也需要在最终复位协议中替换。

最终目标和验收要求见 [WINDOWS_GPU_PLAN.md](../../docs/WINDOWS_GPU_PLAN.md)。
