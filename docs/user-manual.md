# Lingqu Memory Service 使用手册

本文面向构建者、部署者与运维者：如何构建各形态二进制、配置与运行 daemon、
使用 admin CLI、做 systemd 部署与监控、打包发布与升级回滚、以及常见排错。
架构与契约细节见 [design.md](design.md)；下游集成见
[integration-ub-sim.md](integration-ub-sim.md) 与
[integration-ds4.md](integration-ds4.md)。

## 1. 构建

所有构建入口集中在 `apps/mem_service/Makefile`（`make -C apps/mem_service
<target>`）。编译器变量：guest 交叉 `CC=aarch64-linux-gnu-gcc`（默认
`CFLAGS=-O2 -Wall -Wextra -static`），宿主机 `HOST_CC=cc`。

### 1.1 guest 交叉构建（aarch64）

```bash
make -C apps/mem_service all
```

- 产出 `linqu_mem_service` 与 `linqu_mem_service_core`（静态链接的 guest
  daemon/CLI）。
- 当 `llm_infer` 可用时 `all` 还会构建 `linqu_mem_service_qwen3`（Qwen3
  适配器检查二进制）：

```bash
make -C apps/mem_service linqu_mem_service_qwen3 \
    LLM_INFER_ROOT=<ub_sim 检出>/guest-linux/aarch64
```

### 1.2 宿主机构建与 smoke

```bash
make -C apps/mem_service linqu_mem_service_host host-artifact-smoke
```

`linqu_mem_service_host` 用宿主机编译器构建（macOS/Linux 均可）；
`host-artifact-smoke` 依次运行 `--smoke`、`version`、`release-fixtures`、
`package-fixtures`、wire/admin/compat/store/restore/retention/alert/ops/
typed-payload/transport-block 等全部 fixture 门禁。

### 1.3 provider 构建与 smoke

```bash
make -C apps/mem_service tcp-provider-smoke    # 构建 linqu_mem_service_provider_tcp + protocol-fixtures
make -C apps/mem_service roce-provider-smoke   # 构建 linqu_mem_service_provider_roce + protocol-fixtures
```

- TCP provider：宿主机编译器 + `-pthread`，跨平台可用。
- RoCE provider：需要 Linux + libibverbs/librdmacm（链接
  `-lrdmacm -libverbs`），在无 librdma 的主机上构建会失败。
- `network-transport-block-smoke`：TCP-loopback 传输块门禁（会用到 socket）。

### 1.4 测试

```bash
python3 -m unittest discover tests    # 在仓库根目录执行
```

覆盖 daemon runtime（含 pretraining worker 门禁）、record 回收、TCP/RoCE
provider 行为。

## 2. 配置文件

daemon 通过 `serve --config <file>` 读取严格 text-kv 配置。schema 冻结于
`apps/mem_service/configs/mem_service.conf.schema`（
`mem_service_config_schema_version=1`）；未知/重复/畸形字段 fail-closed。

字段一览（`listen` 必填，其余可选）：

| 字段 | 说明 |
| --- | --- |
| `listen` | 服务端点；`auth_mode=none` 下必须是 `unix:<path>` |
| `store` | 快照 store 路径；append-only 幂等/审计 journal 为 `<store>.journal` |
| `node_id` / `cluster_id` | 稳定的部署节点/集群标识 |
| `storage_root` | 持久目录根：catalog manifest、派生 store、sealed block 与 quarantine 布局 |
| `backend` | 持久后端选择：`snapshot` 或 `snapshot+journal` |
| `max_records` / `max_payload_bytes` | 部署配额（record 容量 / wire payload 上限） |
| `retention` | 审计保留：`manual` 或 `audit-log:<events>` |
| `checkpoint_retention` | checkpoint 保留：`manual` 或 `latest:<records>` |
| `record_retention` | record 保留：`manual`、`latest:<n>`、`ttl-ms:<age>`，支持 `kind:`/`tenant:` 作用域 |
| `encryption` | 目前仅 `none`；其它模式 fail-closed |
| `auth_mode` | 目前仅 `none`（本地部署）；网络服务端点被拒绝 |
| `metrics_mode` | 目前仅 `text-kv` |
| `metrics_listen` | 可选 HTTP metrics 端点，必须是 `tcp:127.0.0.1:<port>` |
| `adapter_enablement` | 启用的适配面：`core` 或 `qwen3` |

随仓库提供的配置：

