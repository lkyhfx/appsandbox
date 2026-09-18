# 合成器 GPU 输出与资源互操作：第二轮实测

实测日期：2026-09-18。延续 [第一轮报告](zero-copy-probe-results.md)。
Guest：`yunsen@192.168.42.2`；Mesa 25.3.6 / RTX 4070 / Mutter 50.1。

## 结论

隔离 EGL 进程已经完成 **D3D12 GPU 纹理绘制和像素校验**；但该纹理的
DMA-BUF 导出没有通过。当前不能把 GPU 合成输出交给采集/编码程序。
本轮没有修复生产桌面的 GPU 输出，也没有打通 CUDA 资源导入。

| 检查 | 实测 | 边界 |
| --- | --- | --- |
| surfaceless EGL/GLES | D3D12 渲染器；256×256 RGBA 绘制、单像素校验通过 | 未声明 `EGL_MESA_image_dma_buf_export`，止于扩展检查 |
| GBM `/dev/dri/card1` | D3D12 绘制通过；导出函数返回 EGL_TRUE | 实际 `fd=-1`，不构成可共享输出 |
| GBM `/dev/dri/renderD128` | 同上 | 同样在导出元数据检查失败 |
| 独立无头 GNOME | 创建 GBM renderer、1920×1080 虚拟屏幕及 Wayland display；正常收到限时退出信号 | 未独立读取该合成器的 GL_RENDERER、帧内容或输出句柄，不宣称 GPU 桌面验收通过 |
| 同库集 llvmpipe 对照 | `FAIL stage=d3d12-renderer`，退出码 1 | 探针不会把软件渲染误报为通过 |

两个 GBM 节点均得到：

```text
renderer=D3D12 (NVIDIA GeForce RTX 4070)
PASS stage=gpu-render diagnostic_readback=1-pixel
fourcc=0x34324241 planes=1 modifier=0xffffffffffffff
export_fd=-1 stride=1024 offset=0
FAIL stage=export-metadata egl=0x3000 gl=0x0
EXIT_CODE=1
```

FourCC 为 AB24；modifier 为 DRM_FORMAT_MOD_INVALID，不能解释为 linear。
EGL/GL 错误值表示调用未报告 API 错误，但单平面导出的唯一 FD 无效。
这说明只检查扩展字符串或 EGL_TRUE 会误判。失败仅针对本探针的
GL RGBA 纹理分配/导出路径，尚未排除 GBM 预分配、其他格式或原生 D3D12 共享。

无头 GNOME 的关键日志：

```text
Added device '/dev/dri/renderD128' (asb_drm) using no mode setting.
Created gbm renderer for '/dev/dri/renderD128'
Added virtual monitor Meta-0
Using Wayland display name 'asb-gpu-probe'
GNOME Shell started
Shutting down GNOME Shell
EXIT_CODE=124
```

124 是预设 20 秒 timeout 的结果，不是成功验收退出码。隔离会话有 GDM 注册、
portal 和 Screencast 服务错误，不能据此声称完整桌面会话、录屏或稳定性通过。
实验结束后仅原 `/usr/bin/gnome-shell --mode=ubuntu` 仍运行，
`appsandbox-display` 为 active；未更改 `no-gpu.conf` 或安装系统包。

## 已增加的验证程序

- `gpu-output-probe.c`：指定 surfaceless 或确切 DRM 节点；创建 GLES FBO、
  GPU clear 并校验像素，然后尝试 EGLImage → DMA-BUF → EGLImage。
  检查 FD、stride、offset、plane 数、modifier，不把 DMA-BUF 当 Vulkan OPAQUE_FD。
- 成功导出后才执行重新导入和 120 次改变颜色的资源复用检查。本轮未到达这一步，
  因此这部分运行行为仍未验证。
- `gpu-output-probe.sh`：对三个入口分别记录退出码；任一路失败则整体返回 1。
- `gpu-compositor-probe.sh`：独立 D-Bus/runtime 目录运行限时无头 GNOME。

