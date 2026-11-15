# Windows ARM64 VirtIO 显示驱动

驱动来自 Fedora 官方 `virtio-win-0.1.302-1`，保留原始 INF、SYS 和微软签名目录 CAT，不修改、不重签名。云端 Windows 构建已使用 `SignTool verify /kp /c viogpudo.cat viogpudo.sys` 验证内核签名和目录成员关系，签名者为 Microsoft Windows Hardware Compatibility Publisher。

本驱动提供 Windows 显示模式和桌面帧缓冲，支持在系统显示设置中切换分辨率；**不提供 Direct3D 硬件渲染**。桌面动画仍可能受软件渲染和 ARM64 JIT 性能限制。

## 在 Windows 内安装

1. 正常关闭 Windows，退出 App。新版 App 中打开“VirtIO 显示设备”，关闭“UEFI test”和“挂载安装盘”；驱动盘会随下次启动挂载。
2. 自行用 StikDebug 启用 JIT，启动 Windows。
3. 在 Windows 打开管理员 PowerShell，粘贴：

```powershell
$v = Get-Volume -FileSystemLabel WINIOSDRV -ErrorAction Stop
if (-not $v.DriveLetter) { throw '驱动盘没有盘符，请保留当前画面；不要格式化任何磁盘。' }
pnputil /add-driver "$($v.DriveLetter):\viogpudo.inf" /install
```

如果安装程序提示重启，正常重启 Windows。然后进入“设置 → 系统 → 屏幕 → 屏幕分辨率”，选择合适的分辨率。先使用 1280×960 或 1600×1200；2048×1536 会增加软件渲染负担。

不需要导入自签名测试证书，也不需要为本包开启测试签名。不要运行旧的 `wingpudiag.inf`／`WinGpuProbe.exe` 诊断流程。

若出现黑屏，先等待系统恢复；下一次正常退出 App 后，可关闭“VirtIO 显示设备”，恢复固件显示路径。不要清理或重新格式化 Windows 系统盘。

此处描述的是预期行为；iPad 上的驱动启动和系统内分辨率切换须由用户启动后确认。源文件、哈希及官方镜像来源见随盘 SOURCE.json 和 SHA256SUMS.txt。许可证见 LICENSE.txt。
