# SAO Auto Update Host

独立的远程更新服务. 与 `server/app.py` 中的脚本仓库服务解耦, 不共享部署.

## 启动

```powershell
cd sao_auto/python/update_host
pip install fastapi uvicorn
$env:UPDATE_HOST_RELEASE_DIR = "$pwd\releases"
python update_host_main.py
```

生产服务器由 `server_service.py` 以 Windows SCM 服务托管本源码入口；
`build_server_deploy_zip.bat` 生成可直接作为 `--root` 使用的纯源码部署包，不包含冻结 EXE。

客户端默认访问 `https://x2.sjcmc.cn:15018`；服务端在内部 `9973` 上直接提供 TLS，公网通过 TCP 端口映射把 `15018` 转发到 `9973`。
公网基址固定为 `https://x2.sjcmc.cn:15018`，不读取测试环境或其他外部 origin 覆盖；可在 `settings.json` 中设置 `update_host` 覆盖客户端地址。

## 生产 TLS 与 SCM 部署

部署包根目录布局如下：

```
server_service.py
update_host/
  update_host_main.py
  start_server.bat
  ...
license_server/
  server.crt
  server.key
```

`server.crt` 和 `server.key` 是自签 TLS 证书/私钥，只存在于本地部署包的 `license_server/` 目录，不进入源码仓库。打包脚本要求证书和私钥已经存在：默认读取源码旁的 `python/license_server/server.crt`、`python/license_server/server.key`，也可用 `UPDATE_HOST_SSL_CERTFILE` 与 `UPDATE_HOST_SSL_KEYFILE` 指定现有文件；缺任一文件时打包立即失败。脚本只把它们复制到被 Git 忽略的部署输出目录，再写入 zip。

客户端连接 `https://x2.sjcmc.cn:15018` 时应校验该证书的 SPKI pin。更换自签证书或密钥后，必须同步更新客户端 SPKI pin；证书链不依赖公网 CA。

服务端监听的是 **9973/TLS**；网络边界需要配置 **15018/TCP → 部署机 9973/TCP** 映射，并保持 TLS 字节流直通。SCM 运行示例：

```powershell
python server_service.py run --service-name SaoUpdateHost --role update --root C:\SAO\update-host
```

其中 `C:\SAO\update-host` 必须是上述部署包解压后的根目录.

## 发布新版本

```powershell
# runtime-delta: 路径与客户端 BASE_DIR 一致 (顶层模块化文件夹)
#   先用 build_delta.py 打包, 再用 publish_release.py 发布
python ..\build_delta.py --version 2.1.1 --files runtime/sao_gui.py web/menu.html assets/sounds/click.wav
python publish_release.py --version 2.1.1 \
  --package ..\dist\delta\update-2.1.1-runtime-delta.zip \
    --type runtime-delta \
    --notes "修复覆盖层闪屏; 新增详细视图"

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
  releases/
    update/
      stable/
        windows-x64/
          latest.json
          anchor.json
          artifacts/
            update-2.1.1.zip
    workshop/
      _catalog.json
      <plugin-id>/
        meta.json
        versions/
```

## 端点

- `GET /health` — 健康检查
- `GET /update/<channel>/<target>/latest.json` — C++ 客户端读取 5 字段 manifest
- `GET /update/<channel>/<target>/anchor` — 读取当前服务端 git 锚点
- `POST /update/<channel>/<target>/anchor` — 手动同步锚点（需要 `X-API-Key`）
- `POST /update/<channel>/<target>/publish` — 单流上传更新包并推进锚点
- `GET /update/<channel>/<target>/artifacts/<file>` — 下载更新包
- `GET /update/<channel>/<target>/history` — 列出已发布 artifacts
- `GET /api/v1/workshop/plugins`、`GET /api/v1/workshop/plugins/<id>`、`GET /api/v1/workshop/plugins/<id>/download` — Workshop 浏览/详情/下载
- `POST /api/v1/workshop/plugins/<id>/publish` — Workshop 单流发布（需要 `X-API-Key`）
- `GET /api/v1/workshop/admin` — Workshop 网页管理界面；API key 仅保存在当前浏览器 tab
- `GET /api/v1/workshop/admin/plugins` — 管理目录与版本列表（需要 `X-API-Key`）
- `PATCH /api/v1/workshop/plugins/<id>` — 编辑名称、标签、作者、说明、游戏与最低版本（需要 `X-API-Key`）
- `DELETE /api/v1/workshop/plugins/<id>[?version=<old>]` — 删除整插件或非 active 旧版本（需要 `X-API-Key`）

## Manifest schema

| 字段              | 类型    | 说明                                                |
|-------------------|---------|-----------------------------------------------------|
| version           | string  | 新版本号                                            |
| url               | string  | artifact 相对路径；响应时按固定 public base URL 展开 |
| sha256            | string  | 下载完成后客户端校验                                |
| size              | int     | 字节数                                              |
| notes             | string  | 发布说明                                            |

服务端内部 manifest 还保存 channel/target、force/minimum、commit/anchor 与发布时间；客户端响应严格只返回 `version/url/sha256/size/notes`。

## 安全

- 当前公网契约为 `https://x2.sjcmc.cn:15018` + SHA256 + TLS/SPKI pin；manifest 的绝对 URL 使用固定公网基址生成，不信任请求的 Host/Forwarded 头
- v1 不做包签名；发布 key 缺失返回 503，错误 key 返回 403
- 服务器侧的 `force_update` / `minimum_version` 是强制升级控制点
- 管理页不把 key 写进 HTML、URL 或持久化配置，只通过 `X-API-Key` header 提交；页面启用 no-store、CSP nonce 与防 iframe 响应头。公网使用 HTTPS，管理时的 key 传输受 TLS 保护