- `configs/mem_service.example.conf` — 开发者 `/tmp` 示例。
- `configs/mem_service.runtime.conf` — 主服务部署配置，安装为
  `etc/lingqu/mem_service/mem_service.conf`：`/run/lingqu/mem_service.sock`、
  `/var/lib/lingqu/mem_service`、metrics `tcp:127.0.0.1:9900`。
- `configs/mem_service.host.runtime.conf` — host 服务部署配置，安装为
  `etc/lingqu/mem_service/mem_service.host.conf`：
  `/run/lingqu/mem_service_host.sock`、`/var/lib/lingqu/mem_service_host`、
  metrics `tcp:127.0.0.1:9901`（与主服务可同时运行）。
- `configs/providers/roce/mesh.example.conf` — RoCE mesh 部署示例（拷出仓库
  后替换文档地址/设备/路径）。

## 3. 运行 daemon

### 3.1 直接运行

```bash
# 最小启动（内存态）
linqu_mem_service_host serve --listen unix:/tmp/linqu_mem_service.sock

# 带持久化
linqu_mem_service_host serve --listen unix:/tmp/ms.sock --store /var/lib/ms/store.snapshot

# 配置文件启动（含 metrics HTTP 监听）
linqu_mem_service_host serve --config /etc/lingqu/mem_service/mem_service.conf
```

就绪与状态检查：

```bash
linqu_mem_service_host ready  --connect unix:/tmp/ms.sock
linqu_mem_service_host status --connect unix:/tmp/ms.sock
```

`ready` 在 daemon 接受连接前退出非零，可用于启动脚本等待循环。默认端点
（未传 `--connect` 时）为 `unix:/tmp/linqu_mem_service.sock`。

### 3.2 数据面 CLI（节选）

```bash
linqu_mem_service put-object   --connect unix:/tmp/ms.sock --key <k> [--payload-inline ...|--payload-path ...]
linqu_mem_service get-object   --connect unix:/tmp/ms.sock --key <k>
linqu_mem_service register-prefix / lookup-prefix
linqu_mem_service publish-kv / resolve-kv            # resolve 可用 --key 或 --block-hash
linqu_mem_service publish-runtime-handoff / resolve-runtime-handoff
linqu_mem_service register-execution-artifact / query-execution-artifact
linqu_mem_service register-training-artifact / query-training-artifact
```

变更类命令支持幂等键；查询类命令支持 expected session/model/artifact/owner/
version/checksum 绑定，不匹配即 fail-closed。

## 4. Admin CLI 与输出契约

admin 命令（guest/host 二进制相同表面）：

| 命令 | 说明 |
| --- | --- |
| `health` / `ready` / `status` | 活性、就绪、状态摘要（text-kv） |
| `provider-status` | provider 注册与就绪（复用 status operation） |
| `list-records` | record 列表（`record index=.. kind=.. kind_name=.. key=.. version=.. checksum=..`） |
| `metrics` / `metrics-export` | text-kv 指标 / Prometheus 文本格式 |
| `audit-log` | 审计环（`audit_begin`/`audit_end` 分隔，含 sequence、operation、status、idempotency 等字段） |
| `inspect-object` | 单对象检查 |
| `export-snapshot` / `export-snapshot-page` / `export-snapshot-to` | 快照导出（整份/分页/落盘） |
| `restore-snapshot` / `restore-snapshot-page` | 快照恢复（事务化 staged commit） |

输出契约冻结于 `apps/mem_service/admin-output-schema.txt`：所有输出为
`text-kv`；CLI 状态行为 `mem_service <command>: status=<wire_status_name>`；
`status` 含 `ready`、`control_plane_ready`、`provider_registry_ready`、
`durable_ready`、`data_plane_ready` 及各 record 计数字段；fail-closed 状态为
`stale_ref`、`checksum_mismatch`、`version_conflict`、
`invalid_model_binding`、`invalid_session`。采集器与告警应只依赖该 schema。

## 5. 监控：metrics 与 Prometheus 告警

- `metrics_listen=tcp:127.0.0.1:<port>` 开启 HTTP scrape 监听（仅 loopback），
  scrape 路径 `/metrics`，Content-Type `text/plain; version=0.0.4`。
- 指标前缀 `lingqu_mem_service_`，默认 counter，其中
  `request_latency_max_ms` 为 gauge；覆盖请求/错误/not_found/stale_ref/
  checksum_mismatch/version_conflict/idempotency replay/conflict/延迟直方图
  等（完整清单见 `admin-output-schema.txt`）。
