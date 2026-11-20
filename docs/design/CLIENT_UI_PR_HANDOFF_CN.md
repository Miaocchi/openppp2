# 客户端 UI PR 交接

## 当前状态

- [PR #153](https://github.com/Miaocchi/openppp2/pull/153)：`feat/client-ui-redesign` → `main`，GitHub 显示 `CONFLICTING`。PR 头为 `d2dea6de`；此前核验为 20 个提交、158 个文件，包含尚未合入的 Multipath/runtime 前置提交，并非纯 UI PR。不要按纯 UI 变更合并。
- [PR #152](https://github.com/Miaocchi/openppp2/pull/152)：`multipath-kcc-default` → `main`，也显示 `CONFLICTING`；本次未展开它的冲突，需独立处理。
- #153 的隔离 worktree 为 `/tmp/openppp2-pr153`。已从 `d2dea6de` 启动对 `8a774f3e` 的合并，`MERGE_HEAD` 尚在；未解决、未提交、未推送。原工作区有未提交变更，不要在原工作区接着合并、暂存或清理。若隔离目录已不存在，先 fetch，再从 PR 远端分支建一个新的隔离 worktree。

## #153 续办

1. 先 fetch 并核对远端 PR 头、`main` 是否仍对应上述 SHA；若已变化，在隔离 worktree 按新基线重新评估。用 `git diff --name-only --diff-filter=U` 确认未解决文件。
2. `desktop/client/src/routes/Settings.svelte`：保留 PR 的独立「代理模式访问范围」卡片、自动保存与下次连接生效提示，以及 P2P 卡片的网络覆盖保存提示；同时保留 `main` 的代理模式可见性判断、`normalizeProxyForwards` 校验及错误提示。关闭虚拟子网时仅禁用端口转发，不应禁用互联网代理开关。不要整块选用某一侧。
3. `docs/testing/DESKTOP_GUI.md`：合并两侧的 smoke 覆盖与离线 Chromium 说明。保留三个宽度、日志等覆盖，以及从仅子网切换到仅互联网的回归场景。`desktop/client/test/gui-smoke.mjs` 已自动合并但未运行；核对验收场景反映的实际行为。
4. 在隔离 worktree 跑桌面 `npm test`、`npm run build` 及 Chromium GUI smoke，重点检查代理模式条件显示、互联网开关独立性与端口校验。检查合并后的 diff 和冲突标记，再完成 merge commit；只普通推送 `feat/client-ui-redesign`，不强推。回查 GitHub `mergeable` 和 CI 状态。

## 验证边界

合并前的 `d2dea6de`：Android `flutter analyze lib` 无问题，112 项 Flutter 测试通过；Windows 前端 44 项测试、生产构建与 Chromium 离线 smoke 通过；iOS 只完成 Swift 语法解析，Linux 未运行 `swift test`。这些结果不覆盖未完成的合并，也不证明 Windows 原生行为或真实 VPN 连接。
