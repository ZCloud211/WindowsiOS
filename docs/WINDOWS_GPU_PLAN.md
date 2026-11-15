# Windows ARM64 → Metal GPU 适配目标

目标设备为 M1 iPad Pro、iPadOS 18.7.8、StikDebug/JIT。用户要求 GPU 阶段优先追求 Windows 实际绘图效率，并以 Henge 为主要渲染参考。

## 目标架构

原生 ARM64 WDDM 内核/用户态驱动 → 共享描述符与资源 → 批量命令提交 → 宿主原生 Metal 执行 → GPU 纹理直接呈现。

这是当前选择的低开销设计方向，不是已经证明“绝对最快”的性能结论。是否优于其他实现，必须用相同负载测量。

- Windows 图形驱动按 ARM64 构建，减少额外的指令翻译层；其代码仍由本项目的 ARM64 JIT 执行，并非裸机运行。
- 参考 Henge 的资源映射、命令重放、异步完成和显存生命周期管理。Windows 使用自己的版本化协议与公开 Metal 接口，不假设 macOS 驱动的私有序列化格式对 Windows 可用。
- 多个绘图命令打包后只触发一次设备门铃。最终调度使用异步提交、多帧并行和完成中断/围栏；避免每条绘图命令都触发一次 MMIO 或同步等待。
- 可映射的缓冲区在符合对齐、生命周期及缓存一致性要求时共享。渲染纹理常驻 GPU，按需选择 Metal 存储布局；不能把“所有纹理都放共享线性内存”当成最佳性能。
- 着色器创建时转换并缓存。Apple Metal shader converter 接收 DXIL，D3D11 常见 DXBC 需要额外转换与兼容验证。Apple 3.1 起列有 iOS 库支持，但具体 SDK 的最低系统版本、M1 目标与 iPadOS 18.7.8 的实际兼容性仍需检查安装包并测试。
- 呈现直接使用渲染结果纹理；生产路径不把最终图像读回 CPU，再上传到显示纹理。

着色器/管线缓存键需覆盖原始字节码、入口/阶段、资源绑定布局、专门化参数、转换器版本、GPU 家族、最低目标系统和协议版本，避免错误复用。

## 已实现的 P0：传输与 Metal 命令验证

新增共享 ABI `gpu/wingpu_protocol.h`、Windows KMD 共用编码器 `gpu/wingpu_probe.c`、实验 PCI 设备 `devices/wingpu.c`、Metal 后端 `devices/wingpu_metal.m`。

- 描述符为固定 64 字节，256 条命令为一批。完整校验后才向 GPU 提交；完成结果在 VM 线程发布，Metal 回调不直接修改 JIT 或客体内存。
- P0 只实现 GPU 私有 BGRA8 目标、清屏、固定测试三角形、资源释放和完成回执。测试三角形不能被宣传为 Direct3D 功能实现。
- 显存预算同时计算当前资源和本批内仍被 GPU 保留的已销毁资源，防止创建/销毁循环绕过配额。
- 实验设备只有明确设置 `HV_WINGPU_PROBE=1` 才枚举，默认会话不增加它。`1b36:ff10` 是本项目暂用的实验 ID，不是正式分配的厂商产品标识。
- 生产后端没有像素回读；`*_test_readback` 只用于主机自动验证。

Windows 侧从微软 KMDOD 示例引入独立显示驱动工程，增加受限的私有 escape 与传输组件，并使用与宿主测试相同的批量编码器。它仍是显示/传输验证载体，没有 Direct3D UMD。其探针等待属于诊断路径，不是最终 GPU 调度方案。

## 验证证据与边界

已通过：

1. Mac M1 上调用 Windows KMD 共用编码器，经真实 PCI ECAM/BAR 模型提交，由 Metal 实际完成清屏/三角形，再将完成围栏回写模拟客体 RAM。
2. GPU 结果回读验证蓝底红色三角形；错误 ABI、命令尾部、计数、地址溢出、内存重叠、重复围栏及显存资源循环检查。
3. 共享编码器交叉编译为 Windows ARM64 COFF 对象，确认 ABI 大小和基础 C 源码可编译。
4. iOS ARM64 静态库编译，以及原有核心和 CPU/SMBIOS 回归检查。

未验证：完整 Windows `.sys`/探针的 WDK 编译、驱动签名与加载、POST 显示所有权、iPad 上 GPU 命令执行、Windows 桌面和任意 Direct3D 应用加速。上述主机测试不等于在 Windows 内运行过驱动。

## 下一道验收关卡

1. 在 Windows/WDK 中编译完整内核驱动和 ARM64 探针，修复真实编译错误。
2. 验证驱动加载与固定 GPU 探针的完成回执，处理 POST 所有权、复位、电源状态和失败恢复。目前仍不能在用户已安装系统上直接替换显示驱动。
3. 在独立测试环境补齐 WDDM 资源、上下文、GPU 围栏、调度和用户态 Direct3D DDI；接入并验证着色器转换及缓存。
4. 再把实际渲染纹理交给显示链路，验证 DWM/常用 Windows 应用，随后才测性能。

性能记录应分别包含：客体/宿主 CPU 时间、每帧设备退出数、提交批大小、GPU 执行时间、CPU↔GPU 复制字节数、峰值显存、冷/热缓存着色器时间、帧时间中位数及 P95/P99。仅 FPS、GPU 名称或 Metal 提交次数不足以证明完成。

## 来源

- [Henge 设备与 Metal 后端](https://github.com/Leviidev/Henge/tree/main/devices)
- [Microsoft KMDOD 示例及其能力边界](https://github.com/microsoft/Windows-driver-samples/tree/main/video/KMDOD)
- [Microsoft 驱动构建环境](https://learn.microsoft.com/windows-hardware/drivers/develop/building-a-driver)
- [Apple Metal shader converter 系统要求、输入及 iOS 版本记录](https://developer.apple.com/metal/shader-converter/)
- [Microsoft DXIL 与 DXBC 转换设计](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/DXIL.rst)
