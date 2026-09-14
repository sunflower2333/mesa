# DroidVM 原生 Freedreno WDDM 公共后端：第一阶段

本目录是面向 **Freedreno Gallium 原生 OpenGL/OpenGL ES** 的公共 KMT
传输基础，不是已经完成的 Windows OpenGL 驱动。

目标主线：

```
OpenGL / OpenGL ES
    -> Mesa WGL / EGL 与 state tracker
    -> Freedreno Gallium
    -> 公共 Freedreno WDDM 后端
    -> VIOGPU KMD
```

回退路线保留为 `OpenGL -> Zink -> Turnip/WDDM`。Zink OpenGL 工作流改为
仅手动触发，不删除其代码、已有正确性修复或测试。已有安装包和系统 ICD
注册保持不变；本补丁不会在原生路径未完成时偷偷切换运行时驱动。

## 本阶段实际实现

原先 `vulkan/tu_knl_wddm.cc` 同时包含 KMT 传输和 Vulkan 集成。现在将设备、
上下文、分配、映射、Render、fence 等 OS 传输实现抽入本目录。
`libfreedreno_wddm` 的源文件和链接配置不依赖 Vulkan、Zink 或 libdrm。
为了让现有 Turnip 调用端兼容，保留 `tu_wddm_*` 的历史符号和结构体名称；
这些名称不表示会创建 VkInstance、VkDevice 或通过 Vulkan 转译。

`freedreno_wddm_submit.c` 提供 C11 提交包编码器，接口见
`freedreno_wddm_submit.h`。Turnip 的真实 Render 路径已调用此编码器，后续
Gallium 后端可调用同一接口，无需再次复制私有 ABI 的组包逻辑。
编码器不申请内存、不等待 fence、不调用图形 API；输入输出缓冲区由调用者
持有，彼此不得重叠。它先验证全部输入，再写入输出；拒绝时将输出长度设为
零，提交包内容保持不变。

检查包括 1024 个 BO、256 条命令和外层 Render 总计 64 KiB 的边界，访问
标志、索引、非空命令、四字节对齐及减法形式的范围检查。命令 offset 的
对齐检查比旧组包代码更早拒绝错误输入。host handle、presumed IOVA 和
重定位字段仍留给 KMD 验证与修补，不在用户态伪造。

原有每设备延迟分配的提交暂存区继续复用；增加的 BO/命令描述数组也是该
暂存区的一部分，不产生每次提交的堆分配。all-live BO 驻留列表、reset
代际校验、半范围 fence 比较、busy 时保留所有权、销毁重试均保留。
共享 ABI、dispatch 和 lifetime 头/源由原文件移动，原 Vulkan 头提供转发
包含，避免复制两套定义。KMD、私有 ABI 和延迟销毁默认开关没有修改。

## 尚未实现，不得据此宣称完成

尚未实现 Gallium 使用的 WDDM `fd_device`、`fd_pipe`、`fd_bo`、ringbuffer/
submit/fence 回调，也未接通 WGL screen、窗口呈现、EGL/GLES。
顶层 Meson 对 Gallium Freedreno 与 WDDM 组合的拒绝仍保留。
单纯删除该拒绝、设置 `GALLIUM_DRIVER=freedreno` 或重命名 DLL 都不算移植。

当前唯一完整接入公共库的图形 API 消费者仍是 Turnip；本次拆分消除了
后续原生 Gallium 后端必须依赖 Vulkan 实现的结构障碍，没有假装已经
形成完整的 Gallium 渲染路径。

因此没有可替换系统 OpenGL 的 `opengl32.dll`，也没有新 A8xx 能力、完整
OpenGL/ES 一致性、零拷贝呈现、ARM64EC/ARM64X、x86/x64 游戏兼容性或
性能提升的验收结论。不得修改 DriverStore 或系统注册来试用这个静态库。

## 验证方法

Linux 上执行实际 C 编码器和既有策略/生命周期测试：

```sh
python3 src/freedreno/wddm/tests/check_native_split.py
python3 src/freedreno/wddm/tests/run_packet.py
python3 src/freedreno/wddm/tests/run_packet.py --negative-control capacity
python3 src/freedreno/wddm/tests/run_packet.py --negative-control iova
CC=clang python3 src/freedreno/wddm/tests/run_packet.py
python3 src/freedreno/vulkan/tests/check_tu_wddm_policy.py
python3 src/freedreno/vulkan/tests/lifetime-perf/run.py
for mutation in fence busy residency scratch; do
    python3 src/freedreno/vulkan/tests/lifetime-perf/run.py --negative-control "$mutation"
done
```

`packet_test.c` 包含独立 84 字节标准输出、拒绝后不改包、上限数组、500 组
随机包与独立小端解码器比较。两个负向控制故意破坏长度保护或 IOVA 清零，
必须在指定断言失败，不把任意崩溃视为测试成功。既有生命周期夹具仍使用
显式 fake OS 对象，不是设备测试。

`.github/workflows/windows-freedreno-native-wddm.yml` 新增 x64 和 ARM64
独立编译/链接门禁。它不编译或链接 Vulkan/Gallium 库；x64 执行纯数据、
不打开设备的链接自检和 fake-dispatch 测试，ARM64 仅编译/链接检查。
它产出静态库及测试程序，不产出原生 OpenGL DLL。现有完整 Turnip 编译
工作流同步修改源文件位置，仍需单独通过。

本补丁交付时只在 Linux 运行了源码、编码器和 fake OS 夹具。Windows
MSVC/WDK 编译、完整 Mesa 配置/链接和真实 GPU 执行均未运行；新增工作流
尚未推送或触发，不能将工作流文件存在当作 CI 成功。

## 下一阶段接入边界

首先实现 Gallium 的 WDDM device/pipe/BO 适配：替换 Linux fd/ioctl 依赖，
对齐 LUID、GPU 参数、地址分配、CPU 映射和 context/resource 所有权。
再接通实际 ringbuffer 提交与 fence，验证多 context 资源共享、内存压力、
reset/TDR、busy 销毁，以及失败时不提前复用 GPU 地址。

随后接通 WGL screen 和呈现，再单独验收 EGL/OpenGL ES。应先离屏 clear/
draw/readback，再窗口 swap；必须检查实际 renderer，禁止把 Zink 或软件
回退结果计为原生成功。不同进程体系结构和安装包部署分别验收。
