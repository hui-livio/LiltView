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
                             "Pan left",
                             "Pan right",
                             "Pan up",
                             "Pan down",
                             "Zoom in",
                             "Zoom out",
                             "A lighter way to view.",
                             "No image yet",
                             "Open an image or drop it here. Ctrl+O to browse.",
                             "Ready",
                             "No image",
                             "This feature is not available yet.",
                             "Chinese font not found. Install a CJK font and restart LiltView.",
                             "A lightweight image viewer for a clear, focused browsing experience.",
                             "Licensed under Apache License 2.0.",
                             "Author",
                             "Project homepage",
                             "Loading...",
                             "Restoring cached textures...",
                             "Loading original pixel rows...",
                             "Cannot read this file. Check the path and permissions.",
                             "Supported: JPEG, PNG, basic SVG, static WebP, BMP, animated GIF and TIFF first page.",
                             "The image is damaged or incomplete.",
                             "Image exceeds the limits supported by this decoder or output region.",
                             "Not enough memory to load or display the image.",
                             "The file changed while loading. Open it again.",
                             "Cannot load this image. Try opening another file.",
                             "Enable system titlebar",
                             "Disable rounded corners",
                             "Minimize",
                             "Maximize",
                             "Restore",
                             "Scanning folder...",
                             "Cannot list this folder.",
                             "Image information",
                             "Full path",
                             "File size",
                             "Format",
                             "Source dimensions",
                             "Oriented dimensions",
                             "Decoded canvas",
                             "Displayed zoom",
                             "Orientation (1–8)",
                             "Pixel output",
                             "Output color",
                             "Animation frame",
                             "Source frame duration",
                             "Source duration per pass",
                             "Total plays",
                             "Unknown",
                             "Infinite",
                             "bytes",
                             "straight alpha",
                             "Unknown (no ICC/HDR conversion)",
                             "Display state",
                             "Original image is still loading",
                             "Showing the previous image while loading"};

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
                             "向左平移",
                             "向右平移",
                             "向上平移",
                             "向下平移",
                             "放大",
                             "缩小",
                             "轻快看图，专注画面。",
                             "还没有图片",
                             "打开或拖入图片。Ctrl+O 选择文件。",
                             "就绪",
                             "暂无图片",
                             "此功能暂未实现。",
                             "未找到中文字体，请安装 CJK 字体并重新启动 LiltView。",
                             "轻量、流畅的图片浏览器，专注每一幅画面。",
                             "采用 Apache License 2.0 许可证。",
                             "作者",
                             "项目主页",
                             "正在加载…",
                             "正在恢复缓存纹理…",
                             "正在逐行加载原始图像…",
                             "无法读取文件，请检查路径和访问权限。",
                             "支持 JPEG、PNG、基础 SVG、静态 WebP、BMP、GIF 动画和 TIFF 首页。",
                             "图片数据损坏或不完整。",
                             "图片超过当前解码器或输出区域支持的限制。",
                             "加载或显示图片所需内存不足。",
                             "文件在加载时发生变化，请重新打开。",
                             "无法加载图片，请尝试打开其他文件。",
                             "启用系统标题栏",
                             "关闭圆角",
                             "最小化",
                             "最大化",
                             "恢复",
                             "正在扫描目录…",
                             "无法列出此目录。",
                             "图片信息",
                             "完整路径",
                             "文件大小",
                             "格式",
                             "原始尺寸",
                             "方向变换后尺寸",
                             "解码画布",
                             "当前显示比例",
                             "方向（1–8）",
                             "输出像素",
                             "输出颜色",
                             "动画帧",
                             "源帧时长",
                             "源单轮时长",
                             "总播放次数",
                             "未知",
                             "无限",
                             "字节",
                             "非预乘透明度",
                             "未知（未做 ICC/HDR 转换）",
                             "显示状态",
                             "原始图像仍在加载",
                             "加载新图期间显示上一张图片"};

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
