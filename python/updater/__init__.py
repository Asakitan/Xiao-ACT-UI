# -*- coding: utf-8 -*-
# updater — 远程更新客户端（拉取 manifest / 版本比较 / 下载校验 / 触发 update_apply）。
#
# 功能子包：保持空 __init__（不 re-export），用完整点路径 `from updater import sao_updater`。
# 注意：外部更新应用器 update_apply.py 仍留在根目录（它是 update.spec 的打包入口）。
# （2026-06-02 目录重组：从根目录下沉至此。）
