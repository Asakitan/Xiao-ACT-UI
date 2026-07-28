# Launcher Provider Config

部署方参照 `SaoAuto.provider.example.json` 拷贝为 `SaoAuto.provider.json`（或任意路径），填入真实 build_id + server_ed25519_pubkey，然后启动：

```
SaoAuto.exe --config=path\to\SaoAuto.provider.json
```

## 服务器地址映射（云端不对等端口）

| 服务 | Python 服务器进程内部端口 (server_main.py / update_host_main.py) | 云上外部端口 (C++ 客户端应连) | Python 权威地址 (sao_auto/python/config.py) |
|---|---|---|---|
| license | 15522（`server_config.json.port`） | **15522**（外部直开，无反代） | `https://doi.sakisense.top:15522` |
| update  | 9973（`UPDATE_HOST_PORT` env）      | **15018**（云端反代 15018→9973） | `http://doi.sakisense.top:15018` |
| workshop| 与 update 同进程 9973                | **15018**（同上）                | `http://doi.sakisense.top:15018/api/v1/workshop` |

**Rule of thumb：**
- 服务器**进程内部**端口永远不改（现有云端部署契约）
- C++ 客户端**永远**只连**外部**端口（客户端在公网侧看不到内部端口）
- 部署方在 config 里填的都是**外部**地址

## C++ 客户端如何拿到 URL

- **license**：`license.endpoint` 字段 → `sao/launcher/provider_config.h::LicenseProviderConfiguration.endpoint` → `license_provider.cpp` 把它当基址拼 `/activate` 等操作名
- **workshop**：`workshop_client.cpp:47` `kDefaultBaseUrl` 已硬编码指向 `http://doi.sakisense.top:15018/api/v1/workshop`；env `SAO_WORKSHOP_BASE_URL` 或 API `sao_workshop_client_set_base_url()` 可覆盖
- **updater**：launcher 主动调 `sao_updater_check_for_update(current_version, url, ...)`，url 由 launcher 自定；建议约定为 `http://doi.sakisense.top:15018/update/<channel>/<target>`（例如 `http://doi.sakisense.top:15018/update/stable/windows-x64`），客户端自动追加 `/latest.json`
- **dev_publish CLI**：`sao_dev_publish` 用 `--host <URL>` 或 env `SAO_UPDATE_HOST`

## Config JSON 关键字段

```json
{
  "license": {
    "enabled": true,
    "endpoint": "https://doi.sakisense.top:15522",     // 必填, 外部端口
    "build_id": "SaoAuto-5.1.0+ab12cd3",                // 必填, 每 build 唯一
    "server_ed25519_pubkey": "<64-hex>",                // 必填, license_server 首启打印
    "responses_prevalidated": false,                    // 必须 false 否则拒配
    "heartbeat_interval_ms": 300000                     // 5min, 与服务器建议值一致
  },
  "plugins": {
    "enabled": true,
    "roots": ["../plugins"],                            // 至少一非空
    "max_depth": 1
  }
}
```

## 拿 `server_ed25519_pubkey` 三条路

1. 首次跑 `python license_server/server_main.py`，stdout 里搜 `[license_server] Ed25519 pubkey:`
2. 已运行的 server：`curl https://doi.sakisense.top:15522/pubkey` → `{"ed25519_pubkey_hex":"..."}`
3. 管理员：`curl -H "Authorization: Bearer <admin_token>" https://doi.sakisense.top:15522/admin/pubkey`

## 常见部署陷阱

- **忘了填 build_id** → `/activate` 返回 400 `"build_id required for protocol_v2"`
- **build_id 填了但服务端没上传 build_master_key** → `/activate` 返回 400 `"build_id X not found or deprecated"`；用 `POST /admin/upload_build_key` 上传
- **`responses_prevalidated: true`** → launcher 启动时 `provider_config` 加载失败退出
- **`endpoint` 填内部端口 9973** → 云端外网无 9973，客户端无法连；必须填 **15522 (license) 或 15018 (update/workshop)**
- **反代未设 `X-Forwarded-Proto` / `X-Forwarded-Host`** → update `/latest.json` 里返回的 `url` 会用内网 host，客户端 `sao_updater_download` 拉不到
