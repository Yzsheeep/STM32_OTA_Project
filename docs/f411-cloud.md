# F411 云端固件服务 · 本地验收

固件服务使用 Node.js 24 LTS，已测版本为 24.15.0，无需 npm install。Windows 上位机已接入该服务。在项目根目录打开 PowerShell。

## 一键验收

```powershell
node --version
node --test cloud/test_service.mjs
```

默认读取 `app_f411/MDK-ARM/app_f411/app_f411.bin`。如需指定另一个实际构建产物，先设置 `$env:OTA_TEST_BIN` 为 BIN 的完整路径。测试会在系统临时目录创建独立数据库和随机令牌，通过本机回环地址和随机端口启动服务。在测试结束时清理该临时目录。需要允许本机 TCP 回环连接。

通过标准为全部子测试通过、失败数为零。测试包含上传中断；日志里出现一次 `ECONNRESET` 是该用例主动断开连接。损坏测试只修改临时库。所有测试均不访问串口。

## 手动上传、发布、下载

以下步骤可选，用于实际观察草稿和发布状态。第一个 PowerShell 窗口启动一个新的本地实例。记下显示的目录路径供第二个窗口使用。凭据只写入当前用户的临时目录。

```powershell
$work = Join-Path $env:TEMP ("f411-local-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Select-Object FullName
$env:OTA_TOKEN_FILE = Join-Path $work 'admin_token'
$env:OTA_DB_PATH = Join-Path $work 'firmware.sqlite'
$env:OTA_BIND = '127.0.0.1'
$env:OTA_PORT = '18080'
node -e "require('node:fs').writeFileSync(process.env.OTA_TOKEN_FILE,require('node:crypto').randomBytes(32).toString('base64url'),{flag:'wx',mode:384})"
node cloud/service.mjs
```

第二个窗口也从项目根目录运行。将占位符替换为上一步显示的目录，版本按 BIN 的内嵌版本填写。

```powershell
$work = '<上一步显示的临时目录>'
$api = 'http://127.0.0.1:18080/api/v1/targets/f411-ota'
$admin = @{ Authorization = 'Bearer ' + [IO.File]::ReadAllText((Join-Path $work 'admin_token')).Trim() }
$bin = (Resolve-Path 'app_f411/MDK-ARM/app_f411/app_f411.bin').Path
$version = '1.0.6'
Invoke-RestMethod -Method Post -Uri "$api/admin/releases?version=$version" -Headers $admin -ContentType 'application/octet-stream' -InFile $bin
```

此时响应应为 `state=draft`。全新数据库中查询 `$api/latest` 应返回 404。继续发布并下载。

```powershell
Invoke-RestMethod -Method Post -Uri "$api/admin/releases/$version/publish" -Headers $admin
$manifest = Invoke-RestMethod "$api/latest"
$manifest
$download = Join-Path $work 'download.bin'
Invoke-WebRequest -Uri ([uri]::new([uri]$api, $manifest.download_url)) -OutFile $download
((Get-FileHash $bin).Hash -eq (Get-FileHash $download).Hash) -and ((Get-FileHash $download).Hash -eq $manifest.sha256)
```

最后一行应为 `True`。在第一个窗口按 Ctrl+C 后，使用同一窗口和数据库路径重新运行 `node cloud/service.mjs`，再次查询应得到相同版本。结束后按 Ctrl+C 停止。手动验收目录保留供查看，令牌文件应仅对当前用户可读。

## API 约定

所有路径均相对 `/api/v1/targets/f411-ota`。读接口仅设计给可信回环或 SSH 隧道内的客户端。三个管理接口均要求 `Authorization: Bearer <token>`。启动服务前提供 32 字节随机数生成的 base64url 令牌文件，通过 `OTA_TOKEN_FILE` 指定。文件缺失或格式错误时服务启动失败。

| 方法与路径 | 行为 |
|---|---|
| POST /admin/releases?version=x.y.z | 原始 BIN 请求体，Content-Type 为 application/octet-stream。可选 URL 编码的 changelog 参数最多 1024 个 UTF-8 字节。返回 201 草稿。 |
| POST /admin/releases/{version}/publish | 校验存储内容后发布。重复发布同一完好版本返回 200。 |
| DELETE /admin/releases/{version} | 删除草稿返回 204。已发布版本返回 409。 |
| GET /latest | 返回已发布的最高数字版本。 |
| GET /releases/{version} | 返回该发布版本的清单。 |
| GET /releases/{version}/firmware.bin | 检查库存固件完整性后返回原始字节和 Content-Length。下载中断可从该固定 URL 重新完整获取。 |

