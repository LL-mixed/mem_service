# ds4 使用侧适配手册

可选 V2 reader SDK 增加独立 reference lifecycle、只读 map/unmap 与返回 pending
事务的 `mem_service_client_reference_map_begin()`。已有 reference result、mapping、
lifecycle 和 client record 布局保持不变；原通用 reference-transition SDK 不接纳
map-begin，避免调用方丢失新事务的清理责任。显式采用新 reader API 前先持有
对应 allocation；失败须保留上下文并重试配套 unmap。DS4 默认路径保持不变。

严格 OBMM provider 的固定地址子区间请求现在按视图首字节核对地址，允许合法
非零 offset，并保留只读权限与视图外整页保护。此实现不改变中立 SDK 结构、
RoCE/TCP 路径或 DS4 默认行为；它不提供 V2 reader 的服务端准入或模型验收结果。

V2 服务端引用目录新增 core 发布/解析状态机，沿用同一 record table 与 managed
allocation。它改变 core 内存结构，未改变安装态 client/provider SDK 的既有
结构与默认调用路径；控制协议与 V2 reader 使用独立的 opt-in wire/SDK 接口。
DS4 继续使用当前安装 SDK，不能把 core metadata 测试计作推理或数据面认证。

安装 SDK 的既有 `lingqu_object_service.h` 增加 header-only V2 引用编解码，
V1 布局和版本常量不变，不新增链接依赖。DS4 当前路径保持 V1，须拒绝未知版本。
V2 携带 allocation key/generation/home/incarnation/range/access 元数据；它不
授予访问权限，不验证 payload 内容，不提供服务端绑定或重启恢复保证。显式采用
须等待相应服务/SDK 流程接通，不能把 V1 arena offset 当作 V2 allocation offset。

新增的中立 mapping owner 模块供受管理映射消费者显式采用，安装态通过
`mapping_owner_sources` 与 `mapping_owner_libs` 查询，后者为 `-pthread`。
原 `sdk_sources`、已有 public record/wire 布局和 DS4 transfer 热路径保持不变。
采用时须由单一 access domain 串行化同一 provider 的所有运行期访问，并将原
mapping/lifecycle/holder 转交 owner；接管后的 raw 结构不可继续使用。close 与
最终 destroy 分离，后者要求调用线程已退出。该扩展仍在 AM2 实施阶段。

OBMM mapping pin 接口只供显式 opt-in 的平台 compute adapter 保留现有严格
import 视图的归属。DS4 serving 无需调用，现有 RoCE/TCP 热路径、安装 SDK 的
中立 binding 和 wire 布局均不变；该接口不提供跨进程或并发 holder 管理。

OBMM 平台诊断新增 `mem_service_provider_obmm_endpoint_probe_descriptor()` 和
object-session `probe_descriptor` 操作，仅检查当前持有映射的畸形 descriptor
拒绝及资源保留。DS4 serving 无需调用；RoCE/TCP 数据路径、SDK/wire 布局不变。

daemon 幂等接纳新增受管理资源清理预留：普通请求不能耗尽已有对象、holder 和
mapping 后续清理所需的应答位置，容量不足返回 `CAPACITY_EXCEEDED`。
`allocation-stats` 增加幂等容量/已用/清理预留/可用/缺口文本字段；安装 SDK
的已有结构和 DS4 transfer 热路径保持不变。此保护不实现幂等历史的无限回收。

OBMM 平台诊断新增 `mem_service_provider_obmm_endpoint_probe_conflict()`，用于
当前映射的固定地址冲突验收。它复用已持有的设备句柄，不改变 import、路由或
业务映射 API；DS4 serving 无需调用，RoCE/TCP 的数据路径和配置保持不变。

OBMM 平台新增独立资源快照 API `mem_service_provider_obmm_endpoint_resources_v1()`，
供 endpoint 运维诊断使用；其进程内资源计数不改变服务全局统计或 DS4 transfer
热路径，既有 provider endpoint/binding 布局保持不变。

映射 SDK 新增失败结果 `MEM_SERVICE_MAPPING_CLEANUP_REQUIRED`，调用方必须保留
返回的对象映射并重试 unmap；此时输出仅用于清理，base/len/flags 均为零。
失败的 unmap 同样撤销 SDK 访问并保留 handle。完成清理前不得释放 holder。
此规则适用于映射消费者，不改变现有 transfer provider 的 DS4 数据路径。

