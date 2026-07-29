# ds4 使用侧适配手册

本文面向 `ds4`（3 节点 PP 推理，C/CUDA）的维护者：说明 ds4 如何消费
`mem_service` **安装后的 SDK** 作为 activation payload 传输层。ub_sim 的源码
消费方式见 [integration-ub-sim.md](integration-ub-sim.md)。

> 路径约定：`<mem_service>` 指本仓库检出根目录，`<ds4>` 指 ds4 检出根目录，
> `<prefix>` 指 mem_service 的安装前缀。

## 1. 消费契约：只依赖安装布局

ds4 不编译本仓库源码树，只消费 `make install` 产出的安装布局
（`installed-layout-v1`，受 `package-manifest.txt` 契约约束）：

- 头文件：`<prefix>/include/lingqu/mem_service/`（`mem_service_provider.h`、
  `mem_service_provider_roce.h`、`mem_service_provider_tcp.h` 等）；
- 源码 SDK：`<prefix>/src/lingqu/mem_service/` 下的
  `mem_service_provider.c`、`mem_service_provider_roce.c`、
  `mem_service_provider_tcp.c` —— ds4 把这三个源文件直接编译进自己的
  二进制（`mem_service_provider_sdk.o`、`mem_service_provider_roce_sdk.o`、
  `mem_service_provider_tcp_sdk.o`）；
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

## 7. 排错

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