清单含 target、app_address、latest_version、size、sha256、crc32、changelog、download_url。download_url 为同源站点根路径。版本不可覆盖。错误响应是带 error 字段的 JSON，认证失败 401、无可见版本 404、重复版本 409、超限 413、错误内容类型 415、镜像或版本无效 422、库存摘要不匹配 500。

服务检验 F411 的 16 KiB 上限、长度四字节对齐、初始栈指针范围及八字节对齐、Thumb 复位入口及实际镜像范围、内嵌 APP_VERSION。上传者须使用可信 F411 构建产物；裸 BIN 未提供经过签名的芯片身份。SHA-256 供上位机验证下载完整性。服务单实例运行，一个 SQLite 数据库同时保存 BIN 和元数据。

## Docker 配置与后续部署边界

Docker 构建上下文为 cloud 目录。Compose 项目名为 f411-firmware，仅绑定宿主机 IPv4 回环端口，使用独立命名卷与 unless-stopped 重启策略。容器内服务以 UID 1000 运行。根文件系统只读，数据库目录使用可写挂载。配置示例见 cloud/.env.example。令牌文件必须位于仓库外，且实际挂载后容器用户可读。

以下是部署阶段待确认的命令及影响，本次只在 Windows 本机验收服务。

| 步骤 | 命令或动作 | 影响及回退 |
|---|---|---|
| 预检 | 配置外部令牌路径后执行 `docker compose -f cloud/compose.yaml config --quiet`，核实目标端口空闲、命名卷、令牌权限、SSH 主机指纹与本地转发可用性 | 仅读配置及现状 |
| 启动 | `docker compose -f cloud/compose.yaml up -d --build` | 构建镜像、创建专用容器、网络和数据卷。端口默认 127.0.0.1:18080 |
| Windows 转发 | `ssh -N -o ExitOnForwardFailure=yes -L 127.0.0.1:18080:127.0.0.1:18080 developer@<ECS公网IP>` | 通过已核对主机密钥的 SSH 连接加密公网流量。Ctrl+C 关闭隧道 |
| 验收 | 上传、发布、比对下载摘要；用同一命名卷重新创建服务后，再次查询与下载 | 确认容器重建后数据仍然可用 |
| 停用 | `docker compose -f cloud/compose.yaml down` | 移除本项目容器和网络，保留数据卷。勿带 -v。更新前应停服务备份数据库并保留旧镜像，以便恢复 |

## Windows GUI 验收

运行 `uv run --with pyserial --with requests python cloud/test_gui.py` 可复验实际 Tk 窗口、临时 Node 服务及失败刷新。测试只检查本地/云端交给串口工作线程的参数。手动验收可沿用前文启动并发布过固件的服务，执行以下命令。

```powershell
uv run --with pyserial --with requests python ota_gui.py
```

默认地址为 `https://47.107.50.1/api/v1/targets/f411-ota/latest`。点击“检查云端更新”。未连接设备也可以下载，界面应显示已就绪、设备版本未查询，日志含发布版本及 SHA-256。要复验本地服务，在 API 输入框填入 `http://127.0.0.1:18080/api/v1/targets/f411-ota/latest`，停止该测试服务并重新查询，会清空旧缓存。切换到本地 BIN 仍能单独使用。设备已连接且版本相同或更高时，只检查版本。

在 PowerShell 设置 `$env:OTA_PUBLIC_CHECK='1'` 后执行 GUI 测试，会额外验证默认公网入口，测试假设本地参考 BIN 与公网最新版一致。测试使用隐藏的真实 Tk 窗口，拦截硬件端口枚举、串口连接与升级线程启动。服务端回归命令为 `node --test cloud/test_service.mjs`。公网证书维护及 ECS 配置核对见本文末尾。

## 首次 ECS 上线步骤（待确认）

