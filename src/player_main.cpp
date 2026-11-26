// R6 player: a windowed, double-clickable front end for the development
// interpreter.
//
// It finds the user's own BIN/CUE, boots the executable the way the retail
// loader would, and presents the PlayStation display area in a Win32 window.
// It is a bring-up tool, not a playable build: the guest currently stops during
// its own initialisation, so the window will show an empty display.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "r6/boot_image.h"
#include "r6/mips_interpreter.h"
#include "r6/windows_platform_services.h"

namespace {

constexpr std::uint32_t kRamSize = 2u * 1024u * 1024u;
constexpr std::uint32_t kRamMask = kRamSize - 1u;
constexpr std::uint32_t kDefaultStack = 0x801FFFF0u;
constexpr int kScale = 3;
constexpr int kWindowWidth = 320 * kScale;
constexpr int kWindowHeight = 240 * kScale;
constexpr std::uint64_t kStepsPerFrame = 400000;
constexpr std::uint64_t kFrameMicros = 16667;

// This title's C runtime reads its stack limit from .data and computes
// $sp = 0x807FFFF8, which is outside main RAM. The retail loader supplies a
// usable stack, so do the same when the image's value is not in RAM.
constexpr std::uint32_t kCrStackGlobal = 0x800B3E70u;

const wchar_t* const kWindowClass = L"R6PlayerWindow";

struct Player {
    std::filesystem::path image;
    std::filesystem::path bin;
    std::unique_ptr<r6::WindowsPlatformServices> services;
    std::unique_ptr<r6::Memory> ram;
    std::unique_ptr<r6::MipsInterpreter> cpu;
    r6::boot::ExeImage exe;
    std::uint64_t instructions{0};
    bool halted{false};
    std::string stop_text;
    std::uint64_t frames_presented{0};
    double fps{0.0};
    double instructions_per_second{0.0};
    std::uint64_t fps_window_start{0};
    std::uint64_t fps_window_frames{0};
    std::uint64_t fps_window_instructions{0};

    void* dib_pixels{nullptr};
    HBITMAP dib{nullptr};
    HDC dib_dc{nullptr};
};

Player* g_player = nullptr;

bool looks_like_tim(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::uint8_t magic[4]{};
    file.read(reinterpret_cast<char*>(magic), sizeof(magic));
    return file.gcount() == static_cast<std::streamsize>(sizeof(magic)) &&
           magic[0] == 0x10u && magic[1] == 0u && magic[2] == 0u &&
           magic[3] == 0u;
}

bool has_iso9660_pvd(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    // Accept a cooked 2048-byte image and the two common raw-sector layouts.
    for (const std::uint64_t offset : {16ull * 2048ull,
                                        16ull * 2352ull + 24ull,
                                        16ull * 2352ull + 16ull}) {
        char magic[6]{};
        file.clear();
        file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        file.read(magic, sizeof(magic));
        if (file.gcount() == static_cast<std::streamsize>(sizeof(magic)) &&
            magic[0] == '\x01' && std::memcmp(magic + 1, "CD001", 5) == 0) {
            return true;
        }
    }
    return false;
}

bool is_disc_image(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    for (auto& character : extension) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    if (extension == ".chd") {
        r6::WindowsPlatformServices services(path);
        return services.disc_open() &&
               !r6::boot::list_root(services).empty();
    }
    return has_iso9660_pvd(r6::boot::resolve_bin(path));
}

std::filesystem::path scan_for_image(const std::filesystem::path& root,
                                     int max_depth) {
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) return {};

    std::size_t visited = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied,
             error);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (error) break;
        if (++visited > 40000) break;
        if (it.depth() > max_depth) {
            it.disable_recursion_pending();
            continue;
        }
        const auto& entry = *it;
        if (!entry.is_regular_file(error)) continue;
        auto extension = entry.path().extension().string();
        for (auto& character : extension) {
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character - 'A' + 'a');
            }
        }
        if (extension != ".cue" && extension != ".chd" && extension != ".bin") {
            continue;
        }
        if (is_disc_image(entry.path())) return entry.path();
    }
    return {};
}

std::filesystem::path user_profile() {
    wchar_t buffer[MAX_PATH * 2]{};
    const auto length = GetEnvironmentVariableW(
        L"USERPROFILE", buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0) return {};
    return std::filesystem::path(buffer);
}