显式采用 OBMM provider 的平台集成可使用新增的
`mem_service_provider_obmm_endpoint_close_checked()` 检查关闭结果；失败必须保留
endpoint，旧 void 关闭入口继续兼容并报告未完成清理。DS4 serving 热路径无需
调用该平台接口；现有 RoCE/TCP provider 配置保持不变。

安装 SDK 新增 `mem_service_client_mapping_transition()`，仅供受管理 CPU/memref
映射的生命周期上报；不移动 payload，不修改 DS4 现有 transfer provider 的使用方式。
新调用需要支持 wire `0x7d` 的服务，旧端明确返回 unsupported；不得退回无映射引用
保护的访问路径。现有 client record 与 wire header 布局保持不变。

受管理 map 在 BEGIN 前新增只读服务端绑定核对，拒绝陈旧或不匹配的 descriptor、
home/incarnation、地址和逻辑范围，返回 STALE_REF 且不创建 pending mapping。
BEGIN 再检查 generation、ACTIVE 和 holder；同代已发布绑定不可改写。
每次建映射增加一次元数据查询，正常数据访问不增加 RPC。此改变仅影响采用受管理
CPU mapping 的消费者，不修改 DS4 当前 transfer 热路径、wire opcode 或公开结构布局。

受管理映射消费者可调用 `mem_service_client_map_managed_allocation()` 和
`mem_service_client_unmap_managed_allocation()` 自动编排事务与 provider；原始
map/unmap API 保留。新接口增加独立的 lifecycle 结构，不改变已有 mapping 布局。
每次 map 使用唯一 operation ID，成对保留 mapping/lifecycle，失败后仅重试
managed unmap。`pending=false` 才允许释放 holder；不确定的应答、失败的 provider
清理均保留上下文。该扩展不改动 DS4 当前 transfer 热路径，也不提供重启恢复。

地址管理 SDK 新增 `mem_service_client_poll_allocation()`，供常驻 home provider
查询绑定到自身 incarnation 的待分配/待回收对象。DS4 模型客户端不调用该接口，
也不负责 backing 发布或地址 bootstrap。该接口不改变现有推理 API 或传输选择，
详细扫描及重试语义见组件 README 的 Provider allocation work polling 小节。

本文面向 `ds4`（3 节点 PP 推理，C/CUDA）的维护者：说明 ds4 如何消费
`mem_service` **安装后的 SDK**，并明确区分进程内 activation payload provider
与独立 prefix/KV/object daemon。ub_sim 的源码消费方式见
[integration-ub-sim.md](integration-ub-sim.md)。

> 路径约定：`<mem_service>` 指本仓库检出根目录，`<ds4>` 指 ds4 检出根目录，
> `<prefix>` 指 mem_service 的安装前缀。

## 1. 消费契约：只依赖安装布局

进程内 model range flow 的 `publish_kv_in_place` / `publish_kv_offset`
扩展用于源码集成的计算输出发布。DS4 当前消费的 client/provider 安装 SDK
不使用该 request struct；本次扩展不改变其 wire schema、SDK API 或 provider
配置格式。若后续采用进程内 range flow，需另行约定源码/ABI 与预留区间契约。

ds4 不编译本仓库源码树，只消费 `make install` 产出的安装布局
（`installed-layout-v1`，受 `package-manifest.txt` 契约约束）：

- 头文件：`<prefix>/include/lingqu/mem_service/`（`mem_service_provider.h`、
  `mem_service_provider_roce.h`、`mem_service_provider_tcp.h` 等）；
- 源码 SDK：`<prefix>/src/lingqu/mem_service/` 下的
  `mem_service_client.c`、`mem_service_wire_client.c`、
  `mem_service_provider.c`、`mem_service_provider_roce.c`、
  `mem_service_provider_tcp.c` —— ds4 把 client/wire client 与三个 provider
  源文件直接编译进自己的二进制；
- 链接：`-lrdmacm -libverbs`（RoCE provider 依赖，Linux）。

等价的机器可读入口是安装布局中的 `lib/pkgconfig/lingqu-mem-service.pc`：
`Cflags`、`payload_provider_roce_sources`/`payload_provider_roce_libs`、
`payload_provider_tcp_sources`/`payload_provider_tcp_libs` 变量与上述路径
一一对应。

## 2. 安装 SDK

```bash
make -C <mem_service>/apps/mem_service install PREFIX=/opt/lingqu
# 可选：安装后立即验证布局与 SDK 可编译/可运行
make -C <mem_service>/apps/mem_service installed-sdk-runtime-smoke PREFIX=/opt/lingqu
```

## 3. 构建 ds4（mem-service payload provider）

