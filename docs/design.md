# Lingqu Memory Service 架构设计

本文汇总 `mem_service` 的整体架构：daemon/client 模型、wire 协议与版本化、
provider 契约、payload 后端、cluster/OBMM/GSVA 数据平面、模型适配器、
fail-closed 原则与升级/回滚策略。历史背景与早期取舍见同目录的
[lingqu_db_object_service_design.md](lingqu_db_object_service_design.md)、
[mem_service_implementation_summary.md](mem_service_implementation_summary.md)、
[mem_service_independent_deployment_assessment.md](mem_service_independent_deployment_assessment.md)
与
[mem_service_target_status_gap_report.md](mem_service_target_status_gap_report.md)；
本文为准，细节冲突时以代码与 checked-in manifest 为准。

## 1. 总体定位与分层

`mem_service` 是灵瞿数据服务层的内存/对象服务，同时服务于 LLM serving
（prefix、KV、runtime handoff、execution artifact）与 pretraining
（dataset shard、sample batch、checkpoint、gradient bucket、optimizer
state、training-step-commit）数据路径。它不是通用外部数据库：对象语义、
版本、checksum 与放置策略由服务核心定义，payload 字节可由后端或 provider
数据平面承载。

实现严格分层（见 `components/mem_service/README.md` 的 Productization
Split Contract）：

- **Core metadata**：key 构造（`mem_service_keys.c`）、record 表
  （`mem_service_records.c`）、prefix/KV 元数据状态机
  （`mem_service_metadata.c`）、对象引用投影与 checksum
  （`mem_service_object_refs.c`）。不依赖 QEMU、OBMM 设备文件或任何模型拓扑。
- **Service API**：版本化 wire 信封、稳定 operation/status ID、daemon 生命
  周期、轻量 client transport、类型化 C client、payload schema。必须保持
  模型中立，可被外部 serving/pretraining 进程调用。
- **Transport/runtime**：OBMM pool 映射、队列描述符、cluster bootstrap、
  guest handoff 时序，以及 `providers/` 下的 TCP/RoCE 数据平面 provider。
- **Model adapters**：Qwen3 与 DeepSeek-V4-Flash 的 range/KV/engram/放置
  语义。新模型族必须以 adapter 形式加入，不得特化服务核心。
- **Deployment apps**：`apps/mem_service` 下的 CLI/guest/host 入口、配置、
  systemd、打包与发布认证。

## 2. Daemon / Client 架构

```text
serving / pretraining / admin 进程
   │  mem_service_client.h（类型化 API）
   │  mem_service_wire_client.h（Unix-socket transport）
   ▼
unix:<path>  ◄── 版本化 wire 信封 + text-kv payload ──►
linqu_mem_service serve（guest/aarch64 或本机）
linqu_mem_service_host serve（宿主机，libexec 安装）
   │
   ├─ record 表（内存态）＋ store snapshot + append-only journal（持久化）
   ├─ storage_root 持久目录：catalog/manifest.txt、blocks/、quarantine/
   └─ payload 后端与 provider 数据平面
```

- daemon 为单进程 Unix-socket 服务循环（`mem_service_daemon.c`），提供
  对象、prefix、KV、runtime handoff、execution/training artifact RPC 与
  只读 admin RPC。
- `serve --store <path>` 启用重启恢复：`<store>` 为快照，`<store>.journal`
  为 append-only 的幂等/审计日志流；mem_service 不拥有 durable payload 字节。
- `serve --config <file>` 从 text-kv 配置启动，`storage_root` 派生
  `catalog/store.snapshot` 与 sealed block/quarantine 布局。
- 外部客户端只链接 `mem_service_client.c` + `mem_service_wire_client.c`
  （必要时加 provider SDK 源文件），不链接 daemon、record core 或模型适配器。
- client transport 暴露显式超时与可选 max-attempts/backoff 重试；所有变更类
  RPC 接受可选 `idempotency_key`，已完成请求可安全重放（冲突以稳定状态码
  拒绝）。
- 服务边界是本地：`auth_mode=none` 仅允许 `unix:` 监听端点，metrics 仅允许
  loopback TCP 监听。

## 3. Wire 协议与版本化

协议头定义在 `components/mem_service/mem_service_wire.h`：

