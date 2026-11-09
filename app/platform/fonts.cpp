#include "fonts.hpp"

#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <limits>
#include <span>
#include <vector>

#if defined(__linux__)
    #include <fontconfig/fontconfig.h>
#elif defined(_WIN32)
    #include <windows.h>
#endif

namespace liltview
{
namespace
{
constexpr auto crc32_table()
{
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < table.size(); ++index)
    {
        auto value = index;
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1U) != 0 ? 0xedb88320U : 0U);
        table[index] = value;
    }
    return table;
}

std::uint32_t font_crc32(std::span<const unsigned char> bytes)
{
    static constexpr auto table = crc32_table();
    const auto *lookup          = table.data();
    std::uint32_t checksum      = 0xffffffffU;
    // A table lookup per byte keeps validating the 8 MiB resource inexpensive,
    // including unoptimized development builds.
    for (const auto *byte = bytes.data(), *end = byte + bytes.size(); byte != end; ++byte)
        checksum = lookup[(checksum ^ *byte) & 0xffU] ^ (checksum >> 8);
    return checksum ^ 0xffffffffU;
}

SystemFont bundled_font()
{
    SystemFont result;
    // Executable-relative resources: independent of the process working directory.
    if (const char *base = SDL_GetBasePath(); base != nullptr)
    {
        const std::string_view base_utf8(base);
        const auto directory =
            std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(base_utf8.data()), base_utf8.size()));
        const std::array directories{directory / "assets/fonts", directory / LILTVIEW_INSTALLED_FONT_DIR};
        for (const auto &candidate : directories)
        {
            const auto font = candidate / "HarmonyOS_Sans_SC_Regular.ttf";
            std::error_code error;
            if (std::filesystem::is_regular_file(font, error))
            {
                const auto utf8 = font.u8string();
                result.path.assign(reinterpret_cast<const char *>(utf8.data()), utf8.size());
                result.chinese = true;
                result.bundled = true;
                return result;
            }
        }
    }
    return result;
}

#if defined(__linux__)
SystemFont fontconfig_result(FcPattern *font)
{
    SystemFont result;
    FcChar8 *file      = nullptr;
    FcCharSet *charset = nullptr;
    if (FcPatternGetString(font, FC_FILE, 0, &file) != FcResultMatch)
        return result;
    result.path = reinterpret_cast<const char *>(file);
    FcPatternGetInteger(font, FC_INDEX, 0, &result.face_index);
    if (FcPatternGetCharSet(font, FC_CHARSET, 0, &charset) == FcResultMatch)
        result.chinese =
            FcCharSetHasChar(charset, 0x4E2D) && FcCharSetHasChar(charset, 0x8F7B) && FcCharSetHasChar(charset, 0x89C8);
    return result;
}

SystemFont system_font()
{
    SystemFont result;
    auto *config = FcInitLoadConfigAndFonts();
    if (config == nullptr)
        return result;
    auto *pattern = FcPatternCreate();
    if (pattern != nullptr)
    {
        FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8 *>("sans-serif"));
        FcPatternAddString(pattern, FC_LANG, reinterpret_cast<const FcChar8 *>("zh-cn"));
        FcConfigSubstitute(config, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult match_result;
        auto *font = FcFontMatch(config, pattern, &match_result);
        if (font != nullptr)
        {
            result = fontconfig_result(font);
            FcPatternDestroy(font);
        }
        FcPatternDestroy(pattern);
    }
    FcConfigDestroy(config);
    return result;
}
#elif defined(_WIN32)
SystemFont system_font()
{
    SystemFont result;
    wchar_t directory[MAX_PATH];
    const auto length = GetWindowsDirectoryW(directory, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
    {
        const auto fonts = std::filesystem::path(directory) / "Fonts";
        for (const auto *filename : {L"msyh.ttc", L"simhei.ttf", L"segoeui.ttf"})
        {
            const auto path = fonts / filename;
            std::error_code error;
            if (std::filesystem::is_regular_file(path, error))
            {
                const auto utf8 = path.u8string();
                result.path.assign(reinterpret_cast<const char *>(utf8.data()), utf8.size());
                result.chinese = std::wstring_view(filename) != L"segoeui.ttf";
                break;
            }
        }
    }
    return result;
}
#else
SystemFont system_font()
{
    return {};
}
#endif
} // namespace

SystemFont find_application_font()
{
    // 查找优先级与平台枚举分开；可直接看出“随包字体 → 系统字体”的回退顺序。
    const auto bundled = bundled_font();
    return bundled.path.empty() ? system_font() : bundled;
}

SystemFont find_system_font()
{
    return system_font();
}

bool load_font_candidate(const SystemFont &font)
{
    using FaceIndex = decltype(ImFontConfig::FontNo);
    if (font.face_index < 0 || font.face_index > std::numeric_limits<FaceIndex>::max())
        return false;
    if (font.path.empty())
        return false;
    const auto path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(font.path.data()), font.path.size()));
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size <= 100 || size > 64 * 1024 * 1024)
        return false;
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)))
        return false;
    // CRC detects a corrupted copy of our immutable bundled font before stb
    // parses it. It is an integrity check, not a signature or trust mechanism.
    if (font.bundled && (size != 8261128 || font_crc32(bytes) != 0x88e4e870U))
        return false;
    auto &io = ImGui::GetIO();
    struct AssertionScope
    {
        ImGuiIO &io;
        bool previous = io.ConfigErrorRecoveryEnableAssert;
        ~AssertionScope()
        {
            io.ConfigErrorRecoveryEnableAssert = previous;
        }
    } scope{io};
    io.ConfigErrorRecoveryEnableAssert = false;
    ImFontConfig config;
    config.FontNo                = static_cast<FaceIndex>(font.face_index);
    config.FontDataOwnedByAtlas  = false; // ImGui makes its own owned copy.
    config.Flags                |= ImFontFlags_NoLoadError;
    return io.Fonts->AddFontFromMemoryTTF(bytes.data(), static_cast<int>(size), 17.0f, &config) != nullptr;
}
} // namespace liltview