std::filesystem::path own_directory() {
    wchar_t buffer[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path find_disc() {
    const auto here = own_directory();
    if (auto found = scan_for_image(here, 2); !found.empty()) return found;

    const auto profile = user_profile();
    if (!profile.empty()) {
        for (const char* folder : {"Downloads", "Desktop", "Documents"}) {
            const auto root = profile / folder;
            if (auto found = scan_for_image(root, 3); !found.empty()) {
                return found;
            }
        }
    }
    return {};
}

std::filesystem::path ask_for_disc() {
    wchar_t buffer[MAX_PATH * 4]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"PlayStation images (*.cue;*.bin;*.chd)\0*.cue;*.bin;*.chd\0All files\0*.*\0";
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
    dialog.lpstrTitle = L"Select your PlayStation .cue, .bin or .chd";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) return {};
    return std::filesystem::path(buffer);
}

bool load_image(Player& player, std::string& error) {
    player.bin = r6::boot::resolve_bin(player.image);
    std::error_code code;
    if (!std::filesystem::exists(player.bin, code)) {
        error = "The disc image was not found:\n" + player.bin.string();
        return false;
    }

    player.services = std::make_unique<r6::WindowsPlatformServices>(player.bin);
    if (!player.services->disc_open()) {
        error = "The disc image could not be opened:\n" +
                player.services->disc_error();
        return false;
    }
    player.ram = std::make_unique<r6::Memory>(kRamSize, true);

    const auto files = r6::boot::list_root(*player.services);
    if (files.empty()) {
        error = "No ISO9660 filesystem was found in:\n" + player.bin.string();
        if (looks_like_tim(player.bin)) {
            error += "\n\nThis looks like a PlayStation TIM texture, not a disc "
                     "image. Choose your .cue, .chd or raw .bin disc image.";
        } else {
            error += "\n\nThis is not a PlayStation 1 disc image.";
        }
        if (!player.services->disc_error().empty()) {
            error += "\n" + player.services->disc_error();
        }
        return false;
    }

    std::string exe_name;
    for (const auto& file : files) {
        if (r6::boot::upper(file.name) == "SYSTEM.CNF") {
            exe_name =
                r6::boot::boot_name(r6::boot::read_file(*player.services, file));
        }
    }
    if (exe_name.empty()) {
        // No usable SYSTEM.CNF: fall back to the first PlayStation executable
        // on the disc instead of assuming one particular title's serial.
        for (const auto& file : files) {
            const auto name = r6::boot::upper(file.name);
            for (const char* prefix : {"SLUS", "SCUS", "SLES", "SCES", "SLPS",
                                       "SLPM", "SCPS", "SLED", "SCED"}) {
                if (name.find(prefix) == 0) {
                    exe_name = file.name;
                    break;
                }
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
    if (executable == nullptr) {
        error = "The executable named by SYSTEM.CNF is missing from the disc.";
        return false;
    }

    if (!r6::boot::parse_exe(r6::boot::read_file(*player.services, *executable),
                             player.exe)) {
        error = "The disc executable has no valid PS-X EXE header.";
        return false;
    }

    for (std::size_t index = 0; index < player.exe.payload.size(); ++index) {
        player.ram->write8(
            (player.exe.load_address + static_cast<std::uint32_t>(index)) &
                kRamMask,
            player.exe.payload[index]);
    }

    const auto stack = player.ram->read32(kCrStackGlobal & kRamMask);
    const auto stack_in_ram = stack >= 0x80000000u && stack < 0x80200000u;
    if (!stack_in_ram) {
        player.ram->write32(kCrStackGlobal & kRamMask, kDefaultStack);
    }

    player.cpu =
        std::make_unique<r6::MipsInterpreter>(*player.ram, *player.services);
    auto& cpu = player.cpu->cpu();
    cpu.pc = player.exe.entry;
    cpu.gpr[28] = player.exe.gp;
    cpu.gpr[29] = stack_in_ram ? stack : kDefaultStack;
    cpu.gpr[31] = 0;
    // Vblank ticks are generated for the guest's own handler. The handler now
    // returns through the interpreter's context save, but the CD streaming
    // path it drives is still incomplete, so the display stays off.
    player.cpu->set_vblank_enabled(true);
    return true;
}

void present(Player& player, HDC dc) {
    if (!player.cpu || !player.services || player.dib == nullptr) return;
    const auto frame = player.services->gpu().display_frame();
    auto* pixels = static_cast<std::uint32_t*>(player.dib_pixels);
    constexpr int source_width = 320;
    constexpr int source_height = 240;
    for (int y = 0; y < source_height * kScale; ++y) {
        const auto* row = &frame[static_cast<std::size_t>(y / kScale) *
                                source_width];
        auto* out = pixels + static_cast<std::size_t>(y) * (source_width * kScale);
        for (int x = 0; x < source_width * kScale; ++x) {
            const auto pixel = row[x / kScale];
            // DIB is BGRA in memory.
            out[x] = 0xFF000000u | ((pixel & 0x0000FFu) << 16) |
                     (pixel & 0x00FF00u) | ((pixel >> 16) & 0x0000FFu);
        }
    }
    BitBlt(dc, 0, 0, kWindowWidth, kWindowHeight, player.dib_dc, 0, 0,
           SRCCOPY);

    const auto& gpu = player.services->gpu();

    // Frame-rate readout across the top of the game screen.
    {
        const auto brush = CreateSolidBrush(RGB(0, 0, 0));
        RECT bar{0, 0, kWindowWidth, 20};
        FillRect(dc, &bar, brush);
        DeleteObject(brush);
        char stats[160]{};
        std::snprintf(stats, sizeof(stats),
                      "FPS %.0f   vblank %llu   primitives %llu   %.1fM instr/s",
                      player.fps,
                      static_cast<unsigned long long>(player.cpu->vblanks()),
                      static_cast<unsigned long long>(gpu.rendered_primitives()),
                      player.instructions_per_second / 1000000.0);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(140, 255, 140));
        RECT line{6, 0, kWindowWidth - 6, 20};
        DrawTextA(dc, stats, -1, &line, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }

    if (gpu.display_enabled() && gpu.rendered_primitives() > 0) return;

    // The guest has not drawn anything yet, so say where it is instead of
    // presenting an unexplained black window.
    char text[512]{};
    std::snprintf(text, sizeof(text),
                  "R6 development build - no frame yet\n\n"
                  "Instruction: %llu\n"
                  "GPU primitives: %llu\n"
                  "Disc sectors read: %u\n"
                  "Display: %s\n\n"
                  "The guest boots and runs its own initialisation, then waits "
                  "in its frame loop for a disc interrupt this build does not "
                  "deliver yet.\n%s",
                  static_cast<unsigned long long>(player.instructions),
                  static_cast<unsigned long long>(gpu.rendered_primitives()),
                  player.cpu->cd_sectors_read(),
                  gpu.display_enabled() ? "on" : "off", player.stop_text.empty()
                      ? ""
                      : ("Last stop: " + player.stop_text + "\n").c_str());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(235, 235, 235));
    RECT area{20, 20, kWindowWidth - 20, kWindowHeight - 20};
    DrawTextA(dc, text, -1, &area, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void update_title(Player& player, HWND window) {
    if (!player.cpu) return;
    std::string title = "R6 - ";
    title += player.image.filename().string();
    title += " - ";
    title += std::to_string(player.instructions);
    title += " instructions";
    if (player.services) {
        const auto& gpu = player.services->gpu();
        title += " - ";
        title += std::to_string(gpu.rendered_primitives());
        title += " primitives";
        title += gpu.display_enabled() ? " - display on" : " - display off";
    }
    if (player.halted) {
        title += " - stopped: ";
        title += player.stop_text;
    }
    SetWindowTextA(window, title.c_str());
}

void step(Player& player) {
    if (!player.cpu || player.halted) return;
    const auto result = player.cpu->run(kStepsPerFrame);
    player.instructions += result.steps;
    if (result.reason != r6::StopReason::step_budget) {
        player.halted = true;
        switch (result.reason) {
            case r6::StopReason::returned:
                player.stop_text = "returned to address zero";
                break;
            case r6::StopReason::unsupported_instruction:
                player.stop_text = "unsupported instruction";
                break;
            case r6::StopReason::bios_exit:
                player.stop_text = "BIOS exit";
                break;
            case r6::StopReason::guest_break:
                player.stop_text = "guest BREAK";
                break;
            case r6::StopReason::interrupt_stalled:
                player.stop_text = "interrupt routine did not return";
                break;
            default:
                player.stop_text = "step budget";
                break;
        }
    }
}

void save_screenshot(Player& player) {
    const auto path = own_directory() / "r6-frame.ppm";
    r6::boot::write_ppm(player.services->gpu().display_frame(), 320, 240, path);
    MessageBoxW(nullptr, (L"Saved " + path.wstring()).c_str(), L"R6 Player",
                MB_OK | MB_ICONINFORMATION);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(window, &paint);
            if (g_player) present(*g_player, dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                DestroyWindow(window);
                return 0;
            }
            if (wparam == VK_F12 && g_player) {
                save_screenshot(*g_player);
                return 0;
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

HBITMAP create_frame_bitmap(void** pixels) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = kWindowWidth;
    info.bmiHeader.biHeight = -kWindowHeight;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, pixels, nullptr, 0);
}

bool parse_arguments(int argc, char** argv, std::filesystem::path& image,
                     std::filesystem::path& shot, std::uint64_t& shot_steps) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--shot" && index + 1 < argc) {
            shot = argv[++index];
        } else if (argument == "--steps" && index + 1 < argc) {
            shot_steps = std::stoull(argv[++index]);
        } else if (image.empty()) {
            image = argument;
        }
    }
    return true;
}

int run_headless(Player& player, const std::filesystem::path& shot,
                 std::uint64_t steps) {
    auto remaining = steps;
    while (remaining > 0 && !player.halted) {
        const auto chunk = remaining < 2000000 ? remaining : 2000000;
        const auto result = player.cpu->run(chunk);
        player.instructions += result.steps;
        remaining -= result.steps;
        if (result.reason != r6::StopReason::step_budget) {
            player.halted = true;
            break;
        }
        if (result.steps == 0) break;
    }
    r6::boot::write_ppm(player.services->gpu().display_frame(), 320, 240, shot);
    return 0;
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR command_line, int) {
    int argc = 0;
    auto* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> storage;
    storage.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        const std::wstring wide = argv[index];
        storage.emplace_back(wide.begin(), wide.end());
    }
    if (argv != nullptr) LocalFree(argv);
    std::vector<char*> arguments;
    arguments.reserve(storage.size());
    for (auto& text : storage) arguments.push_back(text.data());

    std::filesystem::path image;
    std::filesystem::path shot;
    std::uint64_t shot_steps = 40000000;
    parse_arguments(argc, arguments.data(), image, shot, shot_steps);
    (void)command_line;

    Player player;
    if (image.empty()) image = find_disc();
    if (image.empty()) image = ask_for_disc();
    if (image.empty()) {
        MessageBoxW(nullptr,
                    L"No disc image was selected.\n\nRun this again and pick "
                    L"your .cue, .bin or .chd file.",
                    L"R6 Player", MB_OK | MB_ICONWARNING);
        return 0;
    }

    player.image = image;
    std::string error;
    if (!load_image(player, error)) {
        if (!shot.empty()) {
            std::fprintf(stderr, "r6-player: %s\n", error.c_str());
            return 1;
        }
        MessageBoxA(nullptr, error.c_str(), "R6 Player", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!shot.empty()) {
        // Headless: run a fixed instruction budget and save one frame.
        return run_headless(player, shot, shot_steps);
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0) return 1;

    RECT rect{0, 0, kWindowWidth, kWindowHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    const std::string start_title =
        "R6 Player - " + player.image.filename().string();
    const std::wstring window_title(start_title.begin(), start_title.end());
    const auto window = CreateWindowExW(
        0, kWindowClass, window_title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
        CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr,
        nullptr, instance, nullptr);
    if (window == nullptr) return 1;

    player.dib = create_frame_bitmap(&player.dib_pixels);
    if (player.dib == nullptr) return 1;
    const auto screen = GetDC(nullptr);
    player.dib_dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    SelectObject(player.dib_dc, player.dib);

    g_player = &player;
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    update_title(player, window);

    auto last_frame = GetTickCount64();
    auto last_title = last_frame;
    MSG message{};
    while (message.message != WM_QUIT) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (message.message == WM_QUIT) break;

        const auto now = GetTickCount64();
        if (now - last_frame >= 16) {
            step(player);
            ++player.frames_presented;
            last_frame = now;
            const auto dc = GetDC(window);
            present(player, dc);
            ReleaseDC(window, dc);
        }
        if (now - player.fps_window_start >= 1000) {
            const auto elapsed = now - player.fps_window_start;
            player.fps = static_cast<double>(player.frames_presented -
                                             player.fps_window_frames) *
                         1000.0 / static_cast<double>(elapsed);
            player.instructions_per_second =
                static_cast<double>(player.instructions -
                                    player.fps_window_instructions) *
                1000.0 / static_cast<double>(elapsed);
            player.fps_window_start = now;
            player.fps_window_frames = player.frames_presented;
            player.fps_window_instructions = player.instructions;
        }
        if (now - last_title >= 500) {
            update_title(player, window);
            last_title = now;
        }
        Sleep(1);
    }

    g_player = nullptr;
    return 0;
}
