/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 * App-local WGL smoke test. No driver installation or registry writes.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

// Resolve only from the explicitly loaded app-local OpenGL module.
template <typename T>
static T load_proc(HMODULE module, const char *name)
{
   FARPROC address = GetProcAddress(module, name);
   if (!address)
      throw std::runtime_error(std::string("missing OpenGL export: ") + name);
   T result;
   static_assert(sizeof(result) == sizeof(address), "function pointer size");
   std::memcpy(&result, &address, sizeof(result));
   return result;
}

// Keep the context, DC and module in dependency order, including failure paths.
struct probe_owner {
   HMODULE module = nullptr;
   HWND window = nullptr;
   HDC dc = nullptr;
   HGLRC context = nullptr;
   BOOL (WINAPI *make_current)(HDC, HGLRC) = nullptr;
   BOOL (WINAPI *delete_context)(HGLRC) = nullptr;
   bool registered = false;
   ~probe_owner()
   {
      if (make_current)
         make_current(nullptr, nullptr);
      if (context && delete_context)
         delete_context(context);
      if (dc)
         ReleaseDC(window, dc);
      if (window)
         DestroyWindow(window);
      if (registered)
         UnregisterClassW(L"DroidVMZinkProbe", GetModuleHandleW(nullptr));
      if (module)
         FreeLibrary(module);
   }
};

// Require a real Zink/Turnip renderer rather than a software or system-GL fallback.
static bool is_zink_turnip(const char *renderer)
{
   return renderer && std::strstr(renderer, "zink") &&
          (std::strstr(renderer, "MESA_TURNIP") || std::strstr(renderer, "Turnip"));
}

// Compare a readback against an unambiguous 8-bit red or blue pixel.
static void check_pixel(const unsigned char pixel[4], bool red)
{
   if (pixel[0] != (red ? 255 : 0) || pixel[1] != 0 || pixel[2] != (red ? 0 : 255))
      throw std::runtime_error("pixel readback mismatch");
}

