// r6-port: native window for a locally translated PlayStation 1 title.
//
// This front end runs the same generated blocks as r6-native, but continuously
// and with real platform services: the user's own disc image supplies the
// executable and streamed data, the recompiled code executes natively, and the
// GPU display area is presented through an OpenGL 3.3 shader.
//
// There is no interpreter and no CPU emulation here. Each guest instruction is
// already a C++ statement in the generated translation unit. The only guest
// code that can run is code the translation covered; anything else stops with a
// recompilation-miss report instead of being interpreted.
//
// Build with -DR6_GENERATED_TRANSLATION=<generated.cpp> (target r6-port).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

#include <GL/gl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "r6/boot_image.h"
#include "r6/memory.h"
#include "r6/native_runtime.h"
#include "r6/ram_snapshot.h"
#include "r6/windows_platform_services.h"

#ifdef R6_HAS_GENERATED_TRANSLATION
namespace r6_generated {
void register_functions(r6::NativeBlockDispatcher& dispatcher);
std::size_t block_count();
}  // namespace r6_generated
#endif

// --------------------------------------------------------------- GL 3.3 loader

namespace {

using GlChar = char;
using GlSizeiptr = std::ptrdiff_t;
using GlIntptr = std::ptrdiff_t;

constexpr GLenum kGlVertexShader = 0x8B31;
constexpr GLenum kGlFragmentShader = 0x8B30;
constexpr GLenum kGlCompileStatus = 0x8B81;
constexpr GLenum kGlLinkStatus = 0x8B82;
constexpr GLenum kGlArrayBuffer = 0x8892;
constexpr GLenum kGlStaticDraw = 0x88E4;
constexpr GLenum kGlTexture0 = 0x84C0;
constexpr GLenum kGlBgra = 0x80E1;
constexpr GLenum kGlClampToEdge = 0x812F;
constexpr GLenum kGlTriangles = 0x0004;
constexpr GLint kGlInfoLogLength = 0x8B84;
constexpr int kWglContextMajor = 0x2091;
constexpr int kWglContextMinor = 0x2092;
constexpr int kWglContextProfileMask = 0x9126;
constexpr int kWglContextCoreProfile = 0x00000001;

struct Gl {
    HGLRC (WINAPI* create_context_attribs)(HDC, HGLRC, const int*) = nullptr;
    void (APIENTRY* gen_buffers)(GLsizei, GLuint*) = nullptr;
    void (APIENTRY* bind_buffer)(GLenum, GLuint) = nullptr;
    void (APIENTRY* buffer_data)(GLenum, GlSizeiptr, const void*, GLenum) = nullptr;
    void (APIENTRY* gen_vertex_arrays)(GLsizei, GLuint*) = nullptr;
    void (APIENTRY* bind_vertex_array)(GLuint) = nullptr;
    void (APIENTRY* vertex_attrib_pointer)(GLuint, GLint, GLenum, GLboolean,
                                           GLsizei, const void*) = nullptr;
    void (APIENTRY* enable_vertex_attrib_array)(GLuint) = nullptr;
    GLuint (APIENTRY* create_shader)(GLenum) = nullptr;
    void (APIENTRY* shader_source)(GLuint, GLsizei, const GlChar* const*,
                                   const GLint*) = nullptr;
    void (APIENTRY* compile_shader)(GLuint) = nullptr;
    void (APIENTRY* get_shaderiv)(GLuint, GLenum, GLint*) = nullptr;
    void (APIENTRY* get_shader_info_log)(GLuint, GLsizei, GLsizei*,
                                         GlChar*) = nullptr;
    GLuint (APIENTRY* create_program)() = nullptr;
    void (APIENTRY* attach_shader)(GLuint, GLuint) = nullptr;
    void (APIENTRY* link_program)(GLuint) = nullptr;
    void (APIENTRY* get_programiv)(GLuint, GLenum, GLint*) = nullptr;
    void (APIENTRY* get_program_info_log)(GLuint, GLsizei, GLsizei*,
                                          GlChar*) = nullptr;
    void (APIENTRY* use_program)(GLuint) = nullptr;
    void (APIENTRY* delete_shader)(GLuint) = nullptr;
    GLint (APIENTRY* get_uniform_location)(GLuint, const GlChar*) = nullptr;
    void (APIENTRY* uniform1f)(GLint, GLfloat) = nullptr;
    void (APIENTRY* uniform2f)(GLint, GLfloat, GLfloat) = nullptr;
    void (APIENTRY* uniform1i)(GLint, GLint) = nullptr;
    void (APIENTRY* active_texture)(GLenum) = nullptr;

