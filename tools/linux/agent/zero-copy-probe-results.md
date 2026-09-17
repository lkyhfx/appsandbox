# AppSandbox zero-copy 实测记录

实测日期：2026-09-17 至 2026-09-18。

后续：[合成器 GPU 输出与资源互操作第二轮实测](gpu-output-probe-results.md)，
已验证 D3D12 EGL 绘制，并定位 GBM 纹理导出返回无效 FD 的阻塞。

## 结论

目标 Guest 的 CUDA → NVENC 编码可以运行。当前 Mesa/D3D12 → Vulkan
OPAQUE_FD → CUDA 纹理导入失败，且 GNOME 正在使用软件合成。因此，现有桌面
不能通过直接接入 CUDA/NVENC 就成为 GPU 全程驻留的显示链路。

这不是对所有零拷贝方案的否定，也不是端到端 4K60 达标报告。

| 验证项 | 实测结果 | 边界 |
| --- | --- | --- |
| framebuffer 来源 | asb_drm 主平面为 1920×1080、XR24、linear；实际 PRIME exporter 为 drm；GNOME 软件合成 | exporter 名字本身不表示显存/系统内存；需结合驱动和合成器证据判断 |
| GPU 资源互操作 | D3D12/Vulkan GPU 纹理成功导出；CUDA 导入返回 801 / CUDA_ERROR_NOT_SUPPORTED | 所测 Vulkan OPAQUE_FD 路径被阻断；未验证其他共享机制 |
| 实际编码 | Guest 编码 1800 帧 4K HEVC，GPU 合成输入平均 174.82 FPS；60 帧样本解码成功 | 简单色块，非桌面；未测真实捕获、格式转换或 Host 显示 |

## 实测环境

- HCS VM：`ubuntu`，Owner：`ubuntu`，SSH：`192.168.42.2`。
- Ubuntu 26.04.1 LTS，内核 `7.0.0-31-generic`。
- RTX 4070；Windows KMD 610.62；CUDA Driver API 13030。
- Guest NVIDIA-SMI 610.43.02；NVENC 最大 API 版本 `0xd1`。
- `/dev/dxg`、`/dev/dri/card1`、`renderD128` 存在；card1 驱动为 `asb_drm`。
- `appsandbox-display` active；当前桌面是 1080p，不是 4K。
- Vulkan：`Microsoft Direct3D12 (NVIDIA GeForce RTX 4070)`，API 1.2.328。
- 项目 Mesa BUILDINFO 为 25.3.6；Guest 实际加载 `/opt/wsl-mesa` 库。
- Guest 未装 FFmpeg。实验使用独立 C 程序，没有安装系统包或改显示配置。

## 1. 实际 framebuffer 与合成器

`zero-copy-drm.c` 只读 DRM 对象、导出 PRIME FD、查询 fdinfo，不执行 modeset、
mmap 或像素读取。普通用户 GETFB2 的 handle=0，因此由用户在 Guest 运行管理员探针。

管理员实测主平面：

```text
driver=asb_drm
plane=34 crtc=40 fb=43
width=1920 height=1080 format=XR24 modifier=0x0 pitch=7680 handle=1
size: 8355840
exp_name: drm
```

光标平面为 256×256、AR24、linear，大小 262144，exporter 同样为 `drm`。
FB ID 随运行变化正常；输出已归档为 `drm-root.log`。

GNOME Shell / Mutter 50.1 启动日志：

```text
Added device '/dev/dri/card1' (asb_drm) using atomic mode setting.
Failed to initialize accelerated iGPU/dGPU framebuffer sharing: Not hardware accelerated
Created gbm renderer for '/dev/dri/card1'
```

Guest 已安装 `/etc/systemd/user/org.gnome.Shell@.service.d/no-gpu.conf`，与项目
`tools/linux/wsl-mesa/org.gnome.Shell-no-gpu.conf` 一致。配置清除 D3D12 环境覆盖；
注释明确说明合成器保留 llvmpipe，仅应用使用 GPU 加速，以避开当前 GBM/KMS
兼容性问题。不能仅删除该配置就认定零拷贝可用。

驱动源码使用 `DRM_GEM_SHMEM_DRIVER_OPS`，采集器 mmap PRIME FD 后发送像素。
结合软件合成器的实测证据，当前桌面输出属于系统内存采集路径，不能视为
GPU 驻留输出。单独的 `exp_name=drm` 不足以证明内存位置；应用到合成器之间的
拷贝次数和 GPU 回读耗时没有通过 tracing 测量。

另外观察到 GNOME 持有一个 4096 字节 `udmabuf`，它不等于完整桌面 framebuffer，
没有用该 FD 作为主平面来源的证明。

