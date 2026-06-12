# -*- coding: utf-8 -*-
"""mem_field_geometry_reader - 读 FieldTable 的领域机制几何 (Size=半径米, 游戏开时可读).

实测: FieldTable.Size(i32array) 就是半径(米): 虚蚀龙-X盾领域=[3], 毒圈=[8], 缩圈=[19]。
游戏开着房间里就能读(领域 field 常驻/当前场景加载)。SkillId 多为 0(静态链路断), 按 Name
含 boss 标识匹配机制。这是"游戏开就能拿的部分几何源", 配合运行时时间关联补全瞬时招几何。
"""
from __future__ import annotations

from typing import Dict, List

# Size 单元素 → 圆(半径); 双元素 → 环(内/外) 的启发
_BOSS_TOKENS = ("虚蚀龙", "双子角羊", "炎光", "幻华", "光龙", "冰龙", "石头人",
                "悖与灾", "蚀花", "机骸", "煌之冠", "苍之冠")


class FieldGeometryReader:
    def __init__(self, dps_source):
        self._src = dps_source

    def read_all(self) -> List[Dict]:
        """返回 [{name, shape, radius, inner}] —— FieldTable 全部带 Size 的领域几何。"""
        out = []
        try:
            from mem_probe.il2cpp import table_columns
            from mem_probe.il2cpp.mem_config_table_reader import MemConfigTableReader
            rd = MemConfigTableReader(self._src)
            cls = "Bokura.FieldTableBase"
            C = {k: v[0] for k, v in
                 table_columns.load_columns([cls], log=lambda *a: None)[cls].items()}
            for key, zl, blob in rd.iter_rows_via_loader(cls):
                nm = rd.col_string(zl, blob, C.get("Name"))
                if not nm:
                    continue
                size = list(rd.col_i32_array(zl, blob, C.get("Size")) or [])
                radius = float(size[0]) if size else 0.0
                if radius <= 0:
                    continue
                inner = float(size[1]) if len(size) > 1 and size[1] > 0 else 0.0
                out.append({"name": nm, "shape": "ring" if inner else "circle",
                            "radius": radius, "inner": inner,
                            "boss": next((b for b in _BOSS_TOKENS if b in nm), "")})
        except Exception:
            pass
        return out


__all__ = ["FieldGeometryReader"]