    template <typename Function>
    bool load(Function& function, const char* name) {
        function = reinterpret_cast<Function>(
            wglGetProcAddress(name));
        if (function != nullptr) return true;
        std::cerr << "r6-port: missing OpenGL entry point " << name << "\n";
        return false;
    }
};

const char* const kDefaultVertexShader = R"(#version 330 core
layout(location = 0) in vec2 a_position;
out vec2 v_uv;
void main() {
    v_uv = vec2(a_position.x * 0.5 + 0.5, 0.5 - a_position.y * 0.5);
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)";

// The guest renders 15-bit pixels into PS1 VRAM; the host presents that
// display area. The shader does the native-side work: nearest sampling, an
// optional scanline mask, and gamma so 5-bit colours expand correctly.
const char* const kDefaultFragmentShader = R"(#version 330 core
uniform sampler2D u_texture;
uniform vec2 u_source_size;
uniform float u_scanlines;
uniform float u_gamma;
in vec2 v_uv;
out vec4 o_color;
void main() {
    vec2 pixel = v_uv * u_source_size;
    vec2 uv = (floor(pixel) + 0.5) / u_source_size;
    vec3 color = texture(u_texture, uv).rgb;
    float line = 0.5 + 0.5 * cos(pixel.y * 3.14159265);
    color *= mix(1.0, line, u_scanlines);
    color = pow(max(color, vec3(0.0)), vec3(1.0 / u_gamma));
    o_color = vec4(color, 1.0);
}
)";

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

GLuint compile_shader(const Gl& gl, GLenum type, const std::string& source,
                      const char* label) {
    const GLuint shader = gl.create_shader(type);
    const GlChar* text = source.c_str();
    gl.shader_source(shader, 1, &text, nullptr);
    gl.compile_shader(shader);
    GLint status = 0;
    gl.get_shaderiv(shader, kGlCompileStatus, &status);
    if (status == 0) {
        char log[2048]{};
        gl.get_shader_info_log(shader, sizeof(log), nullptr, log);
        std::cerr << "r6-port: " << label << " shader failed:\n" << log << "\n";
    }
    return shader;
}

GLuint build_program(const Gl& gl, const std::string& vertex,
                     const std::string& fragment) {
    const GLuint vs = compile_shader(gl, kGlVertexShader, vertex, "vertex");
    const GLuint fs = compile_shader(gl, kGlFragmentShader, fragment, "fragment");
    const GLuint program = gl.create_program();
    gl.attach_shader(program, vs);
    gl.attach_shader(program, fs);
    gl.link_program(program);
    GLint status = 0;
    gl.get_programiv(program, kGlLinkStatus, &status);
    if (status == 0) {
        char log[2048]{};
        gl.get_program_info_log(program, sizeof(log), nullptr, log);
        std::cerr << "r6-port: link failed:\n" << log << "\n";
    }
    gl.delete_shader(vs);
    gl.delete_shader(fs);
    return program;
}

// ------------------------------------------------------------------- host

constexpr std::uint32_t kRamSize = 2u * 1024u * 1024u;
constexpr std::uint32_t kRamMask = kRamSize - 1u;
constexpr std::uint32_t kDefaultStack = 0x801FFFF0u;
// This title's C runtime reads its stack limit from .data and computes a stack
// outside main RAM. The retail loader supplies a usable one.
constexpr std::uint32_t kCrStackGlobal = 0x800B3E70u;
constexpr std::uint64_t kFieldCycles = 564480u;  // 33.8688 MHz / 60
constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 720;

bool is_bios_entry(std::uint32_t pc) {
    const auto physical = pc & 0x1FFFFFFFu;
    return physical == 0xA0u || physical == 0xB0u || physical == 0xC0u;
}

std::string hex2(std::uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    std::string text(2, '0');
    text[0] = digits[(value >> 4) & 0xFu];
    text[1] = digits[value & 0xFu];
    return text;
}