- 固定 48 字节头部：`magic=0x4d535643`、`version=1`、`header_len=48`、
  operation ID、status ID、payload 长度与 checksum；`wire_version=1`，
  `wire_schema_version=1`，payload 当前为 `text-kv`（typed binary 预留为
  `typed-binary-v1` 方向）。
- 23 个 operation（`wire-schema.txt` 中的 `operation=<name>:<id>`）：admin 面
  `health:1`、`ready:2`、`status:3`、`list_records:4`、`metrics:5`、
  `export_snapshot:6/7(page)`、`restore_snapshot:8/9(page)`、`audit_log:10`；
  数据面 `put_object:16`、`get_object:17`、`inspect_object:18`、
  `register_prefix_entry:32`、`lookup_prefix_entry:33`、
  `publish_kv_segment:48`、`resolve_kv_segment:49`、
  `publish_runtime_handoff:64`、`resolve_runtime_handoff:65`、
  `register_execution_artifact:80`、`query_execution_artifact:81`、
  `register_training_artifact:96`、`query_training_artifact:97`。
- 11 个稳定 status：`ok:0`、`not_found:1`、`stale_ref:2`、
  `checksum_mismatch:3`、`version_conflict:4`、`invalid_model_binding:5`、
  `invalid_session:6`、`timeout:7`、`capacity_exceeded:8`、`unsupported:9`、
  `internal:10`。
- 查询类 RPC（resolve/query）可携带 expected session/model/artifact/owner/
  version/checksum 绑定，不匹配即按上述稳定状态码 fail-closed。

### 契约 manifest（checked-in，机器可 diff）

`apps/mem_service/` 下的 text-kv manifest 由 CLI 子命令生成并冻结，任何协议/
输出/策略变化都必须经 `print-*` make 目标重新生成并过 fixture 门禁：

| manifest | 生成命令 | 冻结内容 |
| --- | --- | --- |
| `wire-schema.txt` | `linqu_mem_service wire-schema` | 23 个 operation、164 个字段、selector（如 `resolve_kv_segment` 的 `key`/`block_hash` 二选一） |
| `admin-output-schema.txt` | `admin-output-schema` | status/list-records/metrics/audit/snapshot/restore 输出契约、Prometheus 前缀 `lingqu_mem_service_` 与类型、fail-closed 状态字段 |
| `api-abi-policy.txt` | `api-abi-policy` | client API/ABI v1、record ABI size=808、wire/header/schema 版本、old/new 与升级回滚策略 |
| `upgrade-rollback-policy.txt` | `upgrade-rollback-policy` | current-version-only 准入、同版本重启/恢复门禁、old-server runtime binary 认证状态 |
| `ops-certification-policy.txt` | `ops-certification-policy` | 生产运维准入边界（systemd/Prometheus/rpm/升级回滚在外部 Linux CI 证据前保持 not-certified） |
| `compat-matrix.txt`、`compat-baseline-v1.txt`、`compat-old-new-matrix.txt` | `compat-matrix` 等 | wire/schema/retry/idempotency/audit/snapshot/journal 兼容规则与 v1 基线 |
| `release-manifest.txt`、`package-manifest.txt` | `release-manifest` / `package-manifest` | 发布与 `installed-layout-v1` 安装布局契约（含各 manifest 的长度与 checksum） |

## 4. Provider 契约与边界

provider 实现 `mem_service_provider.h` 的中立契约（region 注册、本地/对端
传输、durable 存储、accelerator memory、receive fence 等 capability）。完整
边界规则见 `components/mem_service/providers/README.md`，要点：

- provider 不定义对象身份、KV 语义、放置策略、wire operation 或服务就绪；
  它们通过中立契约接收不透明 region/传输请求，可注册 capability 与拓扑
  代价，不得修改对象元数据或选择模型策略。
- core 文件绝不 include provider 头；provider 头可以包含 vendor/平台 API。
- provider 无法证明 region 归属、边界、完成、版本或 checksum 时必须
  fail-closed；probe 可报告设备可用，但数据平面就绪要求完成一次对端传输
  并通过 checksum 校验。
- 构建目标显式 opt-in provider；安装后 SDK 的中立 `sdk_sources` 不含
  provider 依赖，消费方通过 `lingqu-mem-service.pc` 的
  `payload_provider_<name>_sources` / `payload_provider_<name>_libs` 显式
  选择。
