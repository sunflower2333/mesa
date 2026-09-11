// SPDX-License-Identifier: MIT
// Application-local ABI and bounded raster probe. No GPU calls in --load-only.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void fail(const char *message)
{
   std::fprintf(stderr, "FAIL: %s (Win32=%lu)\n", message, GetLastError());
   std::exit(1);
}

template<typename T> static T symbol(HMODULE module, const char *name)
{
   FARPROC address = GetProcAddress(module, name);
   if (!address) fail(name);
   // memcpy avoids MSVC's unrelated function pointer cast diagnostic.
   T result;
   static_assert(sizeof(result) == sizeof(address), "function pointer size");
   std::memcpy(&result, &address, sizeof(result));
   return result;
}

static HMODULE load(const char *name)
{
   char absolute[MAX_PATH];
   const DWORD length = GetFullPathNameA(name, MAX_PATH, absolute, nullptr);
   if (!length || length >= MAX_PATH) fail("absolute module path");
   HMODULE module = LoadLibraryExA(absolute, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
   if (!module) fail(name);
   char path[MAX_PATH];
   if (!GetModuleFileNameA(module, path, MAX_PATH)) fail("module path");
   std::printf("loaded %s\n", path);
   if (_stricmp(path, absolute)) fail("module resolved outside candidate directory");
   return module;
}

static HWND window()
{
   WNDCLASSA wc = {};
   wc.style = CS_OWNDC;
   wc.lpfnWndProc = DefWindowProcA;
   wc.hInstance = GetModuleHandleA(nullptr);
   wc.lpszClassName = "MesaArchitectureRasterProbe";
   if (!RegisterClassA(&wc)) fail("RegisterClass");
   HWND hwnd = CreateWindowA(wc.lpszClassName, "Zink architecture raster probe",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 160, 160,
                            nullptr, nullptr, wc.hInstance, nullptr);
   if (!hwnd) fail("CreateWindow");
   return hwnd;
}

static void renderer(HMODULE gl)
{
   auto getString = symbol<const GLubyte *(GL_APIENTRY *)(GLenum)>(gl, "glGetString");
   const char *name = reinterpret_cast<const char *>(getString(GL_RENDERER));
   const char *version = reinterpret_cast<const char *>(getString(GL_VERSION));
   std::printf("GL_RENDERER=%s\nGL_VERSION=%s\n", name ? name : "null", version ? version : "null");
   if (!name || !std::strstr(name, "zink") ||
       !(std::strstr(name, "Turnip") || std::strstr(name, "turnip") || std::strstr(name, "TURNIP")) ||
       std::strstr(name, "llvmpipe") || std::strstr(name, "softpipe"))
      fail("renderer is not accelerated Zink on Turnip");
}

static void readback(HMODULE gl)
{
   auto finish = symbol<void (GL_APIENTRY *)(void)>(gl, "glFinish");
   auto read = symbol<void (GL_APIENTRY *)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)>(gl, "glReadPixels");
   auto error = symbol<GLenum (GL_APIENTRY *)(void)>(gl, "glGetError");
   GLubyte pixel[4] = {};
   finish();
   read(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
   const GLenum status = error();
   std::printf("center RGBA=%u,%u,%u,%u GL_ERROR=%u\n", pixel[0], pixel[1], pixel[2], pixel[3], status);
   if (status != GL_NO_ERROR || pixel[0] < 240 || pixel[1] > 15 || pixel[2] > 15)
      fail("rasterized triangle readback mismatch");
}

static void clear(HMODULE gl)
{
   symbol<void (GL_APIENTRY *)(GLint, GLint, GLsizei, GLsizei)>(gl, "glViewport")(0, 0, 64, 64);
   symbol<void (GL_APIENTRY *)(GLfloat, GLfloat, GLfloat, GLfloat)>(gl, "glClearColor")(0, 0, 1, 1);
   symbol<void (GL_APIENTRY *)(GLbitfield)>(gl, "glClear")(GL_COLOR_BUFFER_BIT);
}

static void wgl(HMODULE gl)
{
   HWND hwnd = window();
   HDC dc = GetDC(hwnd);
   PIXELFORMATDESCRIPTOR pfd = {};
   pfd.nSize = sizeof(pfd);
   pfd.nVersion = 1;
   pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
   pfd.iPixelType = PFD_TYPE_RGBA;
   pfd.cColorBits = 32;
   auto choose = symbol<int (WINAPI *)(HDC, const PIXELFORMATDESCRIPTOR *)>(gl, "wglChoosePixelFormat");
   auto set = symbol<BOOL (WINAPI *)(HDC, int, const PIXELFORMATDESCRIPTOR *)>(gl, "wglSetPixelFormat");
   const int format = choose(dc, &pfd);
   if (!format || !set(dc, format, &pfd)) fail("WGL pixel format");
   HGLRC context = symbol<HGLRC (WINAPI *)(HDC)>(gl, "wglCreateContext")(dc);
   auto current = symbol<BOOL (WINAPI *)(HDC, HGLRC)>(gl, "wglMakeCurrent");
   if (!context || !current(dc, context)) fail("WGL context");
   renderer(gl);
   clear(gl);
   symbol<void (WINAPI *)(GLfloat, GLfloat, GLfloat)>(gl, "glColor3f")(1, 0, 0);
   symbol<void (WINAPI *)(GLenum)>(gl, "glBegin")(GL_TRIANGLES);
   auto vertex = symbol<void (WINAPI *)(GLfloat, GLfloat)>(gl, "glVertex2f");
   vertex(-1, -1); vertex(1, -1); vertex(0, 1);
   symbol<void (WINAPI *)(void)>(gl, "glEnd")();
   readback(gl);
   if (!symbol<BOOL (WINAPI *)(HDC)>(gl, "wglSwapBuffers")(dc)) fail("WGL swap");
   current(nullptr, nullptr);
   symbol<BOOL (WINAPI *)(HGLRC)>(gl, "wglDeleteContext")(context);
   ReleaseDC(hwnd, dc);
   DestroyWindow(hwnd);
}

static void gles(HMODULE egl, HMODULE gl, int version)
{
   HWND hwnd = window();
   auto display = symbol<PFNEGLGETDISPLAYPROC>(egl, "eglGetDisplay")(EGL_DEFAULT_DISPLAY);
   EGLint major = 0, minor = 0;
   if (!symbol<PFNEGLINITIALIZEPROC>(egl, "eglInitialize")(display, &major, &minor)) fail("eglInitialize");
   if (!symbol<PFNEGLBINDAPIPROC>(egl, "eglBindAPI")(EGL_OPENGL_ES_API)) fail("eglBindAPI");
   const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
      version == 2 ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_ES_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
   EGLConfig config;
   EGLint count = 0;
   if (!symbol<PFNEGLCHOOSECONFIGPROC>(egl, "eglChooseConfig")(display, attributes, &config, 1, &count) || count != 1)
      fail("eglChooseConfig");
   const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE};
   EGLContext context = symbol<PFNEGLCREATECONTEXTPROC>(egl, "eglCreateContext")(display, config, EGL_NO_CONTEXT, contextAttributes);
   EGLSurface surface = symbol<PFNEGLCREATEWINDOWSURFACEPROC>(egl, "eglCreateWindowSurface")(display, config, hwnd, nullptr);
   auto current = symbol<PFNEGLMAKECURRENTPROC>(egl, "eglMakeCurrent");
   if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE || !current(display, surface, surface, context)) fail("EGL context/surface");
   renderer(gl);
   clear(gl);
   const GLfloat vertices[] = {-1, -1, 1, -1, 0, 1};
   GLuint program = 0;
   if (version == 2) {
      const char *sources[] = {"attribute vec2 pos; void main(){ gl_Position=vec4(pos,0.0,1.0); }",
                               "precision mediump float; void main(){ gl_FragColor=vec4(1.0,0.0,0.0,1.0); }"};
      auto createShader = symbol<PFNGLCREATESHADERPROC>(gl, "glCreateShader");
      auto shaderSource = symbol<PFNGLSHADERSOURCEPROC>(gl, "glShaderSource");
      auto compileShader = symbol<PFNGLCOMPILESHADERPROC>(gl, "glCompileShader");
      auto shaderiv = symbol<PFNGLGETSHADERIVPROC>(gl, "glGetShaderiv");
      program = symbol<PFNGLCREATEPROGRAMPROC>(gl, "glCreateProgram")();
      for (int i = 0; i < 2; i++) {
         GLuint shader = createShader(i == 0 ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
         shaderSource(shader, 1, &sources[i], nullptr);
         compileShader(shader);
         GLint ok = 0;
         shaderiv(shader, GL_COMPILE_STATUS, &ok);
         if (!ok) fail("GLES shader compile");
         symbol<PFNGLATTACHSHADERPROC>(gl, "glAttachShader")(program, shader);
         symbol<PFNGLDELETESHADERPROC>(gl, "glDeleteShader")(shader);
      }
      symbol<PFNGLBINDATTRIBLOCATIONPROC>(gl, "glBindAttribLocation")(program, 0, "pos");
      symbol<PFNGLLINKPROGRAMPROC>(gl, "glLinkProgram")(program);
      GLint ok = 0;
      symbol<PFNGLGETPROGRAMIVPROC>(gl, "glGetProgramiv")(program, GL_LINK_STATUS, &ok);
      if (!ok) fail("GLES program link");
      symbol<PFNGLUSEPROGRAMPROC>(gl, "glUseProgram")(program);
      symbol<PFNGLVERTEXATTRIBPOINTERPROC>(gl, "glVertexAttribPointer")(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
      symbol<PFNGLENABLEVERTEXATTRIBARRAYPROC>(gl, "glEnableVertexAttribArray")(0);
   } else {
      symbol<void (GL_APIENTRY *)(GLfloat, GLfloat, GLfloat, GLfloat)>(gl, "glColor4f")(1, 0, 0, 1);
      symbol<void (GL_APIENTRY *)(GLint, GLenum, GLsizei, const void *)>(gl, "glVertexPointer")(2, GL_FLOAT, 0, vertices);
      symbol<void (GL_APIENTRY *)(GLenum)>(gl, "glEnableClientState")(0x8074 /* GL_VERTEX_ARRAY */);
   }
   symbol<void (GL_APIENTRY *)(GLenum, GLint, GLsizei)>(gl, "glDrawArrays")(GL_TRIANGLES, 0, 3);
   readback(gl);
   if (!symbol<PFNEGLSWAPBUFFERSPROC>(egl, "eglSwapBuffers")(display, surface)) fail("eglSwapBuffers");
   if (program) symbol<PFNGLDELETEPROGRAMPROC>(gl, "glDeleteProgram")(program);
   current(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   symbol<PFNEGLDESTROYSURFACEPROC>(egl, "eglDestroySurface")(display, surface);
   symbol<PFNEGLDESTROYCONTEXTPROC>(egl, "eglDestroyContext")(display, context);
   symbol<PFNEGLTERMINATEPROC>(egl, "eglTerminate")(display);
   DestroyWindow(hwnd);
}

int main(int argc, char **argv)
{
   if (argc != 2) fail("usage: opengl-probe --load-only|--wgl|--gles2|--gles1");
   HMODULE gl = load(".\\opengl32.dll");
   HMODULE egl = load(".\\libEGL.dll");
   HMODULE es1 = load(".\\libGLESv1_CM.dll");
   HMODULE es2 = load(".\\libGLESv2.dll");
   HMODULE vk = load(".\\vulkan-1.dll");
   HMODULE icd = load(".\\vulkan_freedreno.dll");
   symbol<PROC>(gl, "wglGetProcAddress");
   symbol<PROC>(egl, "eglGetProcAddress");
   symbol<PROC>(es1, "glDrawArrays");
   symbol<PROC>(es2, "glCreateShader");
   symbol<PROC>(vk, "vkGetInstanceProcAddr");
   symbol<PROC>(icd, "vk_icdGetInstanceProcAddr");
   if (!std::strcmp(argv[1], "--wgl")) wgl(gl);
   else if (!std::strcmp(argv[1], "--gles2")) gles(egl, es2, 2);
   else if (!std::strcmp(argv[1], "--gles1")) gles(egl, es1, 1);
   else if (std::strcmp(argv[1], "--load-only")) fail("unknown mode");
   std::printf("PASS %s (%zu-bit process)\n", argv[1], sizeof(void *) * 8);
   return 0;
}
