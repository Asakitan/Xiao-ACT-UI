# SAO Auto

`SAO Auto` 是一个面向《星痕共鸣》的外部 HUD / 自动化工具实验项目。

当前主线能力包括：

- WebView HUD
- 抓包驱动的 HP / 等级 / 技能状态同步
- 纯识图驱动的 STA 百分比识别
- 内置自动按键脚本与云端参数库

## 重要说明

- 本项目当前路线是“外部工具”路线：抓包、窗口识图、WebView HUD、模拟按键。
- 不建议继续尝试内存注入或类似内存修改方案。
- 这条路已经实测被踢了两次 `1` 分钟，所以 README 里明确记录下来：内存注入目前不可行，也不作为后续开发方向。

## 当前实现方向

- `HP / 等级 / 技能状态` 主要来自 `packet`
- `STA` 主要来自窗口内固定 ROI 识图
- `Burst Ready` 由技能状态触发，在独立 `skillfx` 面板渲染
- 自动按键只基于结构化条件和当前状态执行

## 运行

```bash
pip install -r requirements.txt
python main.py
```

常用模式：

- 普通 HUD：`python main.py`
- 测试模式：`python main.py --test`
- 无界面模式：`python main.py --headless`

## 目录概览

```text
sao_auto/
├─ main.py                 # 入口
├─ config.py               # 配置与默认设置
├─ game_state.py           # 统一状态模型与缓存
├─ packet_capture.py       # 抓包
├─ packet_parser.py        # 协议解析
├─ packet_bridge.py        # packet -> GameState
├─ recognition.py          # STA / ROI 识图
├─ auto_key_engine.py      # 自动按键引擎、本地脚本模型、云端客户端
├─ sao_webview.py          # HUD / Menu / SkillFX 主进程
├─ web/
│  ├─ hp.html
│  ├─ menu.html
│  └─ skillfx.html
└─ server/
   └─ app.py               # FastAPI 脚本库服务端
```

## 自动按键

自动按键支持：

- 本地 profile 管理
- 动作顺序、按键、次数、延迟、重触发间隔
- 结构化条件：
  - `hp_pct_gte`
  - `hp_pct_lte`
  - `sta_pct_gte`
  - `burst_ready_is`
  - `slot_state_is`
  - `profession_is`
  - `player_name_is`
- 导出为外部 JSON
- 从服务器搜索、下载、上传脚本

默认服务端目标为 `47.82.157.220`。

## 致谢

这个项目在开发过程中直接受益于以下开源仓库和作者公开的源码、协议整理、思路与工程经验。非常感谢他们的分享：

- [`StarResonanceDamageCounter`](../StarResonanceDamageCounter/README.md)
- [`StarResonanceDps`](../StarResonanceDps/README.md)
- [`ok-star-resonance`](../ok-star-resonance/README.md)

补充说明：

- 本项目里引用、参考、学习到的思路和实现非常多。
- 若某些第三方代码、资源或生成产物来自上游项目，则仍应遵循其原始许可证。
- 本仓库 `sao_auto` 目录下未特别注明的原创部分，按 MIT License 发布。

## License

- `sao_auto` 目录下未特别注明的原创代码：`MIT`
- 第三方项目、第三方资源、第三方生成产物：以各自原始许可证为准