- 告警规则：`apps/mem_service/deploy/linqu_mem_service.prometheus-alerts.yml`
  （6 条，contract_version=1）：`LingquMemServiceDown`、
  `LingquMemServiceErrorRate`、`LingquMemServiceFailClosed`、
  `LingquMemServiceChecksumMismatch`、`LingquMemServiceCapacityExceeded`、
  `LingquMemServiceHighLatency`。安装后位于
  `share/lingqu/mem_service/deploy/`，接入 Prometheus rule 配置并用
  `promtool check rules` 校验。
- `alert-integration-fixtures` 只对合成 `/metrics` payload 校验规则与指标名
  匹配；真实 Prometheus/Alertmanager 环境集成在 Linux ops 认证证据通过前
  保持 `not-certified`。

## 6. systemd 部署

安装布局提供两个 unit（同时放入 `usr/lib/systemd/system/` 与
`share/lingqu/mem_service/deploy/`）：

- `linqu_mem_service.service`：
  `ExecStart=/usr/bin/linqu_mem_service serve --config /etc/lingqu/mem_service/mem_service.conf`，
  `RuntimeDirectory=lingqu`、`StateDirectory=lingqu/mem_service`。
- `linqu_mem_service.host.service`：
  `ExecStart=/usr/libexec/lingqu/mem_service/linqu_mem_service_host serve --config /etc/lingqu/mem_service/mem_service.host.conf`，
  `StateDirectory=lingqu/mem_service_host`。

部署流程：

```bash
sudo make -C apps/mem_service install PREFIX=/usr
sudo systemctl daemon-reload
sudo systemctl enable --now linqu_mem_service.service linqu_mem_service.host.service
systemctl status linqu_mem_service.service
curl -s http://127.0.0.1:9900/metrics | head    # 主服务 metrics
curl -s http://127.0.0.1:9901/metrics | head    # host 服务 metrics
```

## 7. 安装、打包与发布认证

### 7.1 安装

```bash
make -C apps/mem_service install PREFIX=/usr/local [DESTDIR=<staging>]
make -C apps/mem_service install-smoke DESTDIR=<dir> PREFIX=/usr
```

布局：`bin/linqu_mem_service`、
`libexec/lingqu/mem_service/linqu_mem_service_host`、
`include/lingqu/mem_service/`（公开头）、`src/lingqu/mem_service/`（源码
SDK：client/wire_client/provider + roce/tcp provider 源）、
`share/lingqu/mem_service/`（manifest、config、deploy、scripts、examples）、
`etc/lingqu/mem_service/`、systemd unit、
`lib/pkgconfig/lingqu-mem-service.pc`。

安装后验证：

```bash
make -C apps/mem_service installed-sdk-example-smoke   DESTDIR=<dir> PREFIX=/usr
make -C apps/mem_service installed-sdk-pkgconfig-smoke DESTDIR=<dir> PREFIX=/usr
make -C apps/mem_service installed-sdk-runtime-smoke   DESTDIR=<dir> PREFIX=/usr
# 无源码检出时，用安装树内脚本：
<prefix>/share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh --no-runtime
<prefix>/share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh [--preflight]
```

`lingqu-mem-service.pc` 变量：`Cflags`、`sdk_sources`（中立 SDK）、
`payload_provider_roce_sources`/`payload_provider_roce_libs=-lrdmacm -libverbs`、
`payload_provider_tcp_sources`/`payload_provider_tcp_libs=-pthread`。

### 7.2 打包

```bash
make -C apps/mem_service package-tarball package-tarball-smoke
make -C apps/mem_service package-deb     package-deb-smoke
make -C apps/mem_service package-rpm     package-rpm-smoke    # 需 rpmbuild/rpm2cpio/cpio
```

产物在 `out/mem_service/`（`PACKAGE_OUT_DIR` 可覆盖）：
`linqu_mem_service-installed-layout-v1.tar`、
`linqu-mem-service_0.1.0-1_arm64.deb`、
`linqu-mem-service-0.1.0-1.aarch64.rpm`（版本/架构由
`MEM_SERVICE_DEB_VERSION` 等变量控制；rpm spec 见
`apps/mem_service/packaging/linqu-mem-service.spec`）。无 rpm 工具链的主机上
rpm 门禁 fail-closed，不会伪造认证。

### 7.3 发布认证证据链（Linux）

