# -*- coding: utf-8 -*-
# net — Npcap 抓包 / 协议数据桥接 等网络数据管道模块。
#
# 功能子包：保持空 __init__（不 re-export），各模块用完整点路径引用，
# 例如 `from net import packet_bridge`，以避免汇聚式 __init__ 引入循环依赖。
# （2026-06-02 目录重组：从根目录下沉至此。packet_parser 拆成独立包但保留顶层名。）
