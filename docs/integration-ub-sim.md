# ub_sim 使用侧适配手册

本文面向 `ub_sim`（qemu+UB 多节点 PP 模拟器）的维护者：说明 ub_sim 如何以
**源码方式**消费本仓库（`mem_service`），如何构建 guest 二进制并启动多节点
qemu+UB PP 运行。`ds4` 的安装态 SDK 消费方式见
[integration-ds4.md](integration-ds4.md)。

> 路径约定：`<mem_service>` 指本仓库检出根目录，`<ub_sim>` 指 ub_sim 检出
> 根目录。ub_sim 侧的脚本均位于 `<ub_sim>/guest-linux/aarch64/` 下。

## 1. 消费契约：`MEM_SERVICE_ROOT`

OBMM 平台诊断增加 `mem_service_provider_obmm_endpoint_resources_v1()`，查询当前
endpoint 保留的 export/import、VMA、可访问视图及待清理映射；调用须与该 endpoint
的其他操作串行。object-session 的 `stats` 同时输出 `provider-resources`，与服务
全局计数分开。关闭中的 endpoint 仍可查询，已关闭或不支持的平台返回失败。
这些数字来自成功取得及尚未确认释放的本进程资源，不覆盖其他进程或 kernel 孤儿。

SDK map 返回 `MEM_SERVICE_MAPPING_CLEANUP_REQUIRED` 时须保留输出对象并重试
unmap；输出保留 key/generation/owner/handle，访问地址、长度、权限均为零。
unmap 失败同样保留清理归属并撤销 SDK 访问。`object-session` 报告
`map_cleanup_required`，允许显式 unmap 重试，并继续阻止该 key 的 release。
退出清理仍失败时报告 `cleanup_pending`，不销毁持有未确认资源的 endpoint；
该诊断不证明进程退出后的 kernel 清理或恢复已完成。

OBMM provider 增加 `mem_service_provider_obmm_endpoint_close_checked()`：清理失败
保留 endpoint，上层须保留该对象并处理失败；关闭开始后拒绝新注册/映射。
object-session 将关闭失败传播为非零退出码。旧 void 接口继续存在，失败时报告
`endpoint cleanup_pending`。部分 VMA/import 回滚逐项保留尚未确认的归属；close
错误进入隔离状态，不能通过重试同一 fd 编号解除隔离，须由恢复流程核对。

映射生命周期新增中立 `mem_service_client_mapping_transition()`（wire `0x7d`），
诊断入口为 `mapping-transition --key <key> --session-id <session> --generation <g>
--mapping-id <id> --action <begin|confirm|close|finish|cancel|inspect>
--idempotency-key <operation> --connect <endpoint>`。begin 使用 ID 0，其他操作使用
返回的非零 ID；状态编码 0=closed、1=pending、2=active、3=closing。inspect 读取
当前状态，不使用旧幂等应答；其他操作沿用现有幂等契约。旧 begin/confirm 应答只证明
历史操作成功，调用方还须核对当前事务，不能据此重复安装映射。

此接口记录 provider 操作的生命周期，不执行实际 import/unmap；确认成功只能由
掌握 provider 完成结果的 SDK 路径发出。pending/active/closing 均阻止 holder release，
closing 完成和 pending 取消需要已确认无残留映射。多个 mapping ID 可同时绑定同一
holder，其他 holder 的释放不受影响。begin/confirm 要求 provider 就绪且 home 的
当前 incarnation 等于对象绑定值，幂等重放前也执行此检查；已有事务的
inspect/close/finish/cancel 在 readiness 丢失时仍可执行。

`object-session` 的 map/unmap 使用新增
`mem_service_client_map_managed_allocation()` /
`mem_service_client_unmap_managed_allocation()`，自动编排映射事务与实际 provider。
CLI 每次 map 从系统随机源生成 128-bit operation nonce，熵源失败时拒绝映射。
外部 SDK 调用方负责提供每次 map 唯一的 operation ID，并成对保留 mapping 与
零初始化的 lifecycle；map 只调用一次，失败后重试同一上下文的 managed unmap。
`lifecycle.pending` 包含控制应答不确定的状态，即使 provider handle 已解除，
也须完成 FINISH/CANCEL 确认后才能 release。控制面暂不可达时保留实际 VMA，
清零 SDK base/len/flags；这不证明已撤销 CPU 页权限。
session stats 追加 `import_mappings`，表示已确认的客户端视图数，包含本地 home
视图及远端 import 视图。未接入上报的旧 raw mapping 入口不在该计量内；该数字
无法替代 kernel 实际资源枚举，重启恢复与故障 reconciliation 仍待实现。

