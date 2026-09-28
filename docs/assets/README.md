# README 插图

| 插图 | 展示文件 | 可编辑源文件 |
| --- | --- | --- |
| 首页横幅 | [hero.svg](hero.svg) | [hero.excalidraw](hero.excalidraw) |
| 架构全景 | [architecture.svg](architecture.svg) | [architecture.excalidraw](architecture.excalidraw) |

SVG 为自包含矢量图，内嵌实际使用的字体子集，无需外部字体请求；通过 `prefers-color-scheme` 切换深浅配色，并提供可供读屏使用的标题与说明。README 中使用相对路径，离线也能展示插图。

在 [Excalidraw](https://excalidraw.com) 中打开 `.excalidraw` 即可修改文字、形状和连线。中文编辑时的字体回退、文字度量可能与这里的 SVG 略有不同；导出后请检查排版。Excalidraw 的普通 SVG 导出也不会自动保留本版深浅主题 CSS，可分别导出两种主题并通过 README 的 `<picture>` 引用。

图形笔触由 [rough.js](https://github.com/rough-stuff/rough) 生成，场景采用 Excalidraw 元素格式。原始插图按项目 MIT 许可发布；内嵌字体保留各自许可：

- **Excalifont**：来自 [Excalidraw 字体目录](https://github.com/excalidraw/excalidraw/tree/master/packages/excalidraw/fonts/Excalifont)，[SIL OFL 1.1](licenses/Excalifont-OFL.txt)。
- **Comic Shanns**：来自 [Excalidraw 字体目录](https://github.com/excalidraw/excalidraw/tree/master/packages/excalidraw/fonts/ComicShanns)，[MIT](licenses/ComicShanns-MIT.txt)。
- **小赖字体 Xiaolai**：来自 [lxgw/kose-font v3.126](https://github.com/lxgw/kose-font/releases/tag/v3.126)，[SIL OFL 1.1](licenses/Xiaolai-OFL.txt)。
