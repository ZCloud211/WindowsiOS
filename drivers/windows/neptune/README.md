# WindowsiOS D3D11 preview

The untouched upstream v0.5 ARM64X package is from:
https://github.com/osy/kvm-guest-drivers-windows/releases/tag/v0.5

Archive SHA-256: 155695ab7820f71e75c369aeae7b8c060691ba43646ac37edef72aa2428506d5
KMD/UMD sources and build instructions: https://github.com/osy/kvm-guest-drivers-windows/blob/v0.5/viogpu/viogpu3d/BUILDING.md
UMD: https://github.com/osy/virtio-win-mesa

The catalog contains a Microsoft Windows Hardware Compatibility Publisher signature.
This is not a self-signed probe driver. The installer does not change signing policy.
WindowsiOS uses its own JIT and device implementation, Neptune transport and native
ARM64 DXMT/Metal. The initial iOS backend exposes D3D11; bundling the upstream
package's Vulkan ICD does not enable Venus or Vulkan host support.

Enable VirtIO display and D3D11 preview in the app before starting Windows. Mount
WINIOSDRV. Run 3d.cmd, accept Windows' administrator prompt, then restart Windows
if needed. Starting Windows and enabling JIT remain manual user actions.

The existing Windows disk is retained. The script places the driver, original
viogpudo files and Rollback3D.cmd in C:\WINIOS3D. To explicitly undo the driver
change, run that rollback command. It removes only the active viogpu3d package,
then installs the original display driver. No script restarts Windows automatically.

Mac native D3D11 and transport results do not prove iPad Windows DWM acceleration.
Device validation and performance measurements remain outstanding.
