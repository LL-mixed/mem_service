# Lingqu Memory Service（lingqu mem service）

`mem_service` 是灵瞿（Lingqu）内存/对象服务的独立仓库。它提供一个模型中立的
C 语言内存/对象元数据服务：带版本化 wire 协议的 Unix-socket daemon、类型化
C client SDK、可插拔 transport provider（TCP、RoCE/RDMA）、cluster/OBMM/GSVA
数据平面，以及面向 LLM serving 与 pretraining 的模型适配器（Qwen3、
DeepSeek-V4-Flash）。

本仓库从 `ub_sim` 仓库中的 `guest-linux/aarch64` 子树抽取而来，是 `mem_service`
的唯一权威来源；`ub_sim`（qemu+UB 多节点 PP 模拟器）与 `ds4`（3 节点 PP 推理）
作为下游消费方使用本仓库，集成方式见下文"下游集成"。

## 特性

- **独立 daemon / client 架构**：`linqu_mem_service serve` 通过 Unix socket
  对外提供对象、prefix、KV、runtime handoff、execution/training artifact 等
  RPC；`linqu_mem_service_host` 为宿主机侧同构 daemon。
- **版本化 wire 协议**：48 字节定长头部（magic + version + operation/status
  ID + payload checksum），`wire_version=1`、`wire_schema_version=1`，payload
  当前为 `text-kv` 格式，全部操作/字段表面冻结在
  `apps/mem_service/wire-schema.txt` 等 checked-in manifest 中。
- **类型化 C client SDK**：`mem_service_client.h` + `mem_service_wire_client.h`
  提供 serving/pretraining 类型化 API（含 pretraining 的 dataset/sample/
  checkpoint/gradient/optimizer-state 与 training-step-commit）；SDK 以
  "源码头文件 + 源文件" 形式随安装布局发布，外部客户端无需链接 daemon 私有实现。
- **Transport provider**：`mem_service_provider_*` 中立契约 + TCP 数据平面
  provider（跨平台）与 RoCE full-mesh provider（需要 Linux + libibverbs/
  librdmacm）；provider 永不作为其它 provider 失败时的自动回退。
- **payload 后端**：`sealed-local-block-v1`、`sealed-chunked-block-v1`、
  `transport-loopback-block-v1`、`transport-tcp-block-v1`、`ub-ssd-gsva-v1`，
  校验失败一律 fail-closed 并隔离（quarantine）。
- **可运维性**：`status`/`metrics`/`audit-log`/snapshot 导出恢复、Prometheus
  文本指标与告警规则、systemd unit、升级/回滚策略、release certification
  证据链，未认证项一律 `not-certified` fail-closed。

## 仓库布局

| 路径 | 内容 |
| --- | --- |
| `components/mem_service/` | 组件源码（core、daemon、client、wire、cluster/OBMM/GSVA、模型适配器）+ `providers/`（TCP/RoCE provider）+ 组件 README |
| `apps/mem_service/` | CLI 入口 `mem_service.c`、`Makefile`（构建/安装/打包/发布认证）、契约 manifest（`wire-schema.txt`、`release-manifest.txt` 等）、`configs/`（配置 schema 与示例）、`deploy/`（systemd unit 与 Prometheus 告警）、`packaging/`（rpm spec）、`examples/`（可安装的 SDK smoke 客户端） |
| `common/obmm_common.h`、`libs/obmm_queue/`、`kernel_ub/include/uapi/ub/` | vendored 依赖（OBMM 队列与 UB uapi 头文件），与组件一起被 `-I` 引用；来源 revision 与校验和见 `VENDORED.md` |
| `scripts/` | 发布认证/安装校验脚本（`verify_mem_service_*.sh`、`run_mem_service_*_ci.sh`） |
| `tests/` | Python unittest 测试套件 + TCP provider conformance 测试 |
| `docs/` | 设计文档、使用手册、下游集成手册与历史评估报告 |

## 快速开始

前置条件：macOS/Linux 主机（`cc`、`make`、`python3`）；guest 交叉构建需要
`aarch64-linux-gnu-gcc`。

```bash
# 1. 构建宿主机 daemon 并运行自检与全部 fixture 门禁
make -C apps/mem_service linqu_mem_service_host host-artifact-smoke

# 2. TCP provider smoke（RoCE smoke 需要 Linux + librdma）
make -C apps/mem_service tcp-provider-smoke

# 3. 运行 Python 测试套件（仓库根目录）
python3 -m unittest discover tests

# 4. 手动启动一个 daemon 并查询状态
./apps/mem_service/linqu_mem_service_host serve \
    --listen unix:/tmp/linqu_mem_service.sock --store /tmp/ms.store &
./apps/mem_service/linqu_mem_service_host ready  --connect unix:/tmp/linqu_mem_service.sock
./apps/mem_service/linqu_mem_service_host status --connect unix:/tmp/linqu_mem_service.sock
```

