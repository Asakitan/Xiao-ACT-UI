# -*- coding: utf-8 -*-
# render — GPU 叠加窗口/合成器/渲染器/帧调度等通用渲染管线模块。
#
# 功能子包：保持空 __init__（不 re-export），各模块用完整点路径引用，
# 例如 `from render import gpu_renderer`，以避免汇聚式 __init__ 引入循环依赖。
# （2026-06-02 目录重组：从根目录下沉至此。）
