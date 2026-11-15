# Windows ARM64 VirtIO 显示驱动

驱动来自 Fedora 官方 `virtio-win-0.1.302-1`，保留原始 INF、SYS 和微软签名目录 CAT，不修改、不重签名。云端 Windows 构建已使用 `SignTool verify /kp /c viogpudo.cat viogpudo.sys` 验证内核签名和目录成员关系，签名者为 Microsoft Windows Hardware Compatibility Publisher。

本驱动提供 Windows 显示模式和桌面帧缓冲，支持在系统显示设置中切换分辨率；**不提供 Direct3D 硬件渲染**。桌面动画仍可能受软件渲染和 ARM64 JIT 性能限制。

## 在 Windows 内安装

1. 正常关闭 Windows，退出 App。关闭“VirtIO 显示设备”“UEFI test”“挂载安装盘”，只打开“挂载驱动／日志盘”。
2. 自行用 StikDebug 启用 JIT，启动 Windows。在“此电脑”的 WINIOSDRV 盘双击 **准备显示驱动与蓝屏记录.cmd**，权限提示选择“是”。
3. 工具核对官方包哈希，在显卡关闭时把 302 版 ARM64 显示驱动加入驱动库；不会删除旧驱动或自动重启。保存原设置后，将 CrashDumpEnabled 设为 3（小型转储）、LogEvent 设为 1、AutoReboot 设为 0。分页文件设置保持不变，因此是否实际生成转储还取决于原系统配置。
4. 显示“准备完成”后，回复“已准备驱动”，由我只读核对工具盘上的 PREPLOGS。核对后再正常关闭 Windows、退出 App，打开“VirtIO 显示设备”，由你启用 JIT 并启动。新版通常应自动优先匹配，但仍需确认实际加载的驱动版本。

驱动加载后预期可在“设置 → 系统 → 屏幕 → 屏幕分辨率”切换模式。先使用 1280×960 或 1600×1200；2048×1536 会增加软件渲染负担。

如果只有壁纸和任务栏、窗口要按 Win+Shift+方向键才可见，说明仍是扩展桌面。运行同盘 **将VirtIO设为唯一主屏.cmd**，工具按显卡 PCI 标识选中 VirtIO，将其设为唯一主屏，并把鼠标放到画面中央。画面完整后在弹窗选择“保留”；30 秒不确认会尝试恢复原布局。结果在 DISPLAYLOGS，由我读取。原始固件 RAMFB 仍保留，关闭 VirtIO 后可使用原显示路径。

不需要导入自签名测试证书，也不需要为本包开启测试签名。不要运行旧的 `wingpudiag.inf`／`WinGpuProbe.exe` 诊断流程。

若再次蓝屏，下一次退出 App 后关闭“VirtIO 显示设备”，恢复原桌面，再双击“一键收集日志.cmd”。不要清理或重新格式化 Windows 系统盘。

此处描述的是预期行为；iPad 上的驱动启动和系统内分辨率切换须由用户启动后确认。源文件、哈希及官方镜像来源见随盘 SOURCE.json 和 SHA256SUMS.txt。许可证见 LICENSE.txt。
