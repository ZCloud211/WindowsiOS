# Windows ARM64 VirtIO 显示适配

用户反馈：Windows 已安装成功，但没有可用的显示模式切换，软件渲染使桌面动画缓慢。1.6 诊断驱动未解决这些用户问题。

## 本地 1.7 实现

- 新增标准 VirtIO GPU（1AF4:1050，SUBSYS 11001AF4，非 VGA 显示设备 0380）与现代 PCI 传输，连接 Windows ARM64 `viogpudo`。
- 实现 GET_DISPLAY_INFO、GET_EDID、RESOURCE_CREATE_2D、RESOURCE_ATTACH_BACKING、TRANSFER_TO_HOST_2D、SET_SCANOUT、RESOURCE_FLUSH、DETACH/UNREF 和复位。提供常用分辨率及有效 EDID，包含 1280×960、1600×1200、1920×1080、1920×1200、2048×1536。
- 按脏矩形从客体散布页拷贝一次到持久表面。Metal 使用共享缓冲区直接采样该表面，不增加一次完整桌面重新上传；不将此呈现优化称为 Direct3D 加速。
- 使用显式刷新版本号，避免旧采样哈希遗漏光标/文字等小区域变化。表面由资源、当前显示、帧快照和 Metal 缓冲区分别持有引用，改变分辨率或删除资源后不会提前释放被 GPU 使用的内存。
- App 中用“VirtIO 显示设备”开关代替旧 GPU 探针开关，默认打开；关闭安装盘挂载后附带 WINIOSDRV 驱动盘。关闭 VirtIO 开关可恢复固件显示路径。
- 原系统盘、UEFI NVRAM、ISO 安装盘保持独立；不自动在 Windows 内安装驱动或修改其启动/签名策略。

## 官方驱动来源及验证

Fedora 官方 `virtio-win-0.1.302-1`，原始 ISO SHA256：
`303f7ae40dad495d6ae474fdc571df58958a4dbc5c37a522d80f9a203867949d`。

[Windows 构建 43713195856](https://github.com/ZCloud211/WindowsiOS/actions/runs/43713195856) 提取原始 ARM64 INF、SYS、CAT，并以 `SignTool verify /kp /v /c viogpudo.cat viogpudo.sys` 验证通过。目录签名者 Microsoft Windows Hardware Compatibility Publisher；没有修改或重签原驱动文件。驱动版本 `100.103.104.30200`，日期 2026-07-22。

云端产物 ZIP SHA256：`e4043939f1fec56dfdd58ebce2395bc8bd9cb718b1a2627604ecb5b453e2b38d`。

保留上游 BSD 许可证。打包时核验全部文件哈希、ARM64 PE/Native 子系统，并用与生成器独立的 FAT32 读取器比对镜像中的原始驱动文件，保证目录覆盖的文件不变。不需要导入自签名测试证书。

## 已验证与尚未完成

主机真实 PCI/队列测试覆盖驱动显示命令序列、散布页、三种模式切换、单像素更新、完成围栏、INTx 和复位；Mac M1 实际执行 Metal 读取，并验证删除客体资源后缓冲区仍然有效。iOS 静态库、完整 App 和原核心/硬件信息检查通过。

这些是主机验证，不能宣称 iPad 上 Windows 驱动已经加载或分辨率已经实际切换。用户保留所有启动、JIT 与 Windows 操作；真机结果需用户操作后反馈。

**Direct3D 仍未实现。** 此显示专用驱动允许 Windows 管理显示模式，但 DWM/Direct3D 仍可能使用软件渲染。不能以显示驱动加载、Metal 呈现或改了分辨率作为 3D 加速完成的证据。下一阶段仍需可用的 ARM64 渲染 KMD/UMD 与 Metal 渲染后端。

Windows 内安装步骤见 [驱动说明](../drivers/windows/virtio-display/README.md)。完整本地 IPA：`build/WindowsiOS-iPad-1.7-VirtIO-display.ipa`，SHA256 `4ce6f9ce373756d5cc9e818f26670a20d33bde1f8d648384e2f5b69c30ab06bf`。
