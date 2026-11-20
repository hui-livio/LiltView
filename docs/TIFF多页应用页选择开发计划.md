# TIFF 多页应用页选择开发计划

HEIF 依赖方案保持暂停。本计划处理当前格式顺序中 TIFF 多页已经完成 Core 支持、但应用页选择 UI 尚未接入的缺口。

## 当前边界

- Core ABI 3 已支持 `LV_SELECTION_TIFF_PAGE`，使用零基页索引提交请求。
- TIFF INFO 已返回当前页选择和页总数，页面选择已经进入请求身份和内容缓存身份。
- 应用首版只处理静态 TIFF 页面切换，不把页面当作动画，不改变目录导航和普通图片切换语义。
- UI 对用户显示一基页码，例如 `Page 2 / 5`。无效页码、损坏页面或源版本变化必须保留当前已显示页面，并显示对应错误。
- 单页 TIFF 可以显示 `Page 1 / 1`，不显示会改变布局的多余控件。非 TIFF 图片不显示页选择控件。

## 分批计划

| 批次 | 范围 | 独立提交 | 验收 |
| --- | --- | --- | --- |
| T0 | 冻结页码、状态、交互、失败回退和缓存身份契约 | 本提交 | 计划文档、源码证据和未实现边界审计 |
| T1 | Viewer 页面状态和请求 plumbing | `tiff-pages: connect viewer page selection` | 首次打开、页切换、取消、源版本、旧页面保留 |
| T2 | 信息面板页面控件 | `tiff-pages: add page selection UI` | Page n / total、上一页 / 下一页、单页隐藏控件、键盘和可访问性行为 |
| T3 | Core / app targeted tests 与 TIFF fixtures | `tiff-pages: add page selection acceptance tests` | 首页、末页、中间页、越界、损坏、缓存隔离、快速切换、普通单页回归 |
| T4 | Review、全量 CTest、应用 smoke 和回归审查 | `tiff-pages: review page selection` | 代码 review 修正、CTest、CLI / app tests、无旧图错误替换 |
| T5 | 兼容表、Core 契约、帮助文本和发行审查 | `docs: record TIFF page selection` | 文档与实际能力一致，HEIF 仍保持暂停 |

## 交互与状态契约

- Viewer 保存已显示页、请求页、页总数和页切换 generation。新页请求继承当前路径，但使用新的 request ID 和 selection index。
- 页切换只允许在已知页数范围内提交。页总数未知时不猜测，也不生成下一页请求。
- 页面切换失败时保留旧 scene、旧页码和旧信息。错误恢复后可重新请求目标页。
- 页面切换期间状态栏和信息面板显示当前已显示页或目标页正在加载，不能把目标页 INFO 当成已显示页。
- Core 已有 selection cache identity，T1/T3 必须验证同一文件不同页不会重放错误页面。
- 目录上一张 / 下一张继续表示文件导航。TIFF 页上一张 / 下一页只在信息面板的页面控件中操作，避免混淆两种导航。

## 明确不纳入本批次

TIFF 页面编辑、页面重排、页面导出、动画式自动播放、跨文件页连续播放、浮点 / CMYK 新子集、REGION 扩展和 HEIF 依赖方案不纳入本计划。
