# Launcher Provider Config

部署方参照 `SaoAuto.provider.example.json` 拷贝为 `SaoAuto.provider.json`（或任意路径），填入真实 build_id、license 公钥/SPKI pin 与 native update manifest，然后启动：

```
SaoAuto.exe --config=path\to\SaoAuto.provider.json
```

## 服务器地址映射（云端不对等端口）

| 服务 | Python 服务器进程内部端口 (server_main.py / update_host_main.py) | 云上外部端口 (C++ 客户端应连) | Python 权威地址 (sao_auto/python/config.py) |
|---|---|---|---|
| license | 15080（`server_config.json.port`） | **15522**（端口映射） | `https://x2.sjcmc.cn:15522` |
| update  | 9973（`UPDATE_HOST_PORT` env）     | **15018**（端口映射） | `http://x2.sjcmc.cn:15018` |
| workshop| 与 update 同进程 9973               | **15018**（同上）       | `http://x2.sjcmc.cn:15018/api/v1/workshop` |

**Rule of thumb：**
- 服务器**进程内部**端口永远不改（现有云端部署契约）
- C++ 客户端**永远**只连**外部**端口（客户端在公网侧看不到内部端口）
- 部署方在 config 里填的都是**外部**地址

## C++ 客户端如何拿到 URL

- **license**：`license.endpoint` 字段 → `sao/launcher/provider_config.h::LicenseProviderConfiguration.endpoint` → `license_provider.cpp` 把它当基址拼 `/activate` 等操作名
- **workshop**：`workshop_client.cpp` 的 `kDefaultBaseUrl` 指向 `http://x2.sjcmc.cn:15018/api/v1/workshop`；env `SAO_WORKSHOP_BASE_URL` 或 API `sao_workshop_client_set_base_url()` 可覆盖
- **updater**：`update.manifest_url` 使用最终 `.json` 公网 URL；当前 C++ 独立通道固定为 `http://x2.sjcmc.cn:15018/update/stable/windows-x64-native/latest.json`。常规 GUI 启动后后台 check/download，helper ready 后正常退出、overlay apply 并重启；smoke/exit-after-init/RTIO operator 跳过
- **dev_publish CLI**：`sao_dev_publish` 用 `--host <URL>` 或 env `SAO_UPDATE_HOST`；update route 强制为 `stable/windows-x64-native`，上传前会验证文件确为 `sao_pack --zip` 生成的确定性 UTF-8/STORED ZIP32（含 layout/CRC）

## Config JSON 关键字段

```json
{
  "license": {
    "enabled": true,
    "endpoint": "https://x2.sjcmc.cn:15522",           // 必填, 外部端口；服务端内部 15080
    "build_id": "SaoAuto-0.2.0+ab12cd3",                // 必填, 每 build 唯一
    "server_ed25519_pubkey": "<64-hex>",                // 必填, license_server 首启打印
    "server_tls_spki_sha256": "<64-hex>",               // 必填, license TLS SPKI SHA-256
    "responses_prevalidated": false,                    // 必须 false 否则拒配
    "heartbeat_interval_ms": 300000                     // 5min, 与服务器建议值一致
  },
  "update": {
    "enabled": true,
    "manifest_url": "http://x2.sjcmc.cn:15018/update/stable/windows-x64-native/latest.json"
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
2. 已运行的 server：`curl https://x2.sjcmc.cn:15522/pubkey` → `{"ed25519_pubkey_hex":"..."}`
3. 管理员：`curl -H "Authorization: Bearer <admin_token>" https://x2.sjcmc.cn:15522/admin/pubkey`

## 常见部署陷阱

- **忘了填 build_id** → `/activate` 返回 400 `"build_id required for protocol_v2"`
- **build_id 填了但服务端没上传/无法解包 build_master_key** → `/activate` 返回 503，且不会写 activation；用 `POST /admin/upload_build_key` 上传后重试
- **`responses_prevalidated: true`** → launcher 启动时 `provider_config` 加载失败退出
- **客户端填内部端口 15080/9973** → 外网侧不可达；License 必须连 **x2.sjcmc.cn:15522**，Update/Workshop 必须连 **x2.sjcmc.cn:15018**
- **`UPDATE_HOST_PUBLIC_BASE_URL` 不合法或不是公网映射地址** → update host 启动失败或 `/latest.json` 返回不可达 URL；生产值固定为 `http://x2.sjcmc.cn:15018`
- **native 与冻结 Python 共用 `windows-x64` target** → 两条版本线/包布局会互相污染；C++ 发布和客户端必须使用 **`windows-x64-native`**
- **上传普通 deflate ZIP** → native helper fail-closed；先用 `sao_pack --zip` 生成受控 STORED ZIP32，再由 `sao_dev_publish update` 上传
- **manifest 为 `0.0.0` 且 URL/hash 为空** → 服务已连通但尚未发布 native release，launcher保持运行且不下载

## 当前更新信任边界

公网 update 端口按现有部署契约为 HTTP。客户端会限制 manifest/artifact 同源、禁重定向、限制 512 MiB，并对下载包做 SHA-256 与 ZIP entry CRC/路径校验；这些提供传输/文件完整性，**不等同于签名来源认证**。正式发布签名或 HTTPS pin 接入前，不把该链路标为 cryptographically authenticated。