`object-session` 分别跟踪每个 key/generation/session 的引用，切换 inspect 对象
不清除其他引用。成功的 acquire/release 在本次进程内按 idempotency_key 去重；
旧操作重放不能重新授予或移除当前映射权限。配置中的 session_id 只能映射自己
持有的对象。正常结束或中途失败都会输出已知未释放的 holder，CLI 不自动 release；
服务端仍保留实际引用，跨进程同名 session 的协调及崩溃恢复需要独立实现。

`object-session` 新增 `publish_data` / `wait_visible` 操作，参数为 `key`、
`offset`、`len` 和 `seed` 或 `expect_checksum`。写入后先发布，再通过控制状态
通知另一客户端；读取前等待指定 checksum 可见。两者使用进程内中立 provider SDK，
不新增 wire opcode。等待预算取 session 的 `request_timeout_ms`。原始 write/read
保留为字节访问与负例诊断，不能单独作为跨节点可见性证据。

OBMM session 必须配置 `provider_node_id`、`provider_node_count`（2 至 8）和
非零 `provider_generation`。同一组各节点同时启动，使用同一 generation 完成
本进程 endpoint 的 peer canary 后绑定 SDK channel；服务目录的 readiness 仍由
基础设施单独验证。这些配置只用于 provider 诊断入口。

`serve-allocations` 的 worker 配置必须包含 `allocation_granularity_bytes`。
当前 2 MiB guest pool 使用 `2097152`；worker 将 backing 和地址保留向上对齐，
对象的 `size_bytes` 保持请求值，SDK 数据操作拒绝访问逻辑大小之外的 padding。
SDK 向 provider 分别传递逻辑视图长度和完整 backing descriptor。严格 OBMM 映射
创建独立的可访问视图与 `PROT_NONE` 保护页 VMA，保持完整 backing 地址保留；
OBMM 禁止 `mprotect` 和 VMA 拆分，此路径不依赖这两种操作。末页内的 padding 仍由 SDK 字节范围检查
约束，CPU 页表无法提供子页隔离。

诊断 session 支持 `op=probe_readonly key=<key>` 和 `op=probe_guard key=<key>`。
前者要求已持有只读映射，验证 CPU 写入触发故障；后者要求 OBMM backing 内至少
存在一个完整 padding 页，验证 CPU 读取该页触发故障。探针在映射所属进程执行，
只接受目标地址的同步 SIGSEGV/SIGBUS，随后恢复信号处理器；OBMM VMA 不继承到
fork 子进程，因此子进程的访问故障不能用于证明原映射的保护。无适用保护页时
返回 UNSUPPORTED，不能计作验证通过。这两个操作仅用于诊断，不属于业务 SDK。
session 的 `provider_import_region_bytes` 同时作为 canary 大小，须满足 pool 粒度
并覆盖对象 backing。此配置显式描述部署 profile，不代表自动探测硬件能力。

诊断操作 `op=probe_conflict key=<key>` 要求当前 session 已 acquire 且持有可读映射。
它保留该映射，通过原 managed mapping SDK 再次请求同一对象、同一 VA 的只读
视图。只有 SDK 明确失败、第二次请求没有未完成清理且原映射首个逻辑页的 checksum
保持不变才返回 OK；原映射仍由后续显式操作解除。失败时两份映射上下文分别保留，
退出先清理探针的请求，再清理原映射。缺少有效 holder/映射时拒绝探针。
GSVA aperture 拒绝普通匿名映射，此探针不尝试在 aperture 中创建匿名页。
真实 OBMM 验收还须核对 provider 的 `obmm-map` mmap 失败记录、errno=EEXIST、
随后完整对象数据校验和资源回收。该 CLI 探针不新增 SDK API 或 wire opcode，
不作为 DS4 serving 的调用入口，也不接受调用方提供的地址或 descriptor。

取消 ALLOCATING 对象会进入 RETIRING，等待 home 清理确认；迟到 publish 只登记
待清理 reservation，不重新开放 acquire。当前 worker 重启遇到已有 state 文件时
拒绝运行，地址不重用，异常资源保留待核对。该限制不能视为恢复与强制撤销已完成。