以下针对首次部署。先核对 ECS 主机密钥指纹，使用自己已有的 SSH 密钥。由管理员提供实际公网 IP。先在 ECS 做只读预检，记录 my-nginx 的容器 ID、启动时间、端口及网站响应。若 18080 已占用、存在同名项目或卷、Docker 启用了用户命名空间映射，先核实现场再定参数。

```sh
sudo docker compose version
sudo docker info --format '{{json .SecurityOptions}}'
sudo docker inspect --format '{{.Id}} {{.State.StartedAt}} {{json .NetworkSettings.Ports}}' my-nginx
sudo docker ps -a --filter label=com.docker.compose.project=f411-firmware
sudo docker volume ls --filter name=f411-firmware
ss -ltn 'sport = :18080'
curl -sS -o /dev/null -w '%{http_code}\n' http://127.0.0.1/
```

在 ECS 以 developer 新建专用目录，目录已存在时先检查，避免覆盖。Windows 仅复制以下三个部署文件到新目录。

```sh
mkdir -m 700 ~/f411-ota-v1
```

```powershell
$ecs = 'developer@<ECS公网IP>'
scp cloud/service.mjs cloud/Dockerfile cloud/compose.yaml "${ecs}:f411-ota-v1/"
```

在 ECS 生成仓库外的随机管理令牌。以下权限适用于标准 rootful Docker、容器 UID 1000。随机值不输出到终端，不进入命令历史。目录已存在时停止，避免轮换已有令牌。

```sh
sudo mkdir -m 0700 /etc/f411-ota
sudo sh -ec 'set -C; umask 077; openssl rand -hex 32 > /etc/f411-ota/admin_token'
sudo chown 1000:1000 /etc/f411-ota/admin_token
cd ~/f411-ota-v1
ota_compose() { sudo env OTA_TOKEN_FILE=/etc/f411-ota/admin_token docker compose -f "$HOME/f411-ota-v1/compose.yaml" "$@"; }
ota_compose config --quiet && ota_compose up -d --build
ota_compose ps
ota_compose port api 18080
curl -sS -i http://127.0.0.1:18080/api/v1/targets/f411-ota/latest
```

端口输出应为 `127.0.0.1:18080`，空库应返回服务的 JSON 404。重复预检中的 Nginx inspect 与网站请求，确认容器 ID、启动时间和网站响应保持原状。再在 Windows 按上表建立隧道，保持 SSH 窗口打开；本地同端口的测试服务应先退出。首次发布由管理员单独完成，上位机只读查询和下载。安全组沿用已有 SSH 入站，Nginx 无需重载。

**首次部署回退**：在 Windows 隧道窗口按 Ctrl+C。在上述 ECS shell 执行 `ota_compose down`，然后再次核对 Nginx 和网站响应。专用数据卷、令牌和部署目录都保留，可用同一个 `ota_compose up -d` 恢复服务。此过程不执行全局 Docker 清理。

## 已部署环境的软件验收与维护（2026-11-22）

Windows 日常查询、下载走公网 443。管理员沿用 SSH 隧道与 Bearer 令牌。公网实测 GET latest 为 200，两个管理路径为 404，未知版本下载为 404，HEAD latest 为 405，原 80 网站为 200。上位机取得的 1.0.7 为 9936 字节，CRC32 为 7F115067，SHA-256 为 537523c071ac184fd072e7f4f4599bab4c84068d8f6939a781ee0edafc60e181。

### ECS 只读检查

以下命令在 ECS 上执行，检查最近一次部署后的实际设置。token 只检查权限。Docker inspect 只输出指定字段。

```sh
systemctl is-enabled docker
systemctl list-timers --all 'snap.certbot*'
sudo docker inspect -f '{{.Name}} running={{.State.Running}} restart={{.HostConfig.RestartPolicy.Name}} restarts={{.RestartCount}} logs={{json .HostConfig.LogConfig}}' f411-firmware-api-1 f411-ota-https my-nginx
sudo docker inspect -f '{{range .Mounts}}{{println .Destination .Type .RW}}{{end}}' f411-firmware-api-1 f411-ota-https
sudo docker volume inspect -f '{{.Name}} {{.Driver}}' f411-firmware_firmware_data
sudo stat -c '%a %u:%g %n' /etc/f411-ota /etc/f411-ota/admin_token
sudo docker port f411-firmware-api-1
sudo docker logs --since 1h --tail 40 f411-firmware-api-1
sudo journalctl -u snap.certbot.renew.service --since '7 days ago' -n 30 --no-pager
```

