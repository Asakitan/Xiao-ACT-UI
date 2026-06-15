# -*- coding: utf-8 -*-
"""utils — 窗口定位/窗口效果/音效/性能探针/安装辅助 等通用工具模块。

功能子包：保持空 __init__（不 re-export），各模块用完整点路径引用，
例如 `from utils.perf_probe import probe`，以避免汇聚式 __init__ 引入循环依赖。
（2026-06-02 目录重组：从根目录下沉至此。）
"""
