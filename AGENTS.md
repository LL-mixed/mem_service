# AGENTS.md — Lingqu Memory Service

本仓库是灵瞿内存/对象服务（`mem_service`）的独立仓库与唯一权威来源。

## 布局

- `components/mem_service/` — 组件源码（core、daemon、client、wire、
  cluster/OBMM/GSVA、Qwen3/DeepSeek-V4-Flash 适配器）+ `providers/`（TCP/RoCE
  provider）+ 组件 README（含分层契约，改动前先读）。
- `apps/mem_service/` — CLI 入口、`Makefile`（所有构建/安装/打包/认证入口）、
  checked-in 契约 manifest（`*.txt`）、`configs/`、`deploy/`、`packaging/`、
  `examples/`。
- `common/`、`libs/obmm_queue/`、`kernel_ub/include/` — vendored 依赖，不独立
  演进。
- `scripts/` — 发布认证/安装校验 shell；`tests/` — Python unittest 套件。
- `docs/` — 设计与手册（中文）。

## 构建与测试

```bash
python3 -m unittest discover tests                          # 测试套件（仓库根目录）
make -C apps/mem_service linqu_mem_service_host host-artifact-smoke   # 宿主机构建 + 全 fixture 门禁
make -C apps/mem_service tcp-provider-smoke                 # TCP provider smoke
make -C apps/mem_service all                                # guest 交叉构建（需 aarch64-linux-gnu-gcc）
make -C apps/mem_service install PREFIX=/usr/local          # 安装
```

RoCE（`roce-provider-smoke`）需 Linux + librdma；rpm/systemd 认证目标在非
Linux 主机上 fail-closed 属预期。产物输出到 `out/mem_service/`。

## 约定

- 语言为 C（`-Wall -Wextra`）；公共 API、wire operation/status ID 在 v1 内
  稳定；分层契约见 `components/mem_service/README.md`（core 不依赖设备/
  模型，新模型走 adapter）。
- **契约 manifest 同步**：`apps/mem_service/*.txt`（wire-schema、
  admin-output-schema、api-abi-policy、upgrade-rollback-policy、compat-*、
  release/package manifest 等）由 CLI 子命令生成。`print-<name>` 目标构建
  当前 host binary，并把重新渲染的内容输出到 stdout；改动协议/输出/策略后，
  必须把对应输出重定向到 manifest、review 并提交 diff，禁止手工编辑。
- **provider 边界**（详见 `components/mem_service/providers/README.md`）：
  core 绝不 include provider 头；provider 不定义对象语义/放置策略；构建
  目标显式 opt-in provider；无法证明归属/边界/完成/版本/checksum 时
  fail-closed；provider 永不自动回退。
- **fail-closed**：任何完整性不可证的路径返回稳定错误码并计数；未认证的
  发布/运维维度保持 `not-certified`，不得口头宣称。
- 下游契约：ub_sim 经 `MEM_SERVICE_ROOT` 按源码消费本仓库；ds4 经
  `MEM_SERVICE_PREFIX` 只消费安装 SDK。改动公开头、wire 协议、安装布局或
  CLI 表面时，先核对 `docs/integration-ub-sim.md` 与
  `docs/integration-ds4.md` 中的契约描述并同步更新。
- 文档语言：用户面向文档用中文，技术术语/代码/命令/文件名保持原文。
- 不要做 git 提交，除非用户明确要求。