地址管理接入新增 `mem_service_client_poll_allocation()` 和诊断命令
`poll-allocation --node-id <id> --incarnation <u64> --after-generation <u64>`。
它只返回绑定到该 provider 代际的待办元数据，按 generation 扫描，无任务返回
NOT_FOUND。provider 必须按对象 key/generation 核对已有 reservation，再经已有
publish/reclaim 确认；查询不提供独占领取保证。此新增接口尚不表示 OBMM provider
已自动执行这些任务。服务 wire 版本保持 1，新增 operation 为 `0x7c`；旧服务会
拒绝未知操作，消费者不得静默退回手工 descriptor 发布流程。

`mem_service` 已从 ub_sim 的 `guest-linux/aarch64` 子树抽取为独立仓库，
本仓库是唯一权威来源。ub_sim 不再保存组件副本，而是按如下契约直接编译
本仓库的源码：

- ub_sim 的 make/shell 变量 `MEM_SERVICE_ROOT` 指向本仓库检出，默认值为
  相对 `ub_sim/guest-linux/aarch64` 的 `../../mem_service`，即 ub_sim 根目录的
  Git submodule。独立检出可通过显式 `MEM_SERVICE_ROOT` 指定，仍须满足源码锁。
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
# 默认 MEM_SERVICE_ROOT=../../mem_service（相对 guest-linux/aarch64）；
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

## 6. Simpler/PTO 原地发布契约

W5 可以把已提交 hidden-state ObjectRef 对应的 OBMM payload range 交给
`lingqu_shmem` compute adapter，并形成 `AddressSpace::UB_GM` memref。Simpler
执行 PTO callable 时，输入由 `TLOAD` 读取，输出由 `TSTORE` 写入当前节点的
Memory Service payload arena。guest 不携带 tensor inline payload。

本地输出使用两类地址：

| 地址 | 使用者 | 生命周期 |
| --- | --- | --- |
| OBMM self-import alias | Simpler/PTO `TSTORE` 与 QEMU UB_GM callback | 从 memref acquire 持续到 PTO completion 后 release |
| 原始本地 OBMM arena 指针与 `backing_offset` | Memory Service runtime output publish | payload 对象完成发布并结束其正常对象生命周期 |

`mem_service_obmm_range_flow_request` 的
`publish_payload_in_place=true` 要求调用者同时传入
`publish_payload_offset`。publish flow 会检查：

1. range 已位于当前 payload arena 的已预留区间；
2. `payload_len` 没有越过 arena 或 OBMM slot 边界；
3. `payload` 精确等于本地 OBMM slot 基址加 `publish_payload_offset`。

检查通过后，publish flow 直接以该 offset 建立 runtime output ObjectRef，并
跳过 payload `memcpy`。KV state 可独立选择原地发布：计算前通过
`mem_service_model_kv_state_alloc()` 预留完整 KV payload 所需的最终 block
span（包含 header）；计算成功后设置 `publish_kv_in_place=true` 和
`publish_kv_offset`。发布检查实际指针、block 对齐、整个预留 span 的
arena/slot 边界，并验证 checksum；成功时跳过再次分配与复制，日志包含
`payload_mode=in_place publication_copy_bytes=0`。普通 64-byte 对齐的
临时 buffer 不满足该预留契约。未设置新字段的请求保留原分配/复制行为。

该扩展改变进程内 request struct 大小，源码消费者必须整体重新构建，
不能混用旧对象文件；wire schema 与安装 client SDK 的 ABI 保持原有契约。
ub_sim 接线后还需以两节点实际运行证明最终 KV backing 与 PTO 输出一致。
当前接口面向
同进程、受信的 model runtime；多 dispatch 并发阶段还要增加 allocation token
和 owner 校验，防止一个并发调用发布另一个调用预留的 range。

ub_sim 的目标导向入口为：

```bash
python3 guest-linux/aarch64/scripts/run_w5_lingqu_shmem_pto.py \
  --manifest <host_vector_manifest.json> \
  --node-count 2 \
  --decode-steps 2 \
  --qwen-weights-path <qwen3-0.6b-weights>
```

该入口默认执行两个 decode step。正式通过要求每个下游节点、每个 step 的
全部 4096-byte tile 均出现 2 次 `TLOAD`、1 次 `TSTORE`、1 次 fence，且
`segment_payload_staging_bytes=0`、精确公式校验通过、输出以
`payload_mode=in_place` 发布、无残留 QEMU。
