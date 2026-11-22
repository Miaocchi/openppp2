# 文档站

> Status: Active
> Type: Guide
> Last verified: 2026-10-05
> Parent index: [开发文档](README_CN.md) · English: [Documentation site](DOCS_SITE.md)

文档站使用 Zensical 0.0.67，英文入口为 <https://miaocchi.github.io/openppp2/>，中文入口为 <https://miaocchi.github.io/openppp2/zh/>。语言菜单保留当前页面路径。两种语言拥有独立搜索索引；中文内容可以检索，本版本搜索提示仍为英文（[官方搜索说明](https://zensical.org/docs/setup/search/)）。

## 安装与预览

使用与 CI 一致的 Python 3.13，在仓库根目录运行：

```bash
python3 -m venv build/docs-venv
# Windows: build\docs-venv\Scripts\activate
. build/docs-venv/bin/activate
python -m pip install -r requirements-docs.txt
python tools/docs_site.py serve
```

打开 <http://127.0.0.1:8000/> 或 <http://127.0.0.1:8000/zh/>。启动前会构建两种语言。编辑文档后，请停止预览服务并重新运行命令。可用 `--host` 和 `--port` 调整监听地址与端口。

## 构建与验证

```bash
python -m unittest tests.tooling.test_check_docs tests.tooling.test_docs_site -v
python tools/check_docs.py
python tools/docs_site.py build
```

`build` 严格构建两个站点，然后检查生成页面的内部链接及锚点。成品位于 `build/docs-site/site`，中间输入与缓存也保存在 `build/docs-site`。每次构建会清理此工作目录，避免残留已删除页面。可通过 `--site-url https://example.com/project/` 验证其他部署路径。

公开站点只发布首页和[构建工具](../../tools/docs_site.py)中 `SECTIONS` 列出的页面。此清单同时决定发布范围和导航顺序。架构、设计、交接、测试和归档资料保留在 GitHub 仓库，不生成站点页面，也不进入站内搜索。指向仓库专用文件的链接会在 GitHub 打开。

公开页面应保持中英文配对。`README.md` 映射为目录首页，`NAME.md` 与 `NAME_CN.md` 在两个站点共用路径。调整导航时保留现有页面路径。[zensical.toml](../../zensical.toml) 定义主题默认值。

## 发布

[文档工作流](../../.github/workflows/docs.yml) 在文档 PR 中执行治理检查、构建工具测试和双语构建；PR 不会部署。符合路径过滤的 `main` 推送，或在 `main` 上手动运行，会上传官方 Pages artifact 并通过 `github-pages` 环境部署。首次部署前，在 **Settings → Pages → Build and deployment → Source** 选择 **GitHub Actions**。也可设置仓库 secret `PAGES_SETUP_TOKEN`，授予 Pages 写权限（GitHub App 令牌还需要 administration 写权限），工作流将通过 `actions/configure-pages` 自动启用 Pages。默认 `GITHUB_TOKEN` 可部署已启用站点，无法首次启用（[官方 action 参数](https://github.com/actions/configure-pages/blob/v5/action.yml)）。完成启用后可移除不再需要的设置令牌。

合并前请预览配对和未配对页面，在深层页面切换语言，搜索中英文词语，检查 Mermaid 和代码复制，并以移动端宽度检查导航。文档构建无需原生核心、Guardian 或移动应用的构建依赖。
