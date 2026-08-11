# ub_sim 使用侧适配手册

本文面向 `ub_sim`（qemu+UB 多节点 PP 模拟器）的维护者：说明 ub_sim 如何以
**源码方式**消费本仓库（`mem_service`），如何构建 guest 二进制并启动多节点
qemu+UB PP 运行。`ds4` 的安装态 SDK 消费方式见
[integration-ds4.md](integration-ds4.md)。

> 路径约定：`<mem_service>` 指本仓库检出根目录，`<ub_sim>` 指 ub_sim 检出
> 根目录。ub_sim 侧的脚本均位于 `<ub_sim>/guest-linux/aarch64/` 下。

## 1. 消费契约：`MEM_SERVICE_ROOT`

`mem_service` 已从 ub_sim 的 `guest-linux/aarch64` 子树抽取为独立仓库，
本仓库是唯一权威来源。ub_sim 不再保存组件副本，而是按如下契约直接编译
本仓库的源码：

- ub_sim 的 make/shell 变量 `MEM_SERVICE_ROOT` 指向本仓库检出，默认值为
  相对 `ub_sim/guest-linux/aarch64` 的兄弟检出 `../../../mem_service`
  （即两个仓库并排放在同一目录下时开箱即用）。
- `ub_sim/guest-linux/aarch64/mem_service.lock` 固定允许消费的
  `VERSION` 与 Git revision；`scripts/verify_mem_service_source.py` 在构建
  initramfs、W5 bootstrap 和 app build matrix 前验证 checkout 完整、revision
  精确匹配且工作树干净。升级流程必须先提交 mem_service，再更新 lock。
- ub_sim 直接编译 `$(MEM_SERVICE_ROOT)/components/mem_service/*.c`，并加
  `-I$(MEM_SERVICE_ROOT)` 以获得 `common/`、`libs/obmm_queue/`、
  `kernel_ub/include/` 等 vendored 头文件。
- 构建 Qwen3 适配器时，ub_sim 调用本仓库的 Makefile 并传入
  `LLM_INFER_ROOT=<ub_sim>/guest-linux/aarch64`（llm_infer 仍属于 ub_sim）。
- guest 二进制 `linqu_mem_service` / `linqu_mem_service_qwen3` 由
  `build_initramfs.sh` 拷入 initramfs 的 `/bin/`，由 guest init 脚本启动。
- OBMM remote mapping 通过中立 `PEER_MAPPING` provider contract 暴露。
  OBMM provider 独立拥有 export/import、mmap、cache maintenance 和 range
  visibility；cluster queue/object 协议保留在其上层。URMA 是独立的显式传输
  provider，不是 OBMM mapping 的实现层，也不得作为隐式回退。

因此：修改本仓库 `components/mem_service/` 下的任何源文件会直接影响
ub_sim 的下一次构建；两个仓库的接口面（公开头文件、wire 协议、CLI 表面）
变更必须按本仓库的兼容策略（`api-abi-policy.txt`、`compat-matrix.txt`）
推进。

## 2. ub_sim 侧消费点

以下 ub_sim 文件引用 `MEM_SERVICE_ROOT` 并编译/打包本仓库源码（均在
`<ub_sim>/guest-linux/aarch64/` 下）：

| 文件 | 作用 |
| --- | --- |
| `apps/llm_infer/Makefile` | 编译 llm_infer guest app 时链接 mem_service 组件源码（cluster/OBMM/GSVA 数据平面、Qwen3 适配面） |
| `apps/serving_control/Makefile` | serving 控制面 app 链接 mem_service client/daemon 源码 |
| `apps/pretraining_client/Makefile` | pretraining 客户端 app 链接 mem_service client 源码（dataset/sample/checkpoint/gradient/optimizer-state/step-commit） |
| `scripts/build_initramfs.sh` | 调用本仓库 `apps/mem_service/Makefile` 构建 `linqu_mem_service`（核心 daemon/CLI）与 `linqu_mem_service_qwen3`（传 `LLM_INFER_ROOT`），拷入 initramfs `/bin/` 并做链接检查 |
| `scripts/run_w5_memory_service_bootstrap.sh` | W5 场景引导脚本：准备 mem_service 运行所需的环境与启动序列 |

