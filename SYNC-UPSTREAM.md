# 与上游同步指南（fork 维护）

上游：`https://github.com/06066060606060/P4can-universal-unlock.git`（remote 名 `upstream`，已配置）
本 fork 与上游的**全部差异**只有两处：

1. **CAN 引脚映射**（`pin_config.h`）：本机为微雪 ESP32-P4-WIFI6 + 3CAN 扩展板（X179 接线），
   BODY=GPIO4/5、CHASSIS=GPIO2/3、PARTY=GPIO26/27；上游默认 20/21=BODY、22/23=CHASSIS。
2. **仪表盘中文包**（`dashboard_source.html` 内 `<!-- P4CAN I18N START/END -->` 注入块、
   `tools/i18n_source.json`、`tools/build_i18n.py`，以及由其生成的 `index_html.h`、
   相应的 `tests/` 断言）。

## 一键同步（推荐）

```bash
python tools/sync_upstream.py             # fetch -> rebase -> 自动解冲突 -> 强推 origin/main
python tools/sync_upstream.py --no-push  # 只做到本地，不推送（先审结果）
```

脚本自动处理的冲突：

| 文件 | 自动处理方式 |
|---|---|
| `pin_config.h` | 以上游版本为基底，覆盖 6 个 `CAN_*` 宏为 X179 值；上游新增/修改的其他宏全部保留 |
| `dashboard_source.html` | 以上游版本为基底，把 `<!-- P4CAN I18N START/END -->` 整块重新贴到 `</body>` 前（去重，上游已合并该特性时会自动跳过重复注入） |
| `index_html.h` | 冲突标记一律丢弃，重跑 `tools/build_dashboard.py` 重新生成 |
| 其他任何文件 | **故意不猜**，中止并报告，留待人工处理 |

注意 rebase 中 stage 语义反转：`:2:`（ours）是上游基底，`:3:`（theirs）才是自己的提交，脚本已按此实现。
回归测试：`_synctest.py`（工作区根，隔离沙箱模拟"上游改同一行引脚 + 改仪表盘"的硬冲突）。

## 手动同步（上游无冲突改动时）

```bash
git fetch upstream
git rebase upstream/main          # 把本 fork 的提交重放到上游最新之上
git push -f origin main           # rebase 改写了历史，需强推
```

## 冲突解决规则（按文件）

| 冲突文件 | 解决方式 |
|---|---|
| `pin_config.h` | **保留我方（ours）**的 4/5、2/3、26/27 定义，其余取上游 |
| `dashboard_source.html` | **取上游为基底**，再把 `<!-- P4CAN I18N START -->…<!-- P4CAN I18N END -->` 整块重新贴回 `</body>` 前；上游改了界面文案时需在 `tools/i18n_source.json` 补译新词条 |
| `index_html.h` | **生成文件，禁止手工合并**。解决完 `dashboard_source.html` 后重新生成 |
| `tests/*` | 引脚断言从宏派生，取我方；其余取上游 |
| `README.md` 等纯文档 | 直接取上游 |

## 同步后重建固件（仪表盘有变动时才需要）

```bash
python tools/build_i18n.py        # 校验词典覆盖率（有新英文词条会报未覆盖）
python tools/build_dashboard.py   # dashboard_source.html -> index_html.h (gzip)
# 再用 InstaBox VM arduino-cli 编译，参照 firmware/ 下的历史命令
```

## 注意事项

- rebase 前若已发布 tag（如 `v3.29.3-zh.1`），tag 会留在旧提交上，属正常快照，无需移动；
  重建固件后打新 tag 即可。
- `git push -f` 只对**自己的 fork** 使用，永远不要对上游强推。
- 若上游重命名/重构了 `dashboard_source.html` 中的界面结构，中文注入块的位置
  （`</body>` 前）不变，但 MutationObserver 依赖的选择器可能需对照上游 DOM 微调。