std::string hex32(std::uint32_t value) {
    char text[16]{};
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

std::filesystem::path own_directory() {
    wchar_t buffer[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path find_image(const std::filesystem::path& explicit_path) {
    if (!explicit_path.empty()) return explicit_path;
    const auto here = own_directory();
    std::error_code error;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(
             here, std::filesystem::directory_options::skip_permission_denied,
             error)) {
        if (error) break;
        const auto extension = entry.path().extension().string();
        if (extension == ".chd" || extension == ".cue" || extension == ".bin") {
            return entry.path();
        }
    }
    return {};
}

struct Host {
    std::filesystem::path image;
    std::unique_ptr<r6::WindowsPlatformServices> services;
    r6::Memory ram{kRamSize, true};
    r6::CpuState cpu{};
    r6::NativeBlockDispatcher dispatcher;
    std::unique_ptr<r6::NativeBus> bus;
    r6::boot::ExeImage exe;
    std::uint64_t blocks{0};
    bool halted{false};
    bool trace{false};
    std::unordered_map<std::uint32_t, std::uint64_t> histogram;
    bool bios_log{false};
    std::vector<std::uint32_t> watch;
    std::string stop_reason;
};

bool load_boot(Host& host, std::string& error) {
    host.services =
        std::make_unique<r6::WindowsPlatformServices>(host.image);
    if (!host.services->disc_open()) {
        error = "disc image could not be opened: " +
                host.services->disc_error();
        return false;
    }
    const auto files = r6::boot::list_root(*host.services);
    if (files.empty()) {
        error = "no ISO9660 volume in " + host.image.string();
        return false;
    }

    std::string exe_name;
    for (const auto& file : files) {
        if (r6::boot::upper(file.name) == "SYSTEM.CNF") {
            exe_name =
                r6::boot::boot_name(r6::boot::read_file(*host.services, file));
        }
    }
    if (exe_name.empty()) {
        for (const auto& file : files) {
            const auto name = r6::boot::upper(file.name);
            for (const char* prefix : {"SLUS", "SCUS", "SLES", "SCES", "SLPS",
                                       "SLPM", "SCPS"}) {
                if (name.rfind(prefix, 0) == 0) exe_name = file.name;
            }
            if (!exe_name.empty()) break;
        }
    }
    const r6::boot::DiscFile* executable = nullptr;
    for (const auto& file : files) {
        if (r6::boot::upper(file.name) == r6::boot::upper(exe_name)) {
            executable = &file;
            break;
        }
    }
    if (executable == nullptr ||
        !r6::boot::parse_exe(
            r6::boot::read_file(*host.services, *executable), host.exe)) {
        error = "the boot executable could not be loaded";
        return false;
    }

    for (std::size_t index = 0; index < host.exe.payload.size(); ++index) {
        host.ram.write8(
            (host.exe.load_address + static_cast<std::uint32_t>(index)) &
                kRamMask,
            host.exe.payload[index]);
    }
    const auto cr_stack = host.ram.read32(kCrStackGlobal & kRamMask);
    const bool stack_in_ram =
        cr_stack >= 0x80000000u && cr_stack < 0x80200000u;
    if (!stack_in_ram) {
        host.ram.write32(kCrStackGlobal & kRamMask, kDefaultStack);
    }

    host.cpu = r6::CpuState{};
    host.cpu.pc = host.exe.entry;
    host.cpu.gpr[28] = host.exe.gp;
    host.cpu.gpr[29] = host.exe.stack_base != 0 ? host.exe.stack_base
                                                : kDefaultStack;
    host.cpu.gpr[31] = 0;
    // The retail kernel hands over SR with the global enable and the hardware
    // (IP2) mask set; translated code read-modify-writes it for COP2.
    host.cpu.cop0[12] = 0x00000401u;
    return true;
}

// Runs one dispatcher step. BIOS traps are the platform-service seam, exactly
// as in r6-native: poll events, call the kernel service, resume at RA.
bool step_once(Host& host) {
    auto& cpu = host.cpu;
    auto& bus = *host.bus;
    if (is_bios_entry(cpu.pc)) {
        if (bus.event_tick(cpu, true)) return true;
        const auto bios_pc = cpu.pc;
        const auto bios_call_number = cpu.gpr[9];
        const auto bios_a0 = cpu.gpr[4];
        const auto bios_a1 = cpu.gpr[5];
        const auto bios_a2 = cpu.gpr[6];
        const bool bios_handled =
            host.services->bios_call(cpu.pc, cpu, host.ram);
        const auto bios_v0 = cpu.gpr[2];
        if (!bios_handled) {
            host.stop_reason = "unhandled BIOS entry " + hex32(cpu.pc);
            host.halted = true;
            return false;
        }
        if (!bus.take_bios_return(cpu)) cpu.pc = cpu.gpr[31];
        if (host.bios_log) {
            std::cout << "BC " << bus.hardware().cycles() << " "
                      << hex32(bios_pc) << " t1=" << hex32(bios_call_number)
                      << " a0=" << hex32(bios_a0)
                      << " a1=" << hex32(bios_a1)
                      << " a2=" << hex32(bios_a2)
                      << " v0=" << hex32(bios_v0) << "\n";
        }
        ++host.blocks;
        return true;
    }
    if (!host.dispatcher.contains(cpu.pc)) {
        host.stop_reason = "no translated block at " + hex32(cpu.pc);
        host.halted = true;
        return false;
    }
    try {
        cpu.pc = host.dispatcher.step(cpu, bus);
        // A no-op unless block-boundary delivery is enabled; in the default
        // mode the generated blocks take interrupts per instruction and
        // resume through their interior entry points.
        bus.service_interrupt(cpu);
    } catch (const std::exception& exception) {
        host.stop_reason = std::string("native fault at ") + hex32(cpu.pc) +
                           ": " + exception.what();
        host.halted = true;
        return false;
    }
    ++host.blocks;
    if (host.trace) ++host.histogram[cpu.pc & ~3u];
    return true;
}

// --------------------------------------------------------------- window

struct WindowState {
    Host* host = nullptr;
    HWND window = nullptr;
    HDC dc = nullptr;
    HGLRC gl_context = nullptr;
    GLuint program = 0;
    GLuint texture = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLint source_size = -1;
    GLint scanlines = -1;
    GLint gamma = -1;
    bool running = true;
    bool show_fps = true;
    double fps = 0.0;
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    switch (message) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                DestroyWindow(window);
                return 0;
            }
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

bool create_gl(WindowState& state, Gl& gl) {
    PIXELFORMATDESCRIPTOR descriptor{};
    descriptor.nSize = sizeof(descriptor);
    descriptor.nVersion = 1;
    descriptor.dwFlags =
        PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    descriptor.iPixelType = PFD_TYPE_RGBA;
    descriptor.cColorBits = 32;
    descriptor.cDepthBits = 0;
    const int format = ChoosePixelFormat(state.dc, &descriptor);
    if (format == 0 || !SetPixelFormat(state.dc, format, &descriptor)) {
        std::cerr << "r6-port: no suitable pixel format\n";
        return false;
    }
    const HGLRC legacy = wglCreateContext(state.dc);
    if (legacy == nullptr) {
        std::cerr << "r6-port: wglCreateContext failed\n";
        return false;
    }
    wglMakeCurrent(state.dc, legacy);
    gl.create_context_attribs =
        reinterpret_cast<decltype(gl.create_context_attribs)>(
            wglGetProcAddress("wglCreateContextAttribsARB"));
    if (gl.create_context_attribs != nullptr) {
        const int attribs[] = {kWglContextMajor,
                               3,
                               kWglContextMinor,
                               3,
                               kWglContextProfileMask,
                               kWglContextCoreProfile,
                               0};
        const auto context = gl.create_context_attribs(state.dc, nullptr, attribs);
        if (context != nullptr) {
            wglMakeCurrent(state.dc, context);
            wglDeleteContext(legacy);
            state.gl_context = context;
        } else {
            state.gl_context = legacy;
        }
    } else {
        state.gl_context = legacy;
    }
    std::cout << "OpenGL: " << reinterpret_cast<const char*>(glGetString(GL_VERSION))
              << " / " << reinterpret_cast<const char*>(glGetString(GL_RENDERER))
              << "\n";
    return true;
}

bool load_gl_functions(Gl& gl) {
    return gl.load(gl.gen_buffers, "glGenBuffers") &&
           gl.load(gl.bind_buffer, "glBindBuffer") &&
           gl.load(gl.buffer_data, "glBufferData") &&
           gl.load(gl.gen_vertex_arrays, "glGenVertexArrays") &&
           gl.load(gl.bind_vertex_array, "glBindVertexArray") &&
           gl.load(gl.vertex_attrib_pointer, "glVertexAttribPointer") &&
           gl.load(gl.enable_vertex_attrib_array,
                   "glEnableVertexAttribArray") &&
           gl.load(gl.create_shader, "glCreateShader") &&
           gl.load(gl.shader_source, "glShaderSource") &&
           gl.load(gl.compile_shader, "glCompileShader") &&
           gl.load(gl.get_shaderiv, "glGetShaderiv") &&
           gl.load(gl.get_shader_info_log, "glGetShaderInfoLog") &&
           gl.load(gl.create_program, "glCreateProgram") &&
           gl.load(gl.attach_shader, "glAttachShader") &&
           gl.load(gl.link_program, "glLinkProgram") &&
           gl.load(gl.get_programiv, "glGetProgramiv") &&
           gl.load(gl.get_program_info_log, "glGetProgramInfoLog") &&
           gl.load(gl.use_program, "glUseProgram") &&
           gl.load(gl.delete_shader, "glDeleteShader") &&
           gl.load(gl.get_uniform_location, "glGetUniformLocation") &&
           gl.load(gl.uniform1f, "glUniform1f") &&
           gl.load(gl.uniform2f, "glUniform2f") &&
           gl.load(gl.uniform1i, "glUniform1i") &&
           gl.load(gl.active_texture, "glActiveTexture");
}


// A 5x7 bitmap font, just enough for the on-screen counter. The overlay is
// drawn into the presented copy of the guest frame, so it never touches VRAM.
const std::uint8_t* glyph_rows(char glyph) {
    static const std::uint8_t kDigits[10][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
        {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
        {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
        {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
        {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
    };
    static const std::uint8_t kF[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    static const std::uint8_t kP[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    static const std::uint8_t kS[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    static const std::uint8_t kSpace[7] = {0, 0, 0, 0, 0, 0, 0};
    static const std::uint8_t kDot[7] = {0, 0, 0, 0, 0, 0x0C, 0x0C};
    if (glyph >= '0' && glyph <= '9') return kDigits[glyph - '0'];
    if (glyph == 'F') return kF;
    if (glyph == 'P') return kP;
    if (glyph == 'S') return kS;
    if (glyph == '.') return kDot;
    return kSpace;
}

void draw_overlay_text(std::vector<std::uint32_t>& pixels, int x, int y,
                       const std::string& text, std::uint32_t colour,
                       int scale) {
    constexpr int kWidth = 320;
    constexpr int kHeight = 240;
    int cursor = x;
    for (const char glyph : text) {
        const auto* rows = glyph_rows(glyph);
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if ((rows[row] & (1u << (4 - column))) == 0) continue;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        const int px = cursor + column * scale + dx;
                        const int py = y + row * scale + dy;
                        if (px < 0 || py < 0 || px >= kWidth || py >= kHeight) {
                            continue;
                        }
                        pixels[static_cast<std::size_t>(py) * kWidth + px] =
                            colour;
                    }
                }
            }
        }
        cursor += 6 * scale;
    }
}

void present(WindowState& state, Gl& gl, bool scanline_effect) {
    auto frame = state.host->services->gpu().display_frame();
    if (state.show_fps) {
        const auto text = std::to_string(static_cast<int>(state.fps + 0.5)) + " FPS";
        draw_overlay_text(frame, 5, 5, text, 0xFF000000u, 2);
        draw_overlay_text(frame, 4, 4, text, 0xFFFFFFFFu, 2);
    }
    static const std::uint32_t kWidth = 320;
    static const std::uint32_t kHeight = 240;

    glViewport(0, 0, kWindowWidth, kWindowHeight);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    gl.active_texture(kGlTexture0);
    glBindTexture(GL_TEXTURE_2D, state.texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(kWidth),
                 static_cast<GLsizei>(kHeight), 0, kGlBgra,
                 GL_UNSIGNED_BYTE, frame.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kGlClampToEdge);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kGlClampToEdge);

    gl.use_program(state.program);
    gl.uniform1i(gl.get_uniform_location(state.program, "u_texture"), 0);
    gl.uniform2f(state.source_size, static_cast<GLfloat>(kWidth),
                 static_cast<GLfloat>(kHeight));
    gl.uniform1f(state.scanlines, scanline_effect ? 0.25f : 0.0f);
    gl.uniform1f(state.gamma, 1.1f);
    gl.bind_vertex_array(state.vao);
    glDrawArrays(kGlTriangles, 0, 6);
    SwapBuffers(state.dc);
}

// Loads pc/shaders/psx.vert and psx.frag when present so a user can tune the
// presentation without rebuilding; the embedded pair is the fallback.
void load_shaders(const std::filesystem::path& directory, std::string& vertex,
                  std::string& fragment) {
    vertex = kDefaultVertexShader;
    fragment = kDefaultFragmentShader;
    const auto custom_vertex = read_text_file(directory / "shaders" / "psx.vert");
    const auto custom_fragment =
        read_text_file(directory / "shaders" / "psx.frag");
    if (!custom_vertex.empty()) vertex = custom_vertex;
    if (!custom_fragment.empty()) fragment = custom_fragment;
}

int run_headless(Host& host, std::uint64_t frames,
                 const std::filesystem::path& screenshot,
                 std::uint64_t max_cycles, std::uint64_t pc_from,
                 std::uint64_t pc_to,
                 const std::filesystem::path& dump_state,
                 std::uint32_t watch_change, bool cd_log,
                 bool watch_cd, bool walk_trace, bool sync_trace) {
    std::uint64_t presented = 0;
    std::uint64_t sector_last = 0;
    std::uint32_t watch_last =
        watch_change != 0 ? host.ram.read32(watch_change) : 0;
    while (!host.halted && presented < frames &&
           (max_cycles == 0 ||
            host.bus->hardware().cycles() < max_cycles)) {
        const auto target = host.bus->hardware().cycles() + kFieldCycles;
        while (!host.halted && host.bus->hardware().cycles() < target &&
               (max_cycles == 0 ||
                host.bus->hardware().cycles() < max_cycles)) {
            const auto before = host.cpu.pc;
            if (!step_once(host)) break;
            if (sync_trace) {
                static std::uint32_t last_istat = 0;
                const auto istat = host.bus->hardware().interrupt_status();
                if (istat != last_istat) {
                    std::cout << "IS " << host.bus->hardware().cycles()
                              << " stat=" << hex32(istat)
                              << " mask="
                              << hex32(host.bus->hardware().interrupt_mask())
                              << std::endl;
                    last_istat = istat;
                }
            }
            if (sync_trace &&
                (before == 0x8008A068u || before == 0x80086ABCu ||
                 before == 0x80086AB0u || before == 0x80086AB4u)) {
                std::cout << "SY " << host.bus->hardware().cycles() << " pc="
                          << hex32(before) << " a0=" << hex32(host.cpu.gpr[4])
                          << " a1=" << hex32(host.cpu.gpr[5])
                          << " ra=" << hex32(host.cpu.gpr[31])
                          << " v0=" << hex32(host.cpu.gpr[2])
                          << " s0=" << hex32(host.cpu.gpr[16])
                          << " s1=" << hex32(host.cpu.gpr[17])
                          << " cd3C=" << hex32(host.ram.read32(0x800B1C3Cu))
                          << " cd50=" << hex32(host.ram.read32(0x800B1C50u))
                          << " cd54=" << hex32(host.ram.read32(0x800B1C54u))
                          << " cd58=" << hex32(host.ram.read32(0x800B1C58u))
                          << " cd60=" << hex32(host.ram.read32(0x800B1C60u))
                          << " vs=" << hex32(host.ram.read32(0x800B397Cu))
                          << std::endl;
            }
            if (walk_trace && before == 0x800868B4u) {
                static int walk_records = 0;
                if (walk_records < 200) {
                    ++walk_records;
                    const auto s0 = host.cpu.gpr[16];
                    std::cout << "WK " << host.bus->hardware().cycles()
                              << " s0=" << hex32(s0)
                              << " n=" << host.cpu.gpr[18] << " bytes=";
                    for (std::uint32_t i = 0; i < 24; ++i) {
                        std::cout << hex2(host.ram.read8((s0 + i) & 0x1FFFFFu));
                    }
                    std::cout << std::endl;
                }
            }
            if (watch_cd) {
                const auto sectors = host.bus->hardware().cdrom().sectors_read();
                if (sectors != sector_last) {
                    std::cout << "SC " << host.bus->hardware().cycles() << " "
                              << hex32(before) << " " << sectors << "\n";
                    sector_last = sectors;
                }
            }
            if (watch_change != 0) {
                const auto value = host.ram.read32(watch_change);
                if (value != watch_last) {
                    std::cout << "WC " << host.bus->hardware().cycles() << " "
                              << hex32(before) << " " << hex32(value) << "\n";
                    watch_last = value;
                }
            }
            if (pc_from != 0) {
                const auto now = host.bus->hardware().cycles();
                if (now >= pc_from && now <= pc_to) {
                    std::cout << now << " " << hex32(host.cpu.pc)
                              << " v0=" << hex32(host.cpu.gpr[2])
                              << " v1=" << hex32(host.cpu.gpr[3])
                              << " a0=" << hex32(host.cpu.gpr[4]) << "\n";
                }
            }
        }
        ++presented;
    }
    const auto& gpu = host.services->gpu();
    std::cout << "Headless: " << presented << " field(s), " << host.blocks
              << " block(s), PC " << hex32(host.cpu.pc) << "\n"
              << "GPU: " << gpu.rendered_primitives() << " primitive(s), "
              << gpu.filled_vram_pixels() << " VRAM pixel(s), display "
              << (gpu.display_enabled() ? "on" : "off") << "\n";
    if (!host.stop_reason.empty()) {
        std::cout << "Stop: " << host.stop_reason << "\n";
    }
    if (!dump_state.empty()) {
        if (r6::write_snapshot(dump_state, host.cpu, host.ram)) {
            std::cout << "Wrote state " << dump_state.string() << " (PC "
                      << hex32(host.cpu.pc) << ")\n";
        }
    }
    if (!screenshot.empty()) {
        auto frame = gpu.display_frame();
        draw_overlay_text(frame, 5, 5, "60 FPS", 0xFF000000u, 2);
        draw_overlay_text(frame, 4, 4, "60 FPS", 0xFFFFFFFFu, 2);
        r6::boot::write_ppm(frame, 320, 240, screenshot);
        std::cout << "Wrote " << screenshot.string() << "\n";
    }
    if (host.bus) {
        auto& bus = *host.bus;
        std::cout << "IRQ: " << bus.interrupts() << " delivered, "
                  << bus.interrupt_overruns() << " overrun(s), "
                  << (bus.interrupts_enabled() ? "enabled" : "DISABLED")
                  << "\n";
        std::cout << "CD: " << bus.hardware().cdrom().sectors_read()
                  << " sector(s), last " << bus.hardware().cdrom().sector()
                  << "\n";
        std::cout << "Clock: " << bus.hardware().cycles() << " cycle(s), "
                  << bus.hardware().interrupt_status() << " I_STAT, "
                  << bus.hardware().interrupt_mask() << " I_MASK\n";
        const auto& bios = host.services->bios();
        std::cout << "BIOS calls: " << bios.total_calls() << "\n";
        for (const auto& entry : bios.calls()) {
            const auto table = static_cast<r6::BiosTable>(entry.first.first);
            const char name = table == r6::BiosTable::a0
                                  ? 'A'
                                  : (table == r6::BiosTable::b0 ? 'B' : 'C');
            std::cout << "  " << name << "(0x" << std::hex << entry.first.second
                      << std::dec << ") x" << entry.second << "\n";
        }
        if (cd_log) {
            for (const auto& entry : bus.hardware().cdrom().log()) {
                std::cout << "  " << entry << "\n";
            }
        }
        for (const auto address : host.watch) {
            std::cout << "Watch " << hex32(address) << " = "
                      << hex32(host.ram.read32(address)) << "\n";
        }
        if (host.trace) {
            std::vector<std::pair<std::uint64_t, std::uint32_t>> hot;
            hot.reserve(host.histogram.size());
            for (const auto& entry : host.histogram) {
                hot.emplace_back(entry.second, entry.first);
            }
            std::sort(hot.begin(), hot.end(), std::greater<>());
            std::cout << "Hot PCs (" << host.histogram.size()
                      << " distinct):\n";
            for (std::size_t index = 0; index < hot.size() && index < 14; ++index) {
                std::cout << "  " << hex32(hot[index].second) << "  "
                          << hot[index].first << "\n";
            }
        }
    }
    return host.halted ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path image;
    std::filesystem::path screenshot;
    std::filesystem::path dump_state;
    std::uint32_t watch_change = 0;
    bool cd_log = false;
    bool watch_cd = false;
    bool walk_trace = false;
    bool sync_trace = false;
    bool bios_log = false;
    std::uint64_t frames = 0;  // 0 = interactive
    std::uint64_t max_cycles = 0;
    std::uint64_t pc_from = 0;
    std::uint64_t pc_to = 0;
    bool headless = false;
    bool scanlines = true;
    bool trace = false;
    std::vector<std::uint32_t> watch;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--disc" && index + 1 < argc) {
            image = argv[++index];
        } else if (argument == "--shot" && index + 1 < argc) {
            screenshot = argv[++index];
        } else if (argument == "--frames" && index + 1 < argc) {
            frames = std::stoull(argv[++index]);
            headless = true;
        } else if (argument == "--bios-log") {
            bios_log = true;
        } else if (argument == "--watch-cd") {
            watch_cd = true;
        } else if (argument == "--sync-trace") {
            sync_trace = true;
        } else if (argument == "--walk-trace") {
            walk_trace = true;
        } else if (argument == "--cd-log") {
            cd_log = true;
        } else if (argument == "--watch-change" && index + 1 < argc) {
            watch_change = static_cast<std::uint32_t>(
                std::stoul(argv[++index], nullptr, 16));
        } else if (argument == "--dump-state" && index + 1 < argc) {
            dump_state = argv[++index];
        } else if (argument == "--watch" && index + 1 < argc) {
            watch.push_back(static_cast<std::uint32_t>(
                std::stoul(argv[++index], nullptr, 16)));
        } else if (argument == "--pc-from" && index + 2 < argc) {
            pc_from = std::stoull(argv[++index]);
            pc_to = std::stoull(argv[++index]);
        } else if (argument == "--cycles" && index + 1 < argc) {
            max_cycles = std::stoull(argv[++index]);
            headless = true;
        } else if (argument == "--headless") {
            headless = true;
        } else if (argument == "--no-scanlines") {
            scanlines = false;
        } else if (argument == "--trace") {
            trace = true;
        } else if (image.empty()) {
            image = argument;
        }
    }

    image = find_image(image);
    if (image.empty()) {
        std::cerr << "r6-port: no .chd/.cue/.bin found. Pass --disc <image>\n";
        return 2;
    }

    Host host;
    host.trace = trace;
    host.watch = watch;
    host.bios_log = bios_log;
    host.image = image;
    std::string error;
    if (!load_boot(host, error)) {
        std::cerr << "r6-port: " << error << "\n";
        return 2;
    }
#ifdef R6_HAS_GENERATED_TRANSLATION
    r6_generated::register_functions(host.dispatcher);
#else
    std::cerr << "r6-port: built without a generated translation unit\n";
    return 2;
#endif
    host.bus = std::make_unique<r6::NativeBus>(host.ram, *host.services);
    host.bus->set_vblank_enabled(true);
    host.bus->set_block_boundary_interrupts(false);

    std::cout << "R6 native port\n"
              << "Disc: " << image.filename().string() << "\n"
              << "EXE:  " << host.exe.payload.size() << " bytes at "
              << hex32(host.exe.load_address) << "\n"
              << "Entry " << hex32(host.exe.entry) << ", translated blocks "
              << r6_generated::block_count() << "\n";

    if (headless) {
        return run_headless(host, frames == 0 ? 600 : frames, screenshot,
                           max_cycles, pc_from, pc_to, dump_state,
                           watch_change, cd_log, watch_cd, walk_trace,
                           sync_trace);
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.lpszClassName = L"R6PortWindow";
    RegisterClassExW(&window_class);

    RECT rect{0, 0, kWindowWidth, kWindowHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    const auto window = CreateWindowExW(
        0, L"R6PortWindow", L"R6 - Spider-Man", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
        rect.bottom - rect.top, nullptr, nullptr, GetModuleHandleW(nullptr),
        nullptr);
    if (window == nullptr) {
        std::cerr << "r6-port: CreateWindow failed\n";
        return 3;
    }

    WindowState state;
    state.host = &host;
    state.window = window;
    state.dc = GetDC(window);

    Gl gl;
    if (!create_gl(state, gl) || !load_gl_functions(gl)) return 4;

    std::string vertex_source;
    std::string fragment_source;
    load_shaders(own_directory(), vertex_source, fragment_source);
    state.program = build_program(gl, vertex_source, fragment_source);
    state.source_size = gl.get_uniform_location(state.program, "u_source_size");
    state.scanlines = gl.get_uniform_location(state.program, "u_scanlines");
    state.gamma = gl.get_uniform_location(state.program, "u_gamma");

    gl.gen_vertex_arrays(1, &state.vao);
    gl.bind_vertex_array(state.vao);
    gl.gen_buffers(1, &state.vbo);
    gl.bind_buffer(kGlArrayBuffer, state.vbo);
    static const float kQuad[] = {-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
                                  -1.0f, -1.0f, 1.0f, 1.0f,  -1.0f, 1.0f};
    gl.buffer_data(kGlArrayBuffer, sizeof(kQuad), kQuad, kGlStaticDraw);
    gl.enable_vertex_attrib_array(0);
    gl.vertex_attrib_pointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glGenTextures(1, &state.texture);
    glBindTexture(GL_TEXTURE_2D, state.texture);

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    timeBeginPeriod(1);
    auto last_frame = GetTickCount64();
    auto fps_mark = last_frame;
    std::uint64_t fps_frames = 0;
    MSG message{};
    while (state.running) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                state.running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!state.running) break;

        const auto now = GetTickCount64();
        if (now - last_frame < 16) {
            Sleep(1);
            continue;
        }
        last_frame = now;

        const auto target = host.bus->hardware().cycles() + kFieldCycles;
        while (!host.halted && host.bus->hardware().cycles() < target) {
            if (!step_once(host)) break;
        }
        present(state, gl, scanlines);
        ++fps_frames;
        const auto stamp = GetTickCount64();
        if (stamp - fps_mark >= 500) {
            state.fps = fps_frames * 1000.0 /
                        static_cast<double>(stamp - fps_mark);
            fps_frames = 0;
            fps_mark = stamp;
        }
        if (host.halted) {
            std::cerr << "r6-port: stopped: " << host.stop_reason << "\n";
            SetWindowTextW(window, L"R6 - stopped (see console)");
            break;
        }
        const std::string title = "R6 - Spider-Man - " +
                                  std::to_string(static_cast<int>(state.fps + 0.5)) +
                                  " FPS - " + hex32(host.cpu.pc);
        SetWindowTextA(window, title.c_str());
    }

    timeEndPeriod(1);
    wglMakeCurrent(nullptr, nullptr);
    if (state.gl_context != nullptr) wglDeleteContext(state.gl_context);
    return host.halted ? 1 : 0;
}