## 3. 构建 guest 二进制

在 ub_sim 检出中（推荐，走 `MEM_SERVICE_ROOT` 契约）：

```bash
# 默认 MEM_SERVICE_ROOT=../../../mem_service（相对 guest-linux/aarch64）；
# 非标准布局时显式指定：
cd <ub_sim>/guest-linux/aarch64
make -C <mem_service>/apps/mem_service all LLM_INFER_ROOT="$PWD"
```

也可以直接在本仓库构建（等价产物，需要 `aarch64-linux-gnu-gcc`）：

```bash
make -C <mem_service>/apps/mem_service all                              # linqu_mem_service + linqu_mem_service_core
make -C <mem_service>/apps/mem_service linqu_mem_service_qwen3 \
    LLM_INFER_ROOT=<ub_sim>/guest-linux/aarch64                          # Qwen3 适配器
```

`build_initramfs.sh` 会把 `linqu_mem_service`、`linqu_mem_service_qwen3`
安装进 initramfs `/bin/`；guest 内由 init 脚本按内核命令行参数决定是否
启动 mem service。

## 4. 启动多节点 qemu+UB PP 运行

ub_sim 提供封装好的运行脚本（`<ub_sim>/guest-linux/aarch64/scripts/`）：

- `run_ub_dual_node_mem_service.sh` — 双节点 mem_service 场景（透传到
  `run_ub_dual_node_apps.sh --app mem_service`）。
- `run_ub_eight_node_mem_service.sh` — 八节点 PP 场景。

关键内核命令行参数（由上述脚本自动追加）：

- `linqu_mem_service=1` — guest init 据此启动 mem service 路径；
- `mem_service_region_size_mb=512` — mem_service 使用的 OBMM region 大小。

典型端到端序列（双节点）：

```bash
# 1. 构建 guest 二进制（ub_sim 侧，走 MEM_SERVICE_ROOT）
cd <ub_sim>/guest-linux/aarch64
scripts/run_ub_app_build_matrix.sh --only mem_service

# 2. 构建 initramfs（会拷入 linqu_mem_service / linqu_mem_service_qwen3）
scripts/build_initramfs.sh

# 3. 启动双节点 qemu+UB 运行（自动追加 linqu_mem_service=1
#    与 mem_service_region_size_mb=512）
scripts/run_ub_dual_node_mem_service.sh

# 八节点 PP 同理：
scripts/run_ub_eight_node_mem_service.sh
```

W5 内存服务引导场景：

```bash
scripts/run_w5_memory_service_bootstrap.sh
```

## 5. 验证与排错

- guest 内验证：initramfs 中的 `linqu_mem_service` 提供完整 CLI，可在 guest
  控制台执行 `linqu_mem_service wire-fixtures`、`linqu_mem_service serve ...`
  及各类 fixture 命令；`run_app mem_service`（ub_sim 的 guest app runner）
  会跑 metadata smoke、wire/store/journal/compat/release fixture 门禁。
- 本仓库侧回归：在 `<mem_service>` 根目录执行
  `python3 -m unittest discover tests` 与
  `make -C apps/mem_service host-artifact-smoke`，确保组件改动未破坏契约。
- 构建报 `llm_infer.c not found`：构建 `linqu_mem_service_qwen3` 时缺少
  `LLM_INFER_ROOT`，确认指向 `<ub_sim>/guest-linux/aarch64` 且其中存在
  `components/llm_infer/llm_infer.c`。
- `MEM_SERVICE_ROOT` 解析失败：确认 ub_sim 与本仓库为兄弟检出，或显式传
  `MEM_SERVICE_ROOT=<mem_service 绝对路径>`。
- 组件头文件找不到：确认编译时带有 `-I$(MEM_SERVICE_ROOT)`（vendored 依赖
  `common/obmm_common.h`、`libs/obmm_queue/`、`kernel_ub/include/uapi/ub/`
  均通过该 include 根解析）。