- 应用进程只能注册自己拥有或显式映射的内存：模型进程内热路径 buffer 走
  中立 provider SDK；daemon 只做控制平面，不得声称对其它进程堆的零拷贝
  所有权。应用间只交换中立序列化 region 描述符。
- 连接型 provider 使用两阶段 server 生命周期（`listen` 先于 `accept`），
  兼容的 `endpoint_open(..., server=true)` 供独立 canary 使用；验证与 region
  注册只在双方进入连接阶段后发生，禁止用延时掩盖 listen/connect 竞态。
- 数据平面 channel 只有在全部配置的传输注册表就绪后才绑定；健康单边不能
  掩盖缺失的 full-mesh 对端。

现有 provider：

- **TCP**（`mem_service_provider_tcp.c`，CLI `linqu_mem_service_provider_tcp`）：
  持久连接 + `TCP_NODELAY`，发送方在接收方完成拷贝并校验 checksum 后才返回
  完成；支持 receive-fence。是显式选择的 provider，永不做 RoCE 失败时的自动
  回退。canary：`server-canary` / `client-canary`。
- **RoCE**（`mem_service_provider_roce.c`，CLI
  `linqu_mem_service_provider_roce`）：full-mesh RDMA provider，依赖 Linux +
  libibverbs/librdmacm。`mesh-serve --config` 消费严格行式配置
  （`version=1`、`listen=unix:<path>`、可选 `store`/`storage_root`/
  `metrics_listen`、`verify_bytes`/`verify_iterations`/`timeout_ms`、一条或多条
  `endpoint=<server|client>,<local-ip>,<peer-ip>,<port>,<device>`），示例见
  `apps/mem_service/configs/providers/roce/mesh.example.conf`。

## 5. Payload 后端

daemon 侧 payload 后端由 `upgrade-rollback-policy.txt` 冻结
（`payload_block_backend=`）：

- `sealed-local-block-v1`：inline 与 `payload_path` 服务端 payload 写入
  `blocks/<checksum>.block`，读时校验。
- `sealed-chunked-block-v1`：大 payload 写入 chunk 目录，损坏 chunk fail-closed
  隔离。
- `transport-loopback-block-v1`：payload 写入
  `remote-blocks/<checksum>.transport/` 并带 transport manifest，读时经同一
  后端校验，损坏即隔离；用于契约测试。
- `transport-tcp-block-v1`：从 `tcp:<ipv4>:<port>` 拉取 payload 到
  `remote-blocks/<checksum>.tcp/`，按相同 sealed 契约校验与隔离。loopback
  TCP 已认证（`remote_payload_network_transport=tcp-loopback-certified`），
  跨主机生产传输在 `remote-transport-verify --evidence-file` 证据通过前保持
  `not-certified`。
- `ub-ssd-gsva-v1`：与默认 OBMM runtime pool 并列的一等对象后端。metadata-only
  `put-object --backend ub-ssd-gsva-v1` 记录 SSD 设备 CNA、owner node、block
  hi/lo/version、字节范围与 checksum；数据平面 `--backend-write 1` /
  `--backend-read 1` 接受显式 GSVA buffer 描述符并向 `/dev/ub_ssd0`（或
  `--backend-device-path`）提交 `SSD_OP_BLOCK_WRITE/READ`。缺设备或不支持的
  ioctl 以 `unsupported` fail-closed，描述符不完整以 `invalid_session`
  fail-closed。GVA/GSVA 描述 mem_service 如何暴露与搬运调用方 buffer，不归
  任一后端所有。

持久化语义：`store`（快照）+ `<store>.journal`（append-only 幂等/审计流）；
`storage_root` 采用 `storage-root-v1` catalog 布局；restore 采用事务化 staged
commit，坏 magic、乱序 page、record 数不匹配、取消提交全部 fail-closed 且
live state 不变。

## 6. Cluster / OBMM / GSVA 数据平面

guest 侧数据平面围绕模拟 UB 设备的 OBMM pool 与 GSVA 描述符构建：

- `mem_service_cluster_runtime.c`：guest OBMM cluster bootstrap、export/import
  slot 激活、pool 布局；可从当前 OBMM 映射派生后端中立的 GSVA 描述符（GSVA
  不归 OBMM pool 后端所有）。
