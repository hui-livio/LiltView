# 图片样本

本目录图片是项目原创的 96×64 纯红合成样本，随项目采用 Apache-2.0。
PNG、SVG、无损 WebP 的 alpha 为约 50%，BMP 为不透明 24 位。GIF 含红 / 蓝两帧，分别保存播放一次与无限循环版本，帧时长 100 ms；TIFF 为不透明 RGB 首页。

`generate_formats.py` 可重新生成它们；仅生成 WebP 时需要本机 libwebp 编码器。
正常编译与测试直接读已保存文件，不运行生成器，也不依赖系统编码库。
更多位深、Adam7、BMP 位域 / RLE、异常输入由 `tests/core/format_tests.cpp` 在测试内生成。