guest 侧（aarch64）交叉构建：

```bash
make -C apps/mem_service all        # linqu_mem_service + linqu_mem_service_core
# Qwen3 适配器需要 ub_sim 的 llm_infer：
make -C apps/mem_service linqu_mem_service_qwen3 \
    LLM_INFER_ROOT=<ub_sim 检出>/guest-linux/aarch64
```

## 安装

```bash
make -C apps/mem_service install PREFIX=/usr/local
```

安装布局（`installed-layout-v1`，受 `package-manifest.txt` 契约约束）：

- `bin/linqu_mem_service`（核心 daemon/CLI）
- `libexec/lingqu/mem_service/linqu_mem_service_host`（宿主机 daemon）
- `include/lingqu/mem_service/`（公开头文件）
- `src/lingqu/mem_service/`（源码 SDK：`mem_service_client.c`、
  `mem_service_wire_client.c`、`mem_service_provider.c`、
  `mem_service_provider_roce.c`、`mem_service_provider_tcp.c`）
- `share/lingqu/mem_service/`（manifest、configs、deploy、scripts、examples）
- `etc/lingqu/mem_service/`（默认 runtime 配置）、systemd unit、
  `lib/pkgconfig/lingqu-mem-service.pc`

安装后可运行 `install-smoke`、`installed-sdk-example-smoke`、
`installed-sdk-pkgconfig-smoke`、`installed-sdk-runtime-smoke` 验证布局与 SDK
可用性，或使用安装树内的 `share/lingqu/mem_service/scripts/` 脚本做无源码复验。

## 打包与发布

```bash
make -C apps/mem_service package-tarball package-tarball-smoke   # tar 包
make -C apps/mem_service package-deb package-deb-smoke           # deb 包
make -C apps/mem_service package-rpm package-rpm-smoke           # rpm 包（需 Linux rpm 工具链）
```

产物输出到 `out/mem_service/`（`PACKAGE_OUT_DIR`）。发布认证证据链（需要
Linux + systemd + rpm + promtool 环境）：

```bash
make -C apps/mem_service linux-ops-certification-bundle          # Linux 运维认证 bundle
make -C apps/mem_service remote-transport-certification-bundle   # 跨主机传输认证 bundle
make -C apps/mem_service release-certification-verify            # 最终发布认证复验
```

详细的构建/配置/部署/排错说明见 [docs/user-manual.md](docs/user-manual.md)。

## 文档

- [docs/design.md](docs/design.md) — 架构设计（daemon/client、wire 协议、
  provider 契约、payload 后端、cluster/OBMM/GSVA 数据平面、模型适配器、
  fail-closed 原则、升级/回滚策略）
- [docs/user-manual.md](docs/user-manual.md) — 使用手册（构建、配置、运行、
  systemd、监控、打包发布、升级回滚、排错）
- [docs/integration-ub-sim.md](docs/integration-ub-sim.md) — ub_sim 使用侧
  适配手册（`MEM_SERVICE_ROOT` 源码消费契约、guest 构建与多节点运行）
- [docs/integration-ds4.md](docs/integration-ds4.md) — ds4 使用侧适配手册
  （安装 SDK、`cuda-spark-mem-service`、provider 配置、3 节点 PP 启动）
- 历史文档：`docs/lingqu_db_object_service_design.md`、
  `docs/mem_service_implementation_summary.md`、
  `docs/mem_service_independent_deployment_assessment.md`、
  `docs/mem_service_target_status_gap_report.md`
- 组件细节：`components/mem_service/README.md`、
  `components/mem_service/providers/README.md`

## 下游集成

- **ub_sim**：以源码方式消费本仓库，通过 `MEM_SERVICE_ROOT`（默认为
  ub_sim 的兄弟检出 `../../../mem_service`）直接编译
  `components/mem_service/*.c`。见
  [docs/integration-ub-sim.md](docs/integration-ub-sim.md)。
- **ds4**：只消费安装后的 SDK（`MEM_SERVICE_PREFIX`），编译安装树中的
  三个 provider 源文件并链接 `-lrdmacm -libverbs`。见
  [docs/integration-ds4.md](docs/integration-ds4.md)。

## 开发约定

- 语言为 C（少量 Python 测试与 shell 脚本）；公共 API 保持稳定，wire 协议
  operation/status ID 在 v1 内稳定。
- text-kv 契约 manifest（`apps/mem_service/*.txt`）必须由 CLI 的
  `print-*` make 目标重新生成并与二进制保持同步，禁止手工编辑。
- 任何无法证明完整性（region 归属、边界、完成、版本、checksum）的路径必须
  fail-closed。
- 详见根目录 [AGENTS.md](AGENTS.md)。

## 许可证

本仓库随灵瞿项目整体发布，许可证信息以项目顶层约定为准。