```bash
cd <ds4>
make mem-service-sdk-check MEM_SERVICE_PREFIX=/opt/lingqu
make cuda-spark-mem-service MEM_SERVICE_PREFIX=/opt/lingqu
```

`ds4` 当前要求安装 SDK 的 `Version` 精确为 `0.1.0`，并在 CUDA 编译前检查
三个 provider public headers、通用/TCP/RoCE provider sources 与
`lingqu-mem-service.pc`。版本不兼容或安装布局不完整时会在昂贵的 CUDA 编译
前直接失败；升级 SDK 时应先在两个仓库中同步调整兼容门禁。

该目标做的事（见 `<ds4>/Makefile`）：

- 以 `-DDS4_MEM_SERVICE_PAYLOAD -I$(MEM_SERVICE_PREFIX)/include/lingqu/mem_service`
  重新构建 `ds4`、`ds4-server`、`ds4-bench`、`ds4-eval`、`ds4-agent`；
- 把 `$(MEM_SERVICE_PREFIX)/src/lingqu/mem_service/` 下的三个 provider 源文件
  编成对象文件并链接 `-lrdmacm -libverbs`；
- 传输适配层为 `<ds4>/ds4_payload_transport_mem_service.c`（ds4 侧，本仓库
  不维护）。

## 4. provider 配置文件

运行时通过 `--dist-payload-config <file>` 指定严格 text-kv 配置文件。解析
规则由 ds4 的 `ds4_payload_transport_mem_service.c` 定义（未知字段、重复
`provider=`、畸形/不完整字段一律拒绝并报错）。字段：

| 键 | 取值 | 说明 |
| --- | --- | --- |
| `mem_service_payload_config_version` | `1`（必填，唯一） | 配置格式版本，其它值报错 |
| `provider` | `roce` 或 `tcp`（必填，唯一） | 显式选择数据平面 provider；不存在自动回退 |
| `timeout_ms` | 1..3600000，默认 10000 | 传输超时 |
| `slot_bytes` | 4096..512MiB，默认 128MiB | 单个 staging slot 大小 |
| `slot_count` | 1..16，默认 8 | staging slot 数；`slot_bytes*slot_count` 不得超过 2 GiB |
| `link` | `<peer_host>,<local_ipv4>,<peer_ipv4>,<port>,<device>`（可多条） | 按 peer 主机名匹配的链路描述；`device` 对 TCP 填 `-` |

`link` 匹配语义：ds4 用对端（相邻 PP stage）的主机名在配置中查找
`link` 首字段一致的条目，取其中的本端 IPv4、对端 IPv4、端口与设备；必须
恰好匹配一条，否则报 "no matching peer link"。配置中以 `#` 开头的行与空行
被忽略。

示例（node0 用，peer 为 node1；RoCE）：

```text
# mem-service payload provider config for node0
mem_service_payload_config_version=1
provider=roce
timeout_ms=10000
slot_bytes=134217728
slot_count=8
link=node1,192.168.1.10,192.168.1.11,19110,rocep1s0f0
```

TCP 等价配置把 `provider=tcp`，`link` 的 `device` 字段填 `-`：

```text
link=node1,192.168.1.10,192.168.1.11,19110,-
```

## 5. 启动 3 节点 PP 运行

拓扑：node0 承载首层（第一层段）、node1 承载中间层段、node2 承载末层段；
相邻 stage 之间通过 mem_service provider 数据平面传输 activation payload。
每个节点都加：

```bash
--dist-payload-provider mem-service --dist-payload-config <本节点的配置文件>
```

两个参数必须成对出现（只给一个会被 ds4 拒绝）。每节点使用各自的配置
文件（或共享一份包含全部 `link` 条目的配置，ds4 按 peer 主机名自行
匹配）。ds4 分布式 PP 的相关参数为 `--role coordinator|worker`、
`--layers A:B`（含边界的层切片，`output` 表示到输出头）、
`--listen HOST PORT` 与 `--coordinator HOST PORT`；node0 同时承担
coordinator。示例（层边界按实际模型调整）：

```bash
# node0（首层段 + coordinator），peer = node1
./ds4 --model <model.gguf> --role coordinator --layers 0:19 \
    --listen 192.168.1.10 7100 \
    --dist-payload-provider mem-service --dist-payload-config /etc/ds4/mem-service-node0.conf

# node1（中间层段），peer 为 node0/node2（按 stage 连接方向匹配 link）
./ds4 --model <model.gguf> --role worker --layers 20:39 \
    --listen 192.168.1.11 7101 --coordinator 192.168.1.10 7100 \
    --dist-payload-provider mem-service --dist-payload-config /etc/ds4/mem-service-node1.conf

# node2（末层段），peer = node1
./ds4 --model <model.gguf> --role worker --layers 40:output \
    --listen 192.168.1.12 7102 --coordinator 192.168.1.10 7100 \
    --dist-payload-provider mem-service --dist-payload-config /etc/ds4/mem-service-node2.conf
```

