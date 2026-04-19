# SAO Auto Update Host

独立的远程更新服务. 与 `server/app.py` 中的脚本仓库服务解耦, 不共享部署.

## 启动

```powershell
cd sao_auto/update_host
pip install fastapi uvicorn
$env:UPDATE_HOST_RELEASE_DIR = "$pwd\releases"
uvicorn app:app --host 0.0.0.0 --port 9330
```

或使用打包后的 exe:

```powershell
cd update_host
.\UpdateHost.exe
```

客户端默认访问 `http://47.82.157.220:9330` (见 `config.DEFAULT_UPDATE_HOST`).
可在 `settings.json` 中设置 `update_host` 覆盖.

## 发布新版本

```powershell
# runtime-delta: 路径与客户端 BASE_DIR 一致 (顶层模块化文件夹)
#   先用 build_delta.py 打包, 再用 publish_release.py 发布
python ..\build_delta.py --version 2.1.1 --files runtime/sao_gui.py web/menu.html assets/sounds/click.wav
python publish_release.py --version 2.1.1 \
  --package ..\dist\delta\update-2.1.1-runtime-delta.zip \
    --type runtime-delta \
    --notes "修复 BossHP 闪屏; 新增 DPS 详细视图"

# 强制更新 (低于 minimum 的客户端必须升级才能进入)
python publish_release.py --version 2.2.0 \
    --package ..\dist\delta\update-2.2.0-runtime-delta.zip \
    --type runtime-delta \
    --minimum 2.1.0 --force \
    --notes "重要安全修复"

# 全量包 (改了 exe 启动器 / 内置 Python runtime / DLL 等)
#   先 pyinstaller --clean --noconfirm XiaoACTUI.spec, 再 build_full_package.py
python ..\build_full_package.py --version 3.0.0
python publish_release.py --version 3.0.0 \
    --package ..\dist\full\XiaoACTUI-3.0.0-full-package.zip \
    --type full-package \
    --notes "重大版本"
```

## 客户端目录结构 (模块化 onedir 部署)

```
XiaoACTUI/
  XiaoACTUI.exe          # 主启动器 (~3MB)
  update.exe             # 独立更新助手 (~3MB), 主 exe 退出后由它替换文件
  web/                   # HTML / CSS / JS  (模块化, 可独立 delta)
  assets/                # 字体 / 音效 / skill_names.json
  proto/                 # protobuf
  runtime/               # PyInstaller 的 Python 解释器 + 我们的 .py + 依赖 DLL
    sao_gui.py
    sao_updater.py
    python311.dll
    ... (所有第三方包)
  staging/               # 下载中的更新包 + pending.json
  backup/<ver>/          # 应用 delta 时自动备份被覆盖文件
  update_apply.log
  settings.json
  exports/   temp/
```

## 更新模式

- **runtime-delta**: zip 内路径对齐 BASE_DIR (顶层模块文件夹), 例如 `runtime/sao_gui.py` / `web/menu.html` / `assets/sounds/click.wav`。客户端 update.exe 直接写入对应位置, 备份原文件到 `backup/<ver>/`。**不允许覆盖启动器 exe 本身**。
- **full-package**: zip 含 `XiaoACTUI.exe` + `update.exe` + `runtime/...` + 模块化文件夹, 客户端会替换全部文件 (含启动器)。用于 PyInstaller 重打、Python runtime 升级、内置 DLL 变更等场景。

## 目录结构

```
update_host/
  app.py
  publish_release.py
  releases/
    stable/
      windows-x64/
        manifest.json
        update-2.1.1-runtime-delta.zip
    beta/
      windows-x64/
        manifest.json
        ...
```

## 端点

- `GET /api/health` — 健康检查
- `GET /api/update/latest?channel=stable&target=windows-x64&current=2.1.0` — 拉取 manifest
- `GET /api/update/summary` — 列出所有 channel/target
- `GET /downloads/<channel>/<target>/<file>` — 下载更新包

## Manifest schema

| 字段              | 类型    | 说明                                                |
|-------------------|---------|-----------------------------------------------------|
| version           | string  | 新版本号                                            |
| minimum_version   | string  | 低于该版本的客户端必须升级                          |
| force_update      | bool    | 强制升级 (覆盖用户跳过)                             |
| package_type      | string  | `runtime-delta` 或 `full-package`                   |
| target            | string  | 例如 `windows-x64`                                  |
| channel           | string  | `stable` / `beta`                                   |
| download_url      | string  | 下载地址 (相对 `/downloads/...` 或绝对 URL)         |
| sha256            | string  | 下载完成后客户端校验                                |
| size              | int     | 字节数                                              |
| notes             | string  | 发布说明                                            |
| published_at      | string  | ISO8601                                             |

## 安全

- 客户端使用 HTTPS + SHA256 校验包完整性 (manifest 信任远端 host)
- v1 不做包签名; 建议 host 启用 HTTPS, 由网关层做证书认证
- 服务器侧的 `force_update` / `minimum_version` 是强制升级控制点
