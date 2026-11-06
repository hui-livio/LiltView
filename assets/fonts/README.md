# LiltView 默认字体

默认采用 **HarmonyOS Sans SC Regular**，从华为官方 HarmonyOS Sans 字体包提取原始 TTF，未修改、裁剪或转换。

- 字体：`HarmonyOS_Sans_SC_Regular.ttf`，8,261,128 字节。
- 官方包内原始许可证：`LICENSE.HarmonyOS-Sans.txt`，保持原始字节，包括文件末尾填充。
- 下载来源、原包与文件 SHA-256：`source.json`。
- 官方资源页：`https://developer.huawei.com/consumer/cn/design/resource/`
- 原包：`https://developer.huawei.com/images/download/general/HarmonyOS-Sans.zip`

字体遵循 **HarmonyOS Sans Fonts License Agreement**，与项目代码的 Apache-2.0 许可证分开。其许可要求保留字体原样、保留版权和协议，并在软件中显著说明使用该字体；主程序“关于”窗口已提供字体署名。本项目随应用分发该字体，不将字体单独作为产品发布。

CMake 将 TTF、原许可证和来源清单复制到可执行文件旁的 `assets/fonts/`。ImGui 从可执行文件相对路径加载，后端动态生成实际使用的字形纹理；不会启动时预生成整个中文字符集。缺失随包字体时尝试系统字体，缺失中文字形时回退英文。

只分发常规字重。字体文件约 7.88 MiB，应单独计入应用安装资源体积。后续更新字体时需重新检查原包来源、许可和校验值。