- `mem_service_cluster_payload.c` / `mem_service_cluster_read.c`：cluster 元数据
  payload 快照/摘要/本地发布与稳定读侧 helper，wire 格式为设备中立的
  `mem_service_cluster_payload_contract.h`。
- `mem_service_cluster_queue.c`：OBMM SPSC 队列屏障、对象描述符 publish/wait、
  pending 描述符匹配。
- `mem_service_cluster_observe.c`：本地+远端 payload 快照的 cluster 元数据
  抓取、观测与就绪汇总。
- `mem_service_obmm_object_flow.c` / `mem_service_obmm_objects.c`：OBMM 对象
  发布、描述符交换、远端 resolve 与 Qwen3 range handoff 校验；对象种类与
  固定 payload 布局见 `mem_service_object_contract.h`。
- `mem_service_gsva_access.h`：后端中立的 GSVA region/buffer 描述符契约，
  `ub-ssd-gsva-v1` 等后端适配器共用。
- 队列原语来自 vendored `libs/obmm_queue/`（SPSC/MPSC/SPMC/MPMC），UB uapi 头
  来自 vendored `kernel_ub/include/uapi/ub/`（`obmm.h`、`gsva.h`）。

## 7. 模型适配器

- **Qwen3**：`mem_service_qwen3*.c` 实现 range/KV/engram 放置、KV span 分配、
  runtime range 输入等待与输出发布、KV-state 发布/解析、terminal token、
  engram 候选/决策流与 record 回收策略；`mem_service_qwen3.c` 是到
  `llm_infer` Qwen3 拓扑 helper 的私有适配层，构建
  `linqu_mem_service_qwen3` 需要 `LLM_INFER_ROOT=<ub_sim>/guest-linux/aarch64`。
  适配器专属保留/回收策略只存在于 adapter 层。
- **DeepSeek-V4-Flash**：`mem_service_deepseek_v4_flash.c` 提供几何/放置
  helper（layer count、hidden/KV 字节数、layer-range-for-node、OBMM
  range-flow 请求构造），配合 `mem_service_profile.c`、
  `mem_service_expert_route_flow.c`、`mem_service_expert_cache.c` 使用；调用方
  自行计算请求并交给基础设施，服务核心不做全局模型选择。

## 8. Fail-closed 原则

贯穿全栈的准入哲学：无法证明即拒绝。

- wire/schema：缺必填字段 fail schema 校验；未知可选字段忽略；ID 在 v1 内
  稳定。
- 数据一致性：stale ref、checksum mismatch、version conflict、model/session
  绑定不符、容量超限全部以稳定状态码拒绝并计数。
- 存储：sealed block 读校验失败即 quarantine；restore 事务化，非法输入不改
  live state。
- provider：归属/边界/完成/版本/checksum 任一不可证即失败。
- 发布/运维：未通过外部证据的维度（真实 systemd、生产 collector/alert、rpm、
  跨主机传输、加密）一律声明 `not-certified`，`release-readiness` 默认
  `overall_status=not-certified`，仅在 ops 与 remote-transport 证据均通过
  复验后输出 `certified`。

## 9. 升级/回滚策略

- 策略为 `current-version-only`：升级与回滚只接纳当前已知 release
  generation，未知世代拒绝；降级（downgrade）`not-certified`。
- 同版本语义已认证：重启恢复走 `store snapshot+journal`；同版本快照迁移走
  `export-snapshot-page` + `restore-snapshot-page`（事务化）。
- store/catalog schema：`legacy-to-v1-reject-future`（旧版迁移到 v1，未来版本
  拒绝）。
- old-server runtime binary 与 new-client-to-old-server 均 `certified`；
  client API v1 内源码兼容，ABI（wire 头 + client record 布局）v1 内稳定。
- 升级/回滚必须过 `upgrade-rollback-policy.txt` 中冻结的 required gate 集合
  （wire/admin/compat/store/journal/catalog/deployment/collector/alert/
  package/release fixtures + host-artifact-smoke + install-smoke）；真实
  Linux 部署升级/回滚由 `linux-ops-upgrade-rollback-smoke`（需要 root +
  systemd + rpm + rollback rpm）产出 marker 后才计入认证。