启动顺序与时序由 provider 契约保证安全：server 侧先 `listen` 使端点可达，
应用控制平面宣布就绪后才 `accept` 完成连接；数据平面在完成一次带
checksum 的对端传输验证（`data_plane_ready`）之前不会标记就绪，连接/验证
失败则激活失败，不存在靠延时掩盖的 listen/connect 竞态。发送方在接收方
完整拷贝并校验 checksum 后才返回完成。

## 6. RoCE 与 TCP 的选择

| | RoCE | TCP |
| --- | --- | --- |
| 依赖 | Linux + libibverbs/librdmacm + RDMA 网卡 | 仅 TCP/IP，跨平台 |
| 适用 | DGX 类 RDMA 互联的 PP 全互联部署 | 开发联调、无 RDMA 的环境 |
| 配置 | `provider=roce`，`link` 带真实 device（如 `rocep1s0f0`） | `provider=tcp`，`link` 的 device 填 `-` |
| 链接库 | `-lrdmacm -libverbs`（ds4 目标已带） | `-pthread`（由 provider 源自行处理） |

provider 是显式选择，TCP 永不是 RoCE 失败时的自动回退——RoCE 建链失败
会直接报错，便于暴露环境问题而不是静默降级。

## 7. 分布式 prefix/KV checkpoint

完整部署包含两个互不替代的角色：

- 三个 DS4 模型进程内各自编译 mem_service payload provider，只搬运逐 token
  activation，不管理 prefix/KV/object；
- coordinator 所在机器另行运行一个独立、受监督的 mem_service daemon，使用
  Unix socket 服务 DS4 coordinator，并把 metadata state 与 payload blocks
  放在持久化目录。它是唯一 prefix/KV/object 权威。

daemon 的参考配置 `/etc/lingqu/mem_service/ds4-kv.conf`：

```text
listen=unix:/run/lingqu/ds4-kv.sock
store=/var/lib/lingqu/ds4-kv/store.snapshot
storage_root=/var/lib/lingqu/ds4-kv
node_id=dgx1-ds4-kv
cluster_id=dgx-spark-pp
backend=snapshot+journal
max_records=1024
max_payload_bytes=4096
retention=manual
checkpoint_retention=manual
record_retention=latest:900
encryption=none
auth_mode=none
metrics_mode=text-kv
metrics_listen=tcp:127.0.0.1:9902
adapter_enablement=core
```

```bash
linqu_mem_service serve \
    --config /etc/lingqu/mem_service/ds4-kv.conf
```

实际参数以 `linqu_mem_service help` 输出为准；使用 runtime config 时应给出等价
的 socket、state 与 storage root。先用 `ready --connect` 通过 readiness，再
启动 DS4 coordinator：

systemd 部署应把带重试的 `ready --connect` 放在 daemon unit 的
`ExecStartPost`；这样 coordinator 的 `After=lingqu-ds4-kv.service` 等到 Unix
socket 与持久 catalog 真正可用，而不是只等 daemon 进程被 fork 出来。

```bash
linqu_mem_service ready --connect unix:/run/lingqu/ds4-kv.sock
./ds4-server ... \
    --kv-disk-dir /var/lib/ds4/prefix-index \
    --kv-disk-space-mb 8192 \
    --kv-mem-service unix:/run/lingqu/ds4-kv.sock
```

只有 coordinator 连接 daemon。三个 worker 的 KV shard 仍通过 DS4 已有的
checkpoint 协议汇聚到 coordinator。启动三个互不复制的 daemon 会制造三个
权威，无法提供 manifest 原子可见性，因此不是受支持的 3 节点部署形态。

DS4 coordinator 负责 checkpoint transaction：

1. 为一次 checkpoint 分配不可复用的 `generation`；
2. 从三个 PP stage 收集 layer shard，并验证 layer range 连续、无重叠、
   完整覆盖模型全部 layer；
3. 以 generation-scoped object key 把三个 shard 写入 mem_service object
   API，并逐个验证 version/checksum/bytes；
4. 对每个 shard 调用 `publish_kv_segment`，再用 `resolve_kv_segment` 回读，
   验证 generation、stage placement、layer range、state 与 object key；