API 与 HTTPS 容器应为 unless-stopped，Docker 服务应随系统启动。API 的 /data 应挂在命名卷上且可写；/run/secrets/admin_token 和 HTTPS 证书挂载应只读。令牌文件在仓库外，预期 600、UID 1000。重启策略不等于数据库备份。检查 Docker 日志驱动及现有轮转限制；若使用未限额的 json-file，建议在下次经批准重建 OTA 容器时单独设置 max-size=10m、max-file=3。避免调整全局 Docker 日志策略影响网站。报告时不贴令牌、私钥或完整访问日志。

### 短期 IP 证书续期方案（需批准，尚未上线）

首次签发的 webroot 是 /proc/某个PID/root/...。容器重新启动可能使它失效。新增的 prepare-webroot.sh 每次运行时读取 my-nginx 当前 PID，在宿主机私有验证目录中更新 .well-known 链接。公开挑战文件仍由现有 80 网站提供，不使用此前失败的 mount。reload-https.sh 仅对 f411-ota-ip 证书先执行 nginx -t，成功后平滑重载 443 容器。80 端口的网站不重载。

**变更影响**：增加两个 root 所有的脚本、一个私有验证目录与该证书的续期参数。测试 CA 会通过 80 访问临时挑战文件；测试 deploy-hook 会平滑重载独立 HTTPS 容器。实际签发并部署新证书仍需在真实续期后核对。以下为待审批命令，首次执行前检查备份路径和新目录均未被占用。

Windows 从项目根目录上传脚本：

```powershell
scp -o StrictHostKeyChecking=yes -i "$env:USERPROFILE\.ssh\id_ed25519_aliyun" cloud/https/prepare-webroot.sh cloud/https/reload-https.sh developer@47.107.50.1:f411-ota-v1/
```

ECS 在同一 shell 分步执行，每步成功后继续：

```sh
cd ~/f411-ota-v1
conf=/etc/letsencrypt/renewal/f411-ota-ip.conf
work=/var/lib/f411-certbot
sudo mkdir -m 0700 "$work" &&
sudo cp -p "$conf" "$work/renewal.before" &&
sudo install -m 0700 prepare-webroot.sh reload-https.sh "$work/" &&
sudo "$work/prepare-webroot.sh"
```

```sh
sudo /snap/bin/certbot reconfigure --cert-name f411-ota-ip \
  --webroot --webroot-path "$work" --webroot-map '{"47.107.50.1":"/var/lib/f411-certbot"}' \
  --pre-hook "$work/prepare-webroot.sh" --deploy-hook "$work/reload-https.sh"
sudo /snap/bin/certbot renew --cert-name f411-ota-ip --dry-run --run-deploy-hooks
```

检查测试续期成功且无 hook 报错。将公网握手返回的 serial、到期时间与磁盘证书对照，然后复验 GUI、管理路径 404 和原网站 200。后续真实续期后应看到新序列号及延后的到期时间。Certbot reconfigure 的测试及保存行为见 [官方续期文档](https://eff-certbot.readthedocs.io/en/stable/using.html#modifying-the-renewal-configuration-of-existing-certificates)。

```sh
sudo openssl x509 -in /etc/letsencrypt/live/f411-ota-ip/cert.pem -noout -serial -enddate
openssl s_client -connect 127.0.0.1:443 -servername 47.107.50.1 </dev/null 2>/dev/null | openssl x509 -noout -serial -enddate
```

**回退**：若 reconfigure 已保存，恢复本次操作前的参数文件：`sudo cp -p "$work/renewal.before" "$conf"`。保留脚本、备份目录和现有证书供排查。原参数中的 PID 只在对应容器进程仍存在时有效；回退后也要在证书到期前解决续期。此次方案不修改 Nginx 配置、安全组或 Docker Engine。

**后续实板验收**（独立安排）：先通过 V 读设备版本，确认低于已发布版本，再检查公网更新、核对摘要、人工启动 OTA，最后以 APP 实际回复目标版本验收。上述软件用例只验证到工作线程的字节与版本参数交接。
