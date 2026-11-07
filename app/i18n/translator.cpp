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
                             "Open a JPEG image or drop it here. Ctrl+O to browse.",
                             "Ready",
                             "No image",
                             "This feature is not available yet.",
                             "Chinese font not found. Install a CJK font and restart LiltView.",
                             "A lightweight image viewer for a clear, focused browsing experience.",
                             "Licensed under Apache License 2.0.",
                             "Author",
                             "Project homepage",
                             "Loading...",
                             "Cannot read this file. Check the path and permissions.",
                             "This version supports 8-bit RGB and grayscale JPEG images.",
                             "The image is damaged or incomplete.",
                             "Image exceeds this version's limit (64 MiB file / 256 MiB decoded RGBA).",
                             "Not enough memory to load or display the image.",
                             "The file changed while loading. Open it again.",
                             "Cannot load this image. Try opening another file."};

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
                             "打开或拖入 JPEG 图片。Ctrl+O 选择文件。",
                             "就绪",
                             "暂无图片",
                             "此功能暂未实现。",
                             "未找到中文字体，请安装 CJK 字体并重新启动 LiltView。",
                             "轻量、流畅的图片浏览器，专注每一幅画面。",
                             "采用 Apache License 2.0 许可证。",
                             "作者",
                             "项目主页",
                             "正在加载…",
                             "无法读取文件，请检查路径和访问权限。",
                             "当前版本支持 8 位 RGB 和灰度 JPEG 图片。",
                             "图片数据损坏或不完整。",
                             "图片超过当前版本限制（文件 64 MiB / 解码后 RGBA 256 MiB）。",
                             "加载或显示图片所需内存不足。",
                             "文件在加载时发生变化，请重新打开。",
                             "无法加载图片，请尝试打开其他文件。"};

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