```bash
# 预检（不触碰 systemd/rpm）
scripts/run_mem_service_linux_ops_ci.sh --preflight
# 真实部署 + 升级/回滚 + 证据 + bundle（需要 root/systemd/rpm/promtool 与 rollback rpm）
scripts/run_mem_service_linux_ops_ci.sh --rollback-rpm <previous.rpm> [--rpm-file <current.rpm>]

# 跨主机 remote transport 认证（消费者侧）
scripts/run_mem_service_remote_transport_ci.sh --preflight
scripts/run_mem_service_remote_transport_ci.sh --source tcp:<ipv4>:<port> \
    --producer-host <host> --consumer-host <host> --network-partition-marker <path>

# 最终发布认证（两个 bundle 复验 + release-readiness 证据回放）
scripts/verify_mem_service_release_certification.sh \
    --ops-bundle-file <ops-bundle.tar> --remote-transport-bundle-file <rt-bundle.tar>
```

对应 make 入口：`linux-ops-certification-smoke`、
`linux-ops-certification-bundle[-verify]`、
`remote-transport-certification-bundle[-verify]`、
`release-certification-verify`，以及一键 wrapper
`scripts/run_mem_service_release_certification_ci.sh [--preflight [--dry-run]]`。
`linqu_mem_service release-readiness` 是机器可读的总闸门：无外部证据时
`overall_status=not-certified`，仅当
`--ops-evidence-file` 与 `--remote-transport-evidence-file` 均复验通过才输出
`certified`。

## 8. 升级与回滚

- 策略为 `current-version-only`：只接纳当前已知 release generation；未知
  世代拒绝；降级 `not-certified`。完整策略见
  `apps/mem_service/upgrade-rollback-policy.txt`。
- 同版本重启恢复：`store` 快照 + `<store>.journal` 自动回放。
- 同版本数据迁移：`export-snapshot-page` 分批导出 → 新实例
  `restore-snapshot-page` 事务化导入；非法输入（坏 magic、乱序页、计数不
  符、取消提交）fail-closed 且不影响 live state。
- 真实 Linux 包级升级/回滚由
  `linux-ops-upgrade-rollback-smoke OPS_CERTIFICATION_ROLLBACK_RPM=<rpm>`
  执行（装当前 rpm → 重启/scrape 两个 unit → `--oldpackage` 装回滚 rpm →
  再装回当前），产出 marker 后才计入 ops 认证。

## 9. 排错

| 症状 | 排查 |
| --- | --- |
| `ready` 一直失败 | 看 daemon stderr；确认 socket 路径无残留（旧 socket 文件先删除）；`--store` 指向的目录需可写 |
| 客户端 `timeout` | 增大 client 超时/重试配置；确认 daemon 在跑且 socket 路径一致；检查 `max_payload_bytes` 是否小于请求 |
| `checksum_mismatch` / `stale_ref` | fail-closed 已生效：查 `audit-log` 与 `metrics` 中对应计数；检查损坏块是否被 quarantine（`storage_root/quarantine/`），核对生产端 checksum |
| `capacity_exceeded` | 调整 `max_records`/`max_payload_bytes` 或配置 `record_retention` 回收 |
| `unsupported`（ub-ssd-gsva 后端） | 缺少 `/dev/ub_ssd0` 或 ioctl 不支持——该后端只在带模拟 UB SSD 的 guest 环境可用；宿主机上属预期 |
| `invalid_session`（后端数据面） | GSVA buffer 描述符字段不完整，按 wire-schema 中 `backend_buffer_*` 字段补齐 |
| metrics 抓不到 | `metrics_listen` 必须是 `tcp:127.0.0.1:<port>`；确认 scrape 走 `/metrics`；用 `metrics-export` CLI 对比 |
| systemd 启动失败 | `journalctl -u linqu_mem_service.service`；确认 `/etc/lingqu/mem_service/mem_service.conf` 存在且 socket/state 目录由 `RuntimeDirectory`/`StateDirectory` 创建 |
| manifest 校验失败（fixture 报 checksum 不符） | 不要手改 `apps/mem_service/*.txt`；例如用 `make -s --no-print-directory -C apps/mem_service print-wire-schema > apps/mem_service/wire-schema.txt` 由当前二进制重新生成，再 review diff；其它 manifest 使用对应 `print-*` 目标 |
| macOS 上 rpm/ops 认证失败 | 属预期 fail-closed：rpm/systemd/promtool 认证只能在 Linux CI 环境完成 |
