# Documentation site

> Status: Active
> Type: Guide
> Last verified: 2026-10-05
> Parent index: [Development](README.md) · Chinese: [文档站](DOCS_SITE_CN.md)

The site uses Zensical 0.0.67 with English at <https://miaocchi.github.io/openppp2/> and Chinese at <https://miaocchi.github.io/openppp2/zh/>. The language menu keeps the current page path. Search indexes are independent; Chinese content is searchable, but search prompts remain English in this release ([Zensical search documentation](https://zensical.org/docs/setup/search/)).

## Install and preview

Use Python 3.13, matching CI. From the repository root:

```bash
python3 -m venv build/docs-venv
# Windows: build\docs-venv\Scripts\activate
. build/docs-venv/bin/activate
python -m pip install -r requirements-docs.txt
python tools/docs_site.py serve
```

Open <http://127.0.0.1:8000/> or <http://127.0.0.1:8000/zh/>. Both languages are built before serving. After editing a document, stop the server and re-run the command to regenerate both sites. Optional `--host` and `--port` arguments change the listener.

## Build and verify

```bash
python -m unittest tests.tooling.test_check_docs tests.tooling.test_docs_site -v
python tools/check_docs.py
python tools/docs_site.py build
```

`build` runs both Zensical builds in strict mode, then verifies generated internal links and fragments. Output is `build/docs-site/site`; generated inputs and caches also stay under `build/docs-site`. Each run cleans this workspace so removed pages cannot survive. Use `--site-url https://example.com/project/` to test another deployment base path.

The public site includes only the pages listed in `SECTIONS` in [the builder](../../tools/docs_site.py), plus the homepage. The list defines both what is published and the order of the navigation. Architecture, design notes, handoffs, tests, and archives stay in GitHub; they are not generated or indexed by the public site. Links to existing repository-only files open on GitHub.

Keep an English/Chinese pair for each public page. `README.md` becomes a directory homepage; `NAME.md` and `NAME_CN.md` share a path across the two sites. Keep existing page paths when changing the navigation. [zensical.toml](../../zensical.toml) controls theme defaults.

## Publish

[Documentation workflow](../../.github/workflows/docs.yml) checks governance, tests the builder, and builds both languages on documentation PRs. PRs cannot deploy. A matching push to `main`, or a manual run on `main`, uploads the official Pages artifact and deploys through the `github-pages` environment. Before the first deployment, select **Settings → Pages → Build and deployment → Source → GitHub Actions**. Alternatively, provide a repository secret `PAGES_SETUP_TOKEN` with Pages write permission (and administration write permission for a GitHub App token); the workflow then enables Pages through `actions/configure-pages`. The default `GITHUB_TOKEN` can deploy an enabled site but cannot perform first-time enablement ([official action inputs](https://github.com/actions/configure-pages/blob/v5/action.yml)). Remove the setup secret after enablement if no longer needed.

Before merging, preview a paired and an unpaired page, use the language menu from a nested page, search English and Chinese terms, check a Mermaid diagram and code copy, and inspect navigation at a narrow mobile width. Native, Guardian, and mobile build dependencies are unnecessary for the documentation build.