5. 只有三个 KV record 都通过时，才写入并校验 manifest object；
6. 最后调用 `register_prefix_entry`，将 prefix SHA 与模型/量化 namespace
   原子指向 manifest，并立即 `lookup_prefix_entry` 回读验证；
7. prefix publication 的 result segment 就是 generation。daemon 必须拒绝
   旧 generation 覆盖新 generation，以及同 generation 改指另一 manifest；
8. DS4 仅在上述步骤全部成功后写本地 KVC discovery entry。未完成
   generation 永远不会通过公开 prefix namespace 可见。

恢复时，本地 KVC 只负责找到最长 byte-prefix 候选。DS4 必须先通过
`lookup_prefix_entry` 确认 daemon 当前权威记录仍指向该 local ref 的 manifest；
然后解析 manifest，对三个 shard 分别调用 `resolve_kv_segment` 并校验元数据，
再按 version/checksum materialize 对象。任何 prefix 不可见、manifest 不匹配、
KV metadata 缺失、跨 generation、layer coverage 不完整或 checksum 不一致都使
整次恢复 fail-closed；DS4 必须丢弃已经装入的部分 shard，不得继续使用混合
generation 的 KV。

大 payload 不通过 4 KiB text-kv wire 内联返回。SDK 的
`mem_service_client_materialize_object()` 让 daemon 将已校验对象原子写入一个
不存在的 caller 路径；目标已存在时拒绝覆盖。该接口只改变对象内容的交付
方式，不把模型、PP 拓扑或 RoCE 语义带入 mem_service。

mem_service 的 record retention 与 orphan payload GC 负责回收不再被保留
record 引用的对象块；DS4 本地 prefix index 的淘汰不直接删除 daemon 数据，
避免本地 cache policy 越权破坏仍可见的 generation。

## 8. 排错

### 可选 V2 引用控制接口

安装 SDK 新增 `mem_service_reference_protocol.h` 和
`mem_service_client_reference_transition()`，对应 CLI `reference-transition`
的 begin/stage/seal/resolve/acquire。现有 client record 的 808-byte ABI 与
V1 ObjectRef 的 64-byte 编码保持不变；使用新接口需要具有 `0x7e` operation
的服务端，旧服务端返回 unsupported，SDK 不回退到 V1。

新接口只传受管理引用 metadata。调用方必须零初始化 request，明确指定
allocation generation、内容 version、home incarnation 与完整 V2 引用；
失败时 result 输出保持原值，调用者必须先检查返回值，不能继续使用旧 result。
`acquire` 成功后仍需 `release-object` 归还 holder。

DS4 当前推理/KV 路径不因此自动切换到 V2。控制回执不代表 payload 已完成
发布或可读；reader 的真实 provider 数据链路、模型集成和重启恢复仍是独立验收项。
安装边界可使用 `make -C apps/mem_service installed-reference-protocol-smoke`
验证，该目标编译安装目录中的公开头和 client/provider 源码。

| 症状 | 排查 |
| --- | --- |
| `cannot open payload provider config` | `--dist-payload-config` 路径错误或不可读 |
| `invalid payload provider config version` | 首行必须是 `mem_service_payload_config_version=1` |
| `payload provider config has no matching peer link` | 配置中没有首字段等于对端主机名的 `link` 条目；核对 peer 主机名与 `link` 首字段 |
| `unknown payload provider config field` | 配置里有拼写错误或多余字段；只允许第 4 节列出的键 |
| `payload provider staging memory exceeds 2 GiB` | 降低 `slot_bytes` 或 `slot_count` |
| `mem_service payload connection failed` | 对端未启动/未 listen、IP/端口不通、RoCE device 名错误；确认两端 `link` 的 local/peer IP 互为镜像 |
| `mem_service payload listener preparation failed` | server 侧端口被占用或 RDMA device 不可用；换端口，或先用本仓库构建的 canary（`make -C apps/mem_service tcp-provider-smoke` 产出的 `linqu_mem_service_provider_tcp server-canary`/`client-canary`）验证链路 |
| 链接报 undefined `rdma_*`/`ibv_*` | 构建主机缺 librdma；RoCE 仅支持 Linux，安装 libibverbs/librdmacm 开发包，或改用 `provider=tcp` |
| 头文件/源文件找不到 | `MEM_SERVICE_PREFIX` 指错；确认 `<prefix>/include/lingqu/mem_service` 与 `<prefix>/src/lingqu/mem_service` 存在（即先执行过 `make install`） |
