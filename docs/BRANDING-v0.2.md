# LiltView：彩色图标版本

- 版本：v0.2
- 日期：2026-11-06
- 调整依据：增加图标色彩丰富度。
- 状态：用户已选择保留彩色版 ICO，作为当前选定的应用图标。
- 正式项目名：LiltView，已由用户确认。

## 设计变化

保留开口画框、山形与光点的构图，将深青底色替换为蓝紫渐变，山形使用薄荷绿到浅青色，光点使用浅金到珊瑚橙色，画框使用暖白。大尺寸增加淡色山谷填充，丰富层次。

16 / 24 / 32 像素版本去掉山谷填充，并将山形和光点简化为纯色，以保持小尺寸辨识度。单色版本沿用 v0.1。

| 元素 | 色彩 |
| --- | --- |
| 底色 | `#2378D5` → `#5652D2` → `#9845B5` |
| 画框 | `#FFF9F2` |
| 山形 | `#8BF1D2` → `#6ED9FF` |
| 光点 | `#FFE58C` → `#FFAA79` |

## 文件

- 主矢量图：`assets/icons/liltview-color.svg`
- 小尺寸矢量图：`assets/icons/liltview-color-small.svg`
- PNG：`assets/icons/png/liltview-color-{size}.png`，含 16、24、32、48、64、128、256、512 像素。
- Windows 图标：`assets/icons/liltview-color.ico`，含 16、24、32、48、64、128、256 像素。
- 深浅背景与尺寸预览：`assets/icons/preview-color.png`

当前选定的 ICO 是 `assets/icons/liltview-color.ico`。彩色 SVG 源图、PNG 导出及单色工具栏版本一并保留。

旧配色的 SVG、PNG、ICO 和预览已归档至 `assets/icons/archive/v0.1/`，不作为当前应用图标使用。SVG 为可编辑源文件，所有位图均由对应矢量版本导出；正方形图标外部保留透明通道。