探针使用 glFinish 和每帧单像素 glReadPixels 做正确性诊断，**不是无回读链路**。
没有跨进程测试、native fence 同步、CUDA/NVENC 接入、KMS scanout 或 GPU trace。
头文件/API 依据：
[Khronos DMA-BUF export](https://registry.khronos.org/EGL/extensions/MESA/EGL_MESA_image_dma_buf_export.txt)、
[modifier import](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import_modifiers.txt)。

## 编译、复现和日志

Guest 文件保存在 `/tmp/asb-gpu-output-probe/`；本地原始日志在
`build/gpu-output-probe/`：`surfaceless.log`、`card1.log`、`renderD128.log`、
`compositor.log`、`runner.log`、`software-control-matched.log`。
编译使用 `-Wall -Wextra -Werror`，已通过；两个脚本已通过 `bash -n`。

临时依赖通过 apt-get download 和 dpkg-deb 解包，没有 dpkg 安装：
libegl-dev/libgles-dev/libgl-dev 1.7.0-3、libgbm-dev 26.0.8-1ubuntu0.3；
附加诊断工具 mesa-utils-bin 9.0.0-2build1。运行 Mesa 仍由 appsandbox-gpu
选择 `/opt/wsl-mesa` 25.3.6，不是头文件包对应的 Mesa runtime。

```sh
cd /tmp/asb-gpu-output-probe
mkdir -p deps
apt-get download libegl-dev libgles-dev libgl-dev libgbm-dev mesa-utils-bin
for p in *.deb; do dpkg-deb -x "$p" deps; done
gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
    -Ideps/usr/include -I/usr/include/libdrm gpu-output-probe.c \
    -l:libEGL.so.1 -l:libGLESv2.so.2 -l:libgbm.so.1 -o gpu-output-probe
bash gpu-output-probe.sh results
# 当前预期返回 1；查看各日志的 FAIL stage，不忽略退出码。
bash gpu-compositor-probe.sh compositor.log
# 限时测试预期返回 124；必须结合启动/关闭日志解释。
appsandbox-gpu env GALLIUM_DRIVER=llvmpipe MESA_LOADER_DRIVER_OVERRIDE=llvmpipe \
    ./gpu-output-probe surfaceless
# 软件对照预期在 d3d12-renderer 检查失败。
```

曾在未用 appsandbox-gpu 固定库集时尝试 `GALLIUM_DRIVER=llvmpipe` /
`MESA_LOADER_DRIVER_OVERRIDE=swrast`，进程退出 139，日志保存在
`software-control.log`。原因未定位；不能归为 llvmpipe 互操作结果。
固定到同一 Mesa 库集后得到上表所述的正常拒绝结果。

## 后续实现切入点

1. 在 Mesa 25.3.6 的 EGL/DRI image export → Gallium d3d12 resource handle
   路径加日志，确认何处丢失有效 FD；同时对比 GBM 预分配共享纹理。
   本轮没有源码级证据，不把 `fd=-1` 直接归因于某个具体函数。
2. 若输出只能提供 dxg/D3D12 原生共享资源，必须实现匹配的资源和 fence
   消费端，再选 Guest 编码或 Host 编码。不能用类型转换把该句柄冒充
   DMA-BUF / CUDA opaque FD；第一轮 CUDA 801 的阻塞仍然存在。
3. 待单纹理共享、像素正确性和同步复用通过后，再接入隔离 Mutter 的
   render target；最后才替换生产桌面输出及现有 CPU 采集协议。

## 第三轮准备（尚未计入上述实测结论）

已加入 Mesa 25.3.6 补丁和 GBM 共享分配探针，供下一轮构建后验证：

- `../wsl-mesa/patches/0001-d3d12-fix-shared-resource-export.patch`：检查
  `CreateSharedHandle` 的 HRESULT/无效句柄，并让 EGL 在 FD 查询失败时返回
  `EGL_FALSE`，不再出现“成功但 fd=-1”。补丁刻意不把
  `D3D12_HEAP_FLAG_SHARED` 的 dxg/NT shared-object FD 当作 DMA-BUF 导出。
- `gbm-shared-export-probe.c`：走 GBM 预分配共享纹理路径，并用
  `drmPrimeFDToHandle` 验证导出的描述符确实是 DRM PRIME dma-buf；D3D12
  opaque shared fd 不能冒充通过。

编译和运行：

```sh
gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
    -Ideps/usr/include -I/usr/include/libdrm gbm-shared-export-probe.c \
    -l:libgbm.so.1 -l:libdrm.so.2 -o gbm-shared-export-probe
bash gpu-gbm-share-probe.sh results-gbm-share
```

当前未打补丁的 Guest 上，两个 GBM 节点均复现 `export_fd=-1`，停在
`gbm-export-valid-fd`。补丁后的预期变化是 EGL/GBM 明确返回导出失败，而非
假成功；零拷贝验收仍须由后续真正的 dma-buf bridge 或匹配的 D3D12 native
consumer 同时满足有效句柄、类型、像素 round-trip 和同步复用检查。

本轮将阻塞从“可能不兼容”收敛到可重复的 GPU 纹理导出失败，
并验证无头合成器可独立启动；尚不足以修改生产桌面的软件合成兜底。

## 第四轮实测：D3D12 native consumer

已选择原生 D3D12 shared-handle 消费端，并加入
`d3d12-native-share-probe.cpp` 与 `gpu-d3d12-native-share-probe.sh`。探针的
验收边界是：

1. 在同一 DXCore adapter 上创建两个独立 `ID3D12Device`；
2. 生产端创建带 `D3D12_HEAP_FLAG_SHARED` 的 BGRA8 纹理和带
   `D3D12_FENCE_FLAG_SHARED` 的 fence；
3. 消费端仅用 `OpenSharedHandle` 打开两个 dxg/NT shared-object FD；不调用
   DRM PRIME、DMA-BUF 或 CUDA opaque-FD 导入；
4. 消费端先排队等待共享 fence，生产端再 clear 纹理并 signal，以排除仅靠
   CPU/提交顺序造成的假通过；
5. 消费端复制到 readback heap，校验 BGRA 像素值，并用第二组颜色和递增
   fence value 复用同一资源、命令对象与 fence，排除单帧假通过。

构建入口是可选的 `make d3d12-native-share-probe`，未加入生产 daemon 的
`all` 目标。运行：

```sh
make d3d12-native-share-probe
bash gpu-d3d12-native-share-probe.sh results-d3d12-native-share
```

Guest 已安装 GCC 15.2、Make 4.4.1、DirectX-Headers 1.619.1；探针以
`-Wall -Wextra -Werror` 编译通过。RTX 4070 上的原生纹理路径实测结果：

```text
PASS stage=two-devices adapter_luid=00000000:10a35fcb
PASS stage=resource-shared-handle fd=53
PASS stage=fence-shared-handle fd=54
diagnostic_same_device_open_fence=0x80070057
FAIL stage=open-shared-fence HRESULT=0x80070057
```

即共享纹理句柄能由第二个 `ID3D12Device` 打开；共享 fence 虽能导出有效
dxg FD，但 `OpenSharedHandle` 返回 `E_INVALIDARG`。同一生产 device 重开该
fence 也失败，排除了“仅跨 device 不支持”。改用
`D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER` 时，`CreateFence` 本身即返回
`E_INVALIDARG`。

为继续隔离资源路径，`--cpu-sync-fallback` 在生产 fence 完成后由 CPU 排序，
不把它计作 native fence 成功。该模式得到：

```text
pixel_bgra=191,127,64,255 expected=191,128,64,255
PASS stage=native-shared-pixel-roundtrip
reuse_pixel_bgra=102,51,204,255 expected=102,51,204,255
PASS stage=native-shared-reuse
```

首帧绿色通道相差 1，位于 UNORM 舍入容差内。结论是 **D3D12 native shared
resource 的打开、跨 device 像素 round-trip 和复用均已通过；native shared
fence 打开仍被当前 libd3d12/dxg 组合阻塞**。诊断 fallback 固定返回 3，避免
被误报为完整验收成功。

## 第五轮实测：eventfd 完成通知

探针的 CPU fallback 已不再轮询 `GetCompletedValue`。现在每次等待都创建带
`EFD_CLOEXEC | EFD_NONBLOCK` 的 Linux `eventfd`，按 DirectX-Headers 的 WSL
约定将 fd 转换为 `HANDLE` 传给 `ID3D12Fence::SetEventOnCompletion`，再通过
10 秒有界 `poll` 和 `eventfd_read` 等待通知。收到通知后仍会检查
`GetCompletedValue() >= expected`，防止过早唤醒被误报为成功。生产 fence 与
consumer readback 完成 fence 都覆盖首帧和复用帧。

运行入口为：

```sh
make d3d12-native-share-probe
bash gpu-d3d12-native-share-probe.sh results-d3d12-eventfd --cpu-sync-fallback
```

本机 Ubuntu 24.04 WSL / RTX 4070 冒烟测试使用 DirectX-Headers 1.614.1 和
系统 WSL `libd3d12.so` / `libdxcore.so`；源码以 `-Wall -Wextra -Werror`
编译通过。关键结果为：

```text
BLOCKED stage=open-shared-fence HRESULT=0x80070057; continuing with diagnostic CPU wait
PASS stage=producer-eventfd-notification eventfd_notifications=1 completed=1
PASS stage=consumer-eventfd-notification eventfd_notifications=1 completed=1
PASS stage=native-shared-pixel-roundtrip
PASS stage=producer-eventfd-notification-reuse eventfd_notifications=1 completed=2
PASS stage=consumer-eventfd-notification-reuse eventfd_notifications=1 completed=2
PASS stage=native-shared-reuse
BLOCKED d3d12-native-share shared-fence-open
```

因此 `SetEventOnCompletion(eventfd)` 已对生产、消费两侧的首帧和复用帧各完成
一次真实通知，计数均为 1，完成值分别到达 1 和 2；没有退回轮询。该路径成功
时仍固定返回 3；只有跨 device shared fence 打开、排队等待、像素 round-trip
和复用全部通过才返回 0。此冒烟环境不是上述 `yunsen@192.168.42.2` 隔离
Guest，后者仍应复跑同一命令以确认其 DirectX-Headers 1.619.1 / libd3d12
组合；生产桌面、`appsandbox-display` 和 Mesa 运行时仍未更改。
