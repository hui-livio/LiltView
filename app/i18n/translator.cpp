#include "translator.hpp"

#include <array>

namespace liltview
{
namespace
{
using Dictionary = std::array<const char *, static_cast<std::size_t>(Text::count)>;

constexpr Dictionary english{"File",
                             "Open image...",
                             "Quit",
                             "View",
                             "Fullscreen",
                             "Leave fullscreen",
                             "Settings",
                             "Language",
                             "Follow system",
                             "English",
                             "简体中文",
                             "Appearance",
                             "Dark",
                             "Light",
                             "Help",
                             "About LiltView",
                             "Close",
                             "Previous",
                             "Next",
                             "Fit",
                             "100%",
                             "A lighter way to view.",
                             "No image yet",
                             "Image opening will be available in a future version.",
                             "Ready",
                             "No image",
                             "Available when image opening is implemented.",
                             "Chinese font not found. Install a CJK font and restart LiltView.",
                             "A lightweight image viewer for a clear, focused browsing experience.",
                             "Licensed under Apache License 2.0.",
                             "Author",
                             "Project homepage"};

constexpr Dictionary chinese{"文件",
                             "打开图片…",
                             "退出",
                             "视图",
                             "全屏",
                             "退出全屏",
                             "设置",
                             "语言",
                             "跟随系统",
                             "English",
                             "简体中文",
                             "外观",
                             "深色",
                             "浅色",
                             "帮助",
                             "关于 LiltView",
                             "关闭",
                             "上一张",
                             "下一张",
                             "适应窗口",
                             "100%",
                             "轻快看图，专注画面。",
                             "还没有图片",
                             "图片打开功能将在后续版本提供。",
                             "就绪",
                             "暂无图片",
                             "图片打开功能实现后可用。",
                             "未找到中文字体，请安装 CJK 字体并重新启动 LiltView。",
                             "轻量、流畅的图片浏览器，专注每一幅画面。",
                             "采用 Apache License 2.0 许可证。",
                             "作者",
                             "项目主页"};

constexpr bool complete(const Dictionary &dictionary)
{
    for (const auto *text : dictionary)
    {
        if (text == nullptr || text[0] == '\0')
            return false;
    }
    return true;
}

static_assert(complete(english) && complete(chinese));
} // namespace

Language resolve_language(Language preference, std::string_view locale, bool chinese_font)
{
    if (!chinese_font)
        return Language::english;
    if (preference != Language::system)
        return preference;
    return locale.starts_with("zh") ? Language::chinese : Language::english;
}

const char *translate(Language language, Text text)
{
    const auto index = static_cast<std::size_t>(text);
    if (index >= english.size())
        return "";
    return language == Language::chinese ? chinese[index] : english[index];
}

std::string_view language_code(Language language)
{
    switch (language)
    {
    case Language::english:
        return "en-US";
    case Language::chinese:
        return "zh-CN";
    default:
        return "system";
    }
}

Language parse_language(std::string_view code)
{
    if (code == "en-US")
        return Language::english;
    if (code == "zh-CN")
        return Language::chinese;
    return Language::system;
}
} // namespace liltview
