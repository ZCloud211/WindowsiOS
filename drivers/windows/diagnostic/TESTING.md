# WindowsiOS Metal 命令诊断版

这是 ARM64 PCI 诊断驱动，**没有 Direct3D、DWM 或游戏加速**。它在不接管现有显示设备的情况下，提交固定离屏 GPU 命令，检查 Metal 完成围栏。Windows 中的加载和执行仍待真机验证。

仅适用于 WindowsiOS 新版 App 手动开启“GPU 驱动测试”后出现的 `PCI\VEN_1B36&DEV_FF10`，设备名称应为 `WindowsiOS Metal Command Diagnostics (no Direct3D)`，在“系统设备”分类；原有显示适配器保留。

此包包含临时自签名测试证书，不是微软生产签名。`BUILD.json` 列出源码版本、构建编号与证书指纹；只导出公开 CER，不包含签名私钥。切勿把旧版 `WinIOSGpuProbe.sys` 显示驱动与此包混用。

## 手动验证（由用户操作 Windows）

前提：先正常关闭 Windows，在 App 开启 GPU 驱动测试并关闭安装盘挂载，再启用 JIT、启动。诊断盘应自动挂载为卷标 `WINGPUTEST`；驱动文件位于卷根目录。若磁盘没有自动分配盘符，先在磁盘管理确认该卷，不要格式化磁盘。

在管理员终端进入该卷后，先查看证书指纹与 `BUILD.json` 是否一致：

```powershell
certutil -dump .\WinGpuDiagnostic-test.cer
```

为在这台测试虚拟机加载该驱动，需要将测试证书加入客体 Windows 的本机信任库，并开启测试签名。此操作仅在虚拟机 Windows 内执行；测试签名会放宽该客体的驱动签名限制。App 和探针不会自动执行以下命令。

```powershell
certutil -addstore Root .\WinGpuDiagnostic-test.cer
certutil -addstore TrustedPublisher .\WinGpuDiagnostic-test.cer
bcdedit /set testsigning on
shutdown /r /t 0
```

如果 `bcdedit` 明确报告 Secure Boot 策略阻止，停止此步骤并保留错误内容，不要清理分区或修改系统盘。完成重启后，在管理员终端重新进入诊断盘，执行：

```powershell
pnputil /add-driver .\wingpudiag.inf /install
.\WinGpuProbe.exe --repeat 10
```

通过条件：PnP 设备启动没有错误码；探针打印 invalid-request 检查通过，以及 10 次 `status=0x00000000 state=1 error=0`、单调递增的非零 fence。单独一项通过不能证明已支持 Direct3D。`gpu_ns=0` 可能表示宿主未提供 GPU 时间戳，不作为完成失败。

保留探针输出和 Windows 设备状态；不要通过重新安装整个 Windows 来排查。

## 退出测试

关闭 App 中的 GPU 驱动测试即可让下次启动不枚举实验设备。若要卸载驱动，先使用 `pnputil /enum-drivers` 找到原始名称为 `wingpudiag.inf` 的准确 `oemNN.inf`，再卸载该项；不要删除其他显示或磁盘驱动。

可在管理员终端执行 `bcdedit /set testsigning off`，再正常重启 Windows。证书可依据 `BUILD.json` 的准确指纹从本机 Root 和 TrustedPublisher 库移除；不要批量删除证书。
