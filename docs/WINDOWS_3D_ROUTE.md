# Windows ARM64 3D 路线选择（2026-11-16）

目标：M1 iPad Pro、iPadOS 18.7.8，在当前 WindowsiOS ARM64 JIT 上实现 Windows 的实际 D3D11 / DWM 加速，同时控制客体 CPU 翻译、跨边界提交和图像复制开销。

## 推荐：Triton / Neptune + 原生 DXMT

应用与 Windows Direct3D runtime → Triton ARM64X UMD → WDDM viogpu3d KMD → 共享批量命令 / VirtIO → Neptune 宿主解码 → 原生 arm64 DXMT → Metal → 当前 App 全屏呈现。

选择理由：已有可复用的 WDDM KMD、D3D DDI、ARM64/ARM64EC 打包与 DWM 共享资源工作；DXMT 原生分支已列出 iOS 16 目标，可避免把 D3D11 再经过 Vulkan 才转到 Metal。保留本项目的 JIT 与原生 App，宿主资源映射、异步完成、纹理生命周期和直接呈现继续采用 Henge 式设计。这里的 Neptune 模块负责 D3D 命令传输，不能把库名 virglrenderer 理解为必须走 OpenGL。

这是可行性与低开销之间的推荐取舍，不是已经测得最快或已完成移植的结论。现有 viogpudo 是显示专用驱动；WG 固定三角形探针不能替代 Neptune 协议与完整 WDDM 驱动。

## 路线比较

| 路线 | 可复用部分 / 限制 | 当前选择 |
| --- | --- | --- |
| Triton / Neptune → native DXMT → Metal | 已有 Windows ARM64X DDI/KMD 和 iOS native 后端；仍需对接设备、共享资源和呈现 | 优先 |
| Yttrium / Venus → Vulkan → MoltenVK → Metal | 可复用 Mesa 3D 工作，但其宿主要求与外部内存机制需要适配；iOS 上多一层 API 转换 | 暂不作为首选 |
| 完全自行开发 WDDM KMD/UMD → Metal | 可定制性大；完整 DDI、共享资源、调度与着色器实现工作量最大 | 优先复用已有实现 |

## 开发切入点

1. 整理匹配的 KMD、UMD、Neptune 协议子模块和 native DXMT 版本，明确 iOS 构建依赖。候选提交见 windows-3d-candidates.json；它不是已经验证可混用的版本锁。
2. 为当前 VirtIO 设备补齐驱动需要的上下文、capset、3D 提交、resource blob、host-visible aperture 与异步围栏。沿用现有 2D、显示模式和全屏布局。
3. 在同一 iOS 进程内运行渲染上下文线程，复用 native DXMT 的 D3D11、共享纹理和完成接口，适配 Henge 式共享内存与 Metal 呈现。客体 4KiB 页与宿主 16KiB 页要显式处理。
4. 交付实际 Windows 运行结果：硬件 D3D11 设备创建、可编程着色器/纹理绘制、进程间共享窗口表面与 DWM 合成、模式切换和持续呈现。随后针对每帧提交次数、客体 CPU 时间、同步等待和复制字节数优化。

## 开销约束

- ARM64 原生 KMD，UMD 同时提供 ARM64 与 ARM64EC 视图；x86 应用另外需要 x86 UMD。ARM64X 不能消除 x64 应用自身的 CPU 指令翻译。
- 合并提交，减少 MMIO 门铃；GPU 完成异步通知，优先减少同步等待和 CPU 轮询。
- 缓冲区只在符合对齐、寿命与一致性条件时共享；渲染纹理允许保持 GPU 最优布局。最终呈现避免 CPU 回读后重新上传，需要转换时优先 GPU 完成。
- 缓存 shader / pipeline；区分首次编译与稳定运行帧时间。转换层数量少是设计取舍依据，不能直接换算成未经测量的性能损耗百分比。
- 首版 iOS 目标为 D3D11 / DWM。完整 D3D12 与 Apple 的 macOS D3DMetal 运行库属于另一项兼容性工作。

## 已核对的一手资料

- [Windows KMD v0.5 构建与 ARM64X 说明](https://github.com/osy/kvm-guest-drivers-windows/blob/v0.5/viogpu/viogpu3d/BUILDING.md)
- [Triton / Neptune 的 DDI、DWM 共享表面与 native DXMT 架构](https://blog.getutm.app/2026/introducing-triton-directx-11-driver-for-qemu/)
- [DXMT native 分支的 iOS 构建目标](https://github.com/utmapp/dxmt/blob/main/meson.build)
- [当前 iOS DirectX 路径为 DXMT / D3D11](https://docs.getutm.app/guest-support/windows/#directx-graphics)
- [Henge AppleParavirtGPU 协议边界](https://github.com/Leviidev/Henge/blob/main/devices/pvg_lite.h)
- [Venus 的宿主及共享内存要求](https://docs.mesa3d.org/drivers/venus.html)

## 实现与验证进度

- iOS ARM64 native DXMT 和 Neptune 静态渲染库均已编译；DXMT 最低 iOS 17，Neptune 配对提交为 `dd0652b13e589c4c124ea79123a3adcd85173f4f`，两端 directx-native ABI 1。
- 已加入标准 VirtIO Neptune capset、context、blob、submit、scanout、map/unmap 和异步按上下文/时间线完成。配置只有在原生后端创建成功时才公布 3D 功能。
- 512MiB PCI 共享窗口以 4KB 页别名映射独立宿主资源，解决 Windows 4KB / Apple 16KB 差异，无需修改已签名的 KMD。TLB 缓存命中仍直接读写宿主指针；映射更换请求各 CPU 丢弃缓存，并等待确认后回收旧映射和完成队列。
- Metal 可直接呈现保留的原生纹理，或从 Neptune 导出的共享 FD 建立不复制的线性纹理。iOS 匿名共享 FD 使用 App 临时目录内立即 unlink 的文件，不要求 App Group 权限；这依然是文件映射，尚未量化其写回代价。Neptune/DXMT 内部可能有 GPU 表面转换，不能声称整个链路绝对零复制。
- Mac M1 的 native DXMT suite：181 项通过、0 失败，包含真实着色器、纹理、深度、混合、MSAA、计算与同步。真实 Neptune 命令流成功创建 D3D11 硬件设备、使用共享 reply blob，并完成异步围栏。
- Mac Metal 测试通过原生私有纹理和共享 FD 呈现，并核验生产者更新的每个像素；VirtIO split queue 的 3D 协议与现有 2D/MSI-X 回归通过。
- 1.8.0 build 15 IPA 打包 native DXMT framework、Neptune 与完整上游 ARM64X v0.5 驱动。3D 预览开关初始关闭；用户在 App 启用后，启动 Windows 并运行 WINIOSDRV 的 `3d.cmd` 安装，JIT/启动/Windows 操作仍由用户执行。
- 驱动目录证书为 Microsoft Windows Hardware Compatibility Publisher，SYS 内嵌签名为 Turing Software EV；不改 INF/CAT/二进制，不更改 Windows 签名策略。Windows 内核策略的 SignTool 验证由独立分支 CI 执行。

**尚未确认**：iPad 上实际加载的 3D 驱动、Windows DWM/应用的硬件渲染、模式切换与长时间运行，以及帧时间/功耗/同步/复制的实测结果。Mac 成功或 IPA 构建成功均不能替代这些结果。

