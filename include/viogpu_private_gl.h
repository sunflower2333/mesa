/* SPDX-License-Identifier: MIT
 * Names are compiled into import libraries and consumers, never patched in PE.
 */
#ifndef VIOGPU_PRIVATE_GL_H
#define VIOGPU_PRIVATE_GL_H
#if defined(_M_ARM64) && !defined(_M_ARM64EC)
#define VIOGPU_GL_ARCH "arm64"
#elif defined(_M_X64) || defined(_M_ARM64EC)
#define VIOGPU_GL_ARCH "x64"
#elif defined(_M_IX86)
#define VIOGPU_GL_ARCH "x86"
#else
#error Unsupported Windows GL runtime architecture
#endif
#define VIOGPU_GL_DLL "viogpu_gl_" VIOGPU_GL_ARCH ".dll"
#define VIOGPU_EGL_DLL "viogpu_egl_" VIOGPU_GL_ARCH ".dll"
#define VIOGPU_GLES1_DLL "viogpu_gles1_" VIOGPU_GL_ARCH ".dll"
#define VIOGPU_GLES2_DLL "viogpu_gles2_" VIOGPU_GL_ARCH ".dll"
#define VIOGPU_GL_VK_DLL "viogpu_gl_vk_" VIOGPU_GL_ARCH ".dll"
#define VIOGPU_GL_LOADER_DLL "viogpu_gl_loader_" VIOGPU_GL_ARCH ".dll"
#define VIOGPU_WIDE_INNER(x) L##x
#define VIOGPU_WIDE(x) VIOGPU_WIDE_INNER(x)
#endif