## 2. GPU 纹理导出 → CUDA 导入

`zero-copy-vulkan.c` 创建 256×256 BGRA8 Vulkan 图像，使用 device-local、
dedicated allocation；导出真正的 Vulkan OPAQUE_FD 后，调用
`cuImportExternalMemory`，设置匹配的 opaque-FD 类型和 dedicated 标志。
没有把 DRM PRIME FD 冒充 OPAQUE_FD。

optimal 布局实测：

```text
opaque-FD image external features=0x6
Exported real GPU image: size=262144 memory_flags=0x1
cuImportExternalMemory=801 (CUDA_ERROR_NOT_SUPPORTED)
```

之前 RGBA8 optimal 测试同样返回 801。BGRA8 linear、sampled + transfer-destination
usage 的外部图像属性查询返回 `-11`（VK_ERROR_FORMAT_NOT_SUPPORTED），不能推广
为所有 linear 资源都不支持。

opaque-FD 二进制信号量能力查询：

```text
opaque-FD semaphore features=0x0 compatible=0x0
```

设备列出了 external-memory/semaphore-fd 扩展，但具体句柄类型仍可能不支持。
由于资源导入失败，未继续进行像素正确性、fence 同步、循环复用测试；timeline
semaphore 也未单独测试。这里报告的是具体阻断点，没有宣称零拷贝已完成。

该结果不排除直接 D3D12 互操作、D3D12 Video、跨 VM 共享或修改 Mesa/合成器后的方案。

## 3. NVENC 实际编码及码流验证

`zero-copy-nvenc.c` 实际创建 CUDA/NVENC 会话，在 GPU 分配 NV12 缓冲区，
通过 CUDA memset 改变整帧亮度，注册为 NVENC 输入，逐帧取得 HEVC 码流。

- 3840×2160；时间基准 60 FPS；HEVC Main、P1、ultra-low-latency。
- 无 B 帧，lookahead=0，CBR 目标 60 Mbit/s。
- 一次一个输入；CPU 等待 GPU 填充和编码输出完成。
- 前 60 帧写入样本，其余输出只计数。

```text
frames=600 seconds=3.402 fps=176.35 mean_ms=5.671 max_ms=14.824 bytes=1214186
frames=1800 seconds=10.296 fps=174.82 mean_ms=5.720 max_ms=13.957 bytes=3642379
```

计时覆盖合成帧填充、同步、编码及输出处理，不是独立硬件编码时间，更不是
端到端显示延迟。输入色块非常容易压缩，不能据此评价复杂桌面画质或吞吐。
1800 帧按最快速度处理，实际约 10 秒，不能称为长期稳定性验证。

600 帧运行生成的前 60 帧样本在独立 WSL 中由 FFmpeg 解码至 null，退出码 0；
ffprobe count_frames 确认：

```json
{"codec_name":"hevc","width":3840,"height":2160,"nb_read_frames":"60"}
```

此输入直接生成在 GPU 上，没有调用原始画面上传/回读 API。这仅验证 CUDA →
NVENC 阶段；没有 GPU trace 证明整个真实桌面无隐藏拷贝，也未完成实际 BGRA →
NV12 转换、Host 硬件解码呈现或端到端 4K60 验收。

## 证据与复现

本地 `build/zero-copy-probe/` 保存：`drm-root.log`、`nvenc.log`、
`vulkan-optimal.log`、`vulkan-linear.log`、`synthetic.hevc`。
Guest 源码、临时依赖和可执行文件位于 `/tmp/asb-zero-copy-probe/`。

临时依赖：仅解包 Ubuntu libvulkan-dev 1.4.341.0-1，以及公开的 nv-codec-headers
`n13.0.19.0` 的 `nvEncodeAPI.h`，未安装系统包。

```sh
cd /tmp/asb-zero-copy-probe
gcc -Wall -Wextra -O2 -I/usr/include/libdrm zero-copy-drm.c -ldrm -o drm-probe
gcc -Wall -Wextra -O2 -I. zero-copy-nvenc.c -ldl -o nvenc-probe
gcc -Wall -Wextra -O2 -I vulkan-dev/usr/include zero-copy-vulkan.c -l:libvulkan.so.1 -ldl -o vulkan-probe
sudo ./drm-probe
appsandbox-gpu ./vulkan-probe
appsandbox-gpu ./vulkan-probe linear
appsandbox-gpu ./nvenc-probe 1800
```

Vulkan 探针失败退出码是实测结果，不应改成成功。不要将失败后的步骤标记为通过。

另有 WSL 24.04 的 CPU testsrc2 → upload → NVENC 基线：180 帧、2.324 秒、约 77 FPS，
仅作环境对照，不用于推断 AppSandbox 桌面性能。
