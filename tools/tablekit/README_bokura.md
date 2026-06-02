# Bokura 配置表逆向报告 (星痕共鸣 / Star Resonance CN, m0.pkg)

目标: 从 `m0.pkg` 离线抽出完整 `id -> 中文名` 表 (skill / monster / dungeon)。

**结论 (诚实): 离线不可完整解码。** m0.pkg 里有按 id 升序拼接的中文名字符串池,
但缺少把"第 k 个名字"对回"确切 id"所需的结构 (id 列 / 偏移指针), 且很多表的名字
根本不在文件里。完整、准确的表只能从**运行时加载的 C# ZTable (IL2CPP)** 读取。

下面是验证过的事实、方法、和为什么卡住。

---

## 1. 容器结构 (已验证)

- `m0.pkg` = 1,217,263,843 B。magic `76 20 AF E1`。
- 内含 **4713 个 Lua 5.3 字节码 chunk** (`1B 4C 75 61 53` = `\x1bLuaS`,
  format=1, int=4, size_t=4, instruction=4, lua_Integer=8, lua_Number=8)。
- 关键: 用 **完整解析每个 chunk 的 Proto** (code/constants/upvalues/protos/debug)
  得到 chunk 真实结束位置, 发现 chunk 本体只占几十 MB; **chunk 之间的"数据缝隙"
  共 ~1.18 GB**, 这些缝隙才是 Bokura 表二进制数据区。
  (注意: 不能用"下一个 LuaS 签名"当 chunk 结束 —— 那会把缝隙误算进 chunk。)
  解析器见 `bokura_decode.build_gap_map()` (4713/4713 chunk 全部解析成功)。

## 2. 中文名字符串池 (已验证)

- 名字以 **`<1 字节长度><UTF-8>`** 紧密拼接 (长名也见过 `<u16 长度>` 变体)。
- 主池在 **缝隙 `gap@0x124DE0AA .. 0x12D41D0D` (~8.6 MB)**, token 化得到
  **102,416 个 token (91,537 唯一)** —— 技能描述、怪物名、UI 文本、对话**全混在一起**。
- **池里名字按 id 升序排列** (实证: 怪物名锚点 97.8% 单调递增; 取一段连续 run,
  其名字顺序精确对应 id `10000,10001,10002,10003,10004,(跳)10007,10008...`)。
- 名字**去重共享** (id 116-120 都叫"木桩"→同一偏移; 110/111"棒槌哥布林"→同偏移)。

## 3. 为什么 id 对不上 (卡点, 已验证)

按 `ReadProxy.ReadString(offset)` 的设想, 行表里每行应有"Name 字段在池中的偏移"。
但实测三条全否:

1. **无绝对偏移指针**: 833 个怪物名的绝对文件偏移, 作为 u32 在**全文件 0 次**出现。
2. **无简单相对偏移**: 试遍各种池基址, 无法让一批名字偏移落进同一密集指针数组。
3. **无与名字行序对应的 id 列**: 找到的连续 u32 id 数组 (如 `10000,10001,...` ) 是
   **无跳号的全枚举**, 与真实表 (有跳号) 对不上; 不是该 name run 的 id 列。
   另外存在 `<u16 len><id数字+名字>` 拼接的小 K-V 表 (如 `"1013利奥雷乌斯"`), 但只有
   零散几百行, 且数字前缀语义不一 (有的是 ModelID, 有的是装备等级), 不能当统一 id。

并且 **池只装了部分名字**:
- 怪物名: crib 887 个唯一名中 835 个在池里 (94%), 但因重名+缺名+无 id 列,
  稳对齐只能锚定 ~350 个。
- 技能名: crib 8489 个唯一名中**只有 1733 个 (20%)** 在池里; 其余 80% 不在 m0.pkg。

→ 即使知道"池按 id 升序", 也无法离线唯一确定每个 token 的 id (跳号 id 列表未知,
  crib 本身不全/不准 README 已注明 "mapping is not accurate"), 强行插值=瞎猜。

## 4. 锚点对齐能做到的 (可复用, 但只是子集)

`bokura_decode.align_table(tokens, crib)`: 用已知正确的 crib 当锚点,
LIS 剔除非单调离群锚点, 对锚点区间做**零容错**等长填充。产出与 crib 交叉验证 **100%
一致** 的子集:
- skill: 对齐 300 条 (crosscheck 300/300 = 100%)
- monster: 对齐 350 条 (crosscheck 350/350 = 100%)

但这些都是 crib 的**真子集 (0 新增)**, 对 `name_tables.NameResolver` 的覆盖**无提升**
(它本来就加载 crib), 故 `bokura_extract` **默认不写文件** (避免冗余 + 等长巧合错位的
污染风险)。

## 5. dungeon

仓库内**无任何 dungeon crib**。地牢/副本名 (如"巨塔遗迹""试炼之路") 散落在池里, 与
大量任务/对话叙事文本交错, 无 id 锚点可区分。**离线无法可靠提取。**

## 6. 要拿到完整表, 正确做法 = 读运行时 ZTable

表类 `SkillTableBase / MonsterTableBase / DungeonsTableBase ...` 都是
`ZTableRow<int>` + `ReadProxy proxy_`, 在游戏启动时由 IL2CPP 加载并在内存里建好
`id -> row` 的字典, `get_Name()` 直接从内存读。完整准确的 `id -> 中文名` 应:
- 用内存探针 (`tools/mem_probe`, 管理员可用) attach 运行中的客户端;
- 定位每个 ZTable 单例的 `Dictionary<int, Row>` (或 row 数组 + id 数组);
- 遍历 row, 对每行调用/复刻 `ReadString(Name 列)` 读出名字。

这超出"只读 m0.pkg 离线解码"的范围, 是本任务的明确 fallback。

---

## 工具

- `bokura_decode.py` — gap 地图 + 字符串池 token 化 + crib 锚点对齐 (可复用三件套)。
  `python -m tools.tablekit.bokura_decode` 打印 gap/池概况。
- `bokura_extract.py` — 诊断: 报告对齐子集与交叉验证率 (默认不写)。
  `python -m tools.tablekit.bokura_extract` 仅诊断; `--write` 强制写子集 (不推荐)。