// Exercise actual WGL clear, fixed-function draw, readback, and presentation.
static int render_probe()
{
   wchar_t executable[32768];
   DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
   if (!length || length >= 32768)
      throw std::runtime_error("cannot locate probe executable");
   std::wstring path(executable, length);
   path.resize(path.find_last_of(L"\\/") + 1);
   path += L"opengl32.dll";
   probe_owner owner;
   owner.module = LoadLibraryExW(path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
   if (!owner.module)
      throw std::runtime_error("cannot load app-local OpenGL DLL or a dependency");

   const auto choose = load_proc<int (WINAPI *)(HDC, const PIXELFORMATDESCRIPTOR *)>(owner.module, "wglChoosePixelFormat");
   const auto set = load_proc<BOOL (WINAPI *)(HDC, int, const PIXELFORMATDESCRIPTOR *)>(owner.module, "wglSetPixelFormat");
   const auto create = load_proc<HGLRC (WINAPI *)(HDC)>(owner.module, "wglCreateContext");
   const auto swap = load_proc<BOOL (WINAPI *)(HDC)>(owner.module, "wglSwapBuffers");
   owner.make_current = load_proc<BOOL (WINAPI *)(HDC, HGLRC)>(owner.module, "wglMakeCurrent");
   owner.delete_context = load_proc<BOOL (WINAPI *)(HGLRC)>(owner.module, "wglDeleteContext");
   const auto get_string = load_proc<const GLubyte *(APIENTRY *)(GLenum)>(owner.module, "glGetString");
   const auto error = load_proc<GLenum (APIENTRY *)()>(owner.module, "glGetError");
   const auto clear_color = load_proc<void (APIENTRY *)(GLfloat, GLfloat, GLfloat, GLfloat)>(owner.module, "glClearColor");
   const auto clear = load_proc<void (APIENTRY *)(GLbitfield)>(owner.module, "glClear");
   const auto read = load_proc<void (APIENTRY *)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)>(owner.module, "glReadPixels");
   const auto disable = load_proc<void (APIENTRY *)(GLenum)>(owner.module, "glDisable");
   const auto viewport = load_proc<void (APIENTRY *)(GLint, GLint, GLsizei, GLsizei)>(owner.module, "glViewport");
   const auto begin = load_proc<void (APIENTRY *)(GLenum)>(owner.module, "glBegin");
   const auto end = load_proc<void (APIENTRY *)()>(owner.module, "glEnd");
   const auto color = load_proc<void (APIENTRY *)(GLfloat, GLfloat, GLfloat)>(owner.module, "glColor3f");
   const auto vertex = load_proc<void (APIENTRY *)(GLfloat, GLfloat)>(owner.module, "glVertex2f");

   WNDCLASSW wc = {};
   wc.style = CS_OWNDC;
   wc.lpfnWndProc = DefWindowProcW;
   wc.hInstance = GetModuleHandleW(nullptr);
   wc.lpszClassName = L"DroidVMZinkProbe";
   if (!RegisterClassW(&wc))
      throw std::runtime_error("cannot register probe window");
   owner.registered = true;
   owner.window = CreateWindowW(wc.lpszClassName, L"DroidVM Zink probe",
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 128, 128,
      nullptr, nullptr, wc.hInstance, nullptr);
   if (!owner.window || !(owner.dc = GetDC(owner.window)))
      throw std::runtime_error("cannot create probe window/DC");
   PIXELFORMATDESCRIPTOR pfd = {};
   pfd.nSize = sizeof(pfd);
   pfd.nVersion = 1;
   pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
   pfd.iPixelType = PFD_TYPE_RGBA;
   pfd.cColorBits = 32;
   pfd.cAlphaBits = 8;
   int format = choose(owner.dc, &pfd);
   if (!format || !set(owner.dc, format, &pfd))
      throw std::runtime_error("app-local pixel format selection failed");
   owner.context = create(owner.dc);
   if (!owner.context || !owner.make_current(owner.dc, owner.context))
      throw std::runtime_error("Zink WGL context creation failed");
   const char *renderer = reinterpret_cast<const char *>(get_string(GL_RENDERER));
   const char *version = reinterpret_cast<const char *>(get_string(GL_VERSION));
   std::printf("renderer=%s\nversion=%s\n", renderer ? renderer : "(null)", version ? version : "(null)");
   if (!is_zink_turnip(renderer))
      throw std::runtime_error("expected Zink over Turnip, refusing fallback result");
   ShowWindow(owner.window, SW_SHOWNOACTIVATE);
   viewport(0, 0, 32, 32);
   disable(GL_DITHER);
   for (unsigned iteration = 0; iteration < 16; ++iteration) {
      bool red = (iteration & 1) != 0;
      clear_color(red ? 1.0f : 0.0f, 0.0f, red ? 0.0f : 1.0f, 1.0f);
      clear(GL_COLOR_BUFFER_BIT);
      unsigned char pixel[4] = {};
      read(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
      check_pixel(pixel, red);
      color(red ? 0.0f : 1.0f, 0.0f, red ? 1.0f : 0.0f);
      begin(GL_TRIANGLES);
      vertex(-1.0f, -1.0f);
      vertex(1.0f, -1.0f);
      vertex(0.0f, 1.0f);
      end();
      read(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
      check_pixel(pixel, !red);
      if (error() != GL_NO_ERROR || !swap(owner.dc))
         throw std::runtime_error("GL error or swap failure");
      MSG message;
      while (PeekMessageW(&message, owner.window, 0, 0, PM_REMOVE)) {
         TranslateMessage(&message);
         DispatchMessageW(&message);
      }
   }
   if (!owner.make_current(nullptr, nullptr) || !owner.delete_context(owner.context))
      throw std::runtime_error("WGL context teardown failed");
   owner.context = nullptr;
   std::puts("PASS: 16 clears, 16 triangle draws, 32 pixel checks, 16 swaps");
   std::puts("This is not conformance, an A8xx capability claim, or a performance measurement.");
   return 0;
}

// Default mode is inert; only --render performs GPU/display operations.
int main(int argc, char **argv)
{
   if (argc == 1 || (argc == 2 && std::strcmp(argv[1], "--describe") == 0)) {
      std::puts("DroidVM app-local ARM64 Zink/Turnip WGL probe; use run-probe.ps1 for rendering.");
      return 0;
   }
   if (argc != 2 || std::strcmp(argv[1], "--render") != 0) {
      std::fputs("usage: zink_wgl_probe.exe [--describe|--render]\n", stderr);
      return 2;
   }
   try {
      return render_probe();
   } catch (const std::exception &error) {
      std::fprintf(stderr, "FAIL: %s\nWin32 error=%lu\n", error.what(), GetLastError());
      return 1;
   }
}
