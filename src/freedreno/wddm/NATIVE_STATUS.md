# 原生 WDDM 资源与提交实现状态

本文件更新 README 第一阶段之后的实现进展；仍不表示已有可安装的原生 OpenGL DLL。

## 已完成的代码

`freedreno_wddm_native.h` 是不含 Windows/Vulkan/DRM 头的 C 接口，实际实现位于
`freedreno_wddm_native.cc`，已加入 `libfreedreno_wddm` 的生产构建。

支持按 LUID 打开设备、查询验证后的硬件参数、创建和映射 native BO、预留 GPU
地址、提交真实 native IB、查询和等待 fence、释放 BO、失败销毁重试与隔离队列。
默认创建 OpenGL client-hint 的 KMT context；原有 Turnip context 包装函数仍使用
Vulkan client hint，现有调用行为不变。

当前私有 ABI 的 allocation 绑定创建它的 KMT context。因此同一原生 device 使用
一个 context/队列和地址空间，内部串行保护其 KMT 命令、分配与 patch 替换缓冲区。
后续 Gallium logical pipe 必须共享该 owner，不能把同一 BO 提交到另一 KMT context。

每次 Render 构建全部存活 BO 的驻留列表，调用公共 C 编码器和真实 KMT transport。
提交暂存区只在首次需要时分配，之后复用。CPU 端用 64 位 serial，wire fence 保持
32 位并跳过保留值零；未来、倒退、半范围不明确的 completion 均不作为回收证据。

创建失败但返回 handle、unlock 失败、VidMm busy、context/device/adapter 销毁失败
都会保留相应所有权；只有实际销毁成功才释放 GPU VA。close 不设置 AssumeNotInUse。
成功 Render 后若 KMT 返回无效替换缓冲区，仍记录已接受 serial，但设备进入 lost。

## 调用端仍须遵守

调用 submit 时保持 command/resource BO 引用有效，并在进入前完成 CPU 写入的缓存
发布。MemoryBarrier 不等价于验证了所有 guest/host 内存别名的 cache coherence。
CPU readback 与 GPU 可见性仍需硬件测试，不能用 fake OS 的可见性代替。

`fd_wddm_native_close()` 失败时必须保留 owner 或交给 quarantine。前端卸载代码必须
以 `fd_wddm_native_reap_quarantine()` 成功为条件，不能在仍有 KMT owner 时卸载 DLL。
借用 runtime 的测试入口要求调用者在 close/reap 成功之前保持 runtime 有效。

本阶段尚未完成 Gallium `fd_device/fd_pipe/fd_bo` 回调接入、WGL screen/present、
EGL/GLES、共享资源导入导出或安装包切换。Meson 的 Gallium/WDDM 门禁仍保留。
不得把本目录静态库当成 opengl32.dll，不得把 Zink/软件回退计为原生渲染成功。

## 已执行验证

2026-09-14，GitHub Actions run 34822218530：Linux 数据测试、Windows x64 和
ARM64 编译/链接全部通过。x64 运行实际 native owner、实际 KMT transport 和实际
packet encoder，OS 回调显式注入；ARM64 只编译和链接，没有在 x64 runner 执行。

`native_owner_test.cpp` 检查全存活驻留、CPU 映射、pending、失败创建、busy、地址
保留与复用、unlock 失败、parent 销毁顺序、隔离重试、Render 失败、成功但替换
缓冲区无效、reset、未来 fence，以及四线程共 800 次提交。它不是 GPU 渲染测试。

`timeline_test.c` 在 GCC/Clang ASan/UBSan 及 Windows x64 运行；覆盖 2054 次断言，
包括 32 位 wrap 边界、零值、倒退、未来、半范围与 64 位溢出拒绝。既有生产组包
29380 次断言和 fake-OS 生命周期 504957 次断言继续通过；不是同等数量独立用例。

运行入口仍为 `tests/build-native.ps1`。本次没有真实 GPU、OpenGL context、离屏
clear/draw/readback、窗口 swap 或性能验收结果。CI 的成功不替代这些验收。
