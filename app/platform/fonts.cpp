#include "fonts.hpp"

#include <SDL3/SDL.h>
#include <filesystem>

#if defined(__linux__)
    #include <fontconfig/fontconfig.h>
#elif defined(_WIN32)
    #include <windows.h>
#endif

namespace liltview
{
namespace
{
SystemFont bundled_font()
{
    SystemFont result;
    // Executable-relative resources: independent of the process working directory.
    if (const char *base = SDL_GetBasePath(); base != nullptr)
    {
        const std::string_view base_utf8(base);
        const auto directory =
            std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(base_utf8.data()), base_utf8.size()));
        const auto font = directory / "assets/fonts/HarmonyOS_Sans_SC_Regular.ttf";
        std::error_code error;
        if (std::filesystem::is_regular_file(font, error))
        {
            const auto utf8 = font.u8string();
            result.path.assign(reinterpret_cast<const char *>(utf8.data()), utf8.size());
            result.chinese = true;
            return result;
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
} // namespace liltview
