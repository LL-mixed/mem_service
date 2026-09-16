# Memory Service Component

## Managed store 恢复实施约定

`serve --store` 必须在首次受管理分配起保存完整分配表、原操作身份、holder、
mapping 事务、代际上界和隔离状态，不依赖幂等缓存归档触发。采用独立版本化
store magic，旧服务拒绝读取；保留原 metadata-only store 兼容路径。持久确认
须先于成功应答及 provider 工作交付；不确定写入停止新的资源状态变更。
重启仅恢复资源身份和待对账义务，不恢复进程指针、provider 注册或数据就绪。
未终结对象进入隔离，已有引用和映射继续计量，重复重启不得清零或重复分配。
完整资源回收和重新加入仍须经 provider/kernel 实际对账与 fencing 证明。
回归沿用真实 daemon/CLI 的 provider-backed allocation 测试；合成 descriptor
只用于控制状态验证，不能当作物理 backing 恢复证据。

候选实现使用 `mem_service_store_managed_v1` 保存字段级编码的分配表及完整 V2
record 绑定，附校验和与完整结束标记；未知版本、截断、损坏及多余尾部拒绝。
资源状态和精确应答在同一原子 snapshot 中确认，该格式不重放旧 reply-only
journal 覆盖快照。历史归档仍用原 `.replay-history` 及前缀校验。写入不确定
后禁止继续变更或向 provider 交付新工作；修复文件路径不自动解除该状态。
原 wire 完整/分页 snapshot 不携带此恢复域，已有 managed 状态时拒绝导出及
覆盖。旧 metadata-only store 保持兼容；缺少资源状态的旧 managed store
不能直接恢复数据就绪。core 消费者须重新构建，wire 与 client record ABI 不变。

## Provider 失联的运行期隔离

目录租约到期、成功注销或 incarnation 替换必须在旧登记消失前后同一串行化域
通知 managed 分配状态机。该 home 的未终结分配进入 QUARANTINED；当前 holder
尚无节点归属，因此其他仍有 holder 的 provider 分配也保留并隔离。身份、地址、
descriptor、holder 和 mapping transaction 不清除，不调用 backing release。
服务锁存 managed_recovery_required，重新注册不解除该状态。新 managed 数据
工作在幂等重放前拒绝；只读查询及已有 mapping 清理、release/retire 继续使用
原代际和事务检查。隔离状态不能通过普通 reclaim 确认或重放重新激活。

quarantined_bytes 单独计量已确认 backing；尚未 publish 的隔离意图保持 in_flight，
已确认字节数为零不证明 provider 未分配。隔离对象的 holder、export/import 仍
计入对应资源数。此阶段为运行期准入与记账，不能撤销既有 CPU 页权限，也未补齐
跨 daemon 重启的持久分配表、holder 节点归属、fencing 或解除隔离接口。CLI 使用
既有 provider-register/refresh/deregister、allocation-stats 与 inspect-allocation；
不新增 wire 操作或改变公开结构布局，core 源码消费者需重新链接。

## 退役 descriptor 的同进程诊断契约

`object-session` 的 `capture_mapping key=<old>` 只保存当前完整 allocation
映射的身份快照；调用前必须持有映射，并通过带 seed/checksum 的 read。
每个 session 最多保存一份，不能接受外部地址或 descriptor，不复制 holder。
旧对象退役、新一代对象同址映射并通过带期望值的 read、再解除映射后，可执行
`probe_retired_mapping key=<old>`。新对象的 holder 必须保持，且查询确认旧代
retired、新代 active。诊断将原始快照直接交给 provider 映射，随后要求目标
地址的同步 CPU 读故障和真实 unmap 成功。普通 map 错误标为未分类失败；
清理失败保留独立 binding 并阻止后续操作。调用方必须随后重新映射并校验新
对象，排除 provider 整体不可用。loopback 只验证探针逻辑；真实 OBMM 结果
须独立验收。该诊断不覆盖旧 GSVA 字段与新 export token 的组合、强制撤销、
重启恢复或新映射仍存在时的裸指针隔离，不改变业务 SDK/wire。

## 解除映射后的 CPU 诊断契约

`object-session` 的 `unmap key=<key> probe_unmapped=1` 显式启用同进程
CPU 探针。它要求当前可读映射，先读取原地址，再完成原 managed unmap，
最后读取保存的地址并要求同步目标地址故障。unmap 失败保持清理上下文且
不执行探针；默认 unmap 不增加 CPU 访问。探针不接收外部地址，不复制
mapping/holder 归属，不改变 SDK/wire。它只验证新映射建立前的不可访问性，
旧身份拒绝、强制撤销及同址新映射后的访问隔离仍须独立验收。

## 幂等历史批量归档约定

既有 `serve --store` 的缓存归档使用有界批量接口，一批最多等于幂等缓存
容量。每批完整扫描并校验全部历史一次，核对批内及历史 key 的精确重复或
冲突，全部预检通过后才追加；不以提前命中或缓存命中跳过历史尾部校验。
新增帧统一同步成功后才更新内存 checkpoint，再持久化含 checkpoint 的
服务快照，最后淘汰原缓存。写入或同步不确定时保留整个缓存，句柄进入
failed 状态；完整后缀可在重新打开时验证，半帧不自动截断。批次不承诺
失败时零字节落盘。该内部优化不改变 wire、安装 SDK、磁盘帧格式或请求
超时，也不替代实际 guest 百轮与 managed backing 恢复验证。

## V2 writer 发布准备契约

`mem_service_client_prepare_managed_reference()` 接收 begin 返回的 allocation、
当前完整读写 mapping/lifecycle 和逻辑视图参数。它先核对服务端 allocation 与
活动 mapping，再从实际映射字节计算 checksum，确认中立 provider publish 完成后
返回只读 V2。失败不修改引用输出，不发送 stage/seal。调用者保持唯一写入权，
从准备开始到 seal 完成禁止并发改写 payload，并保留返回的完整引用用于同一
idempotency key 的 stage 重试；应答不确定时禁止重新生成引用。多个逻辑视图可
在同一次冻结的完整映射上准备，全部 stage 后确认 unmap，最后 seal。
该 API 不代替 begin/stage/seal 的服务端准入，也不提供进程崩溃后的恢复。

## V2 reader 映射接入契约

`reference-transition` 增加 `map-begin`，只接受完整已登记、当前封存版本的
只读引用，并要求 session 已实际持有该 allocation。它创建原有 pending mapping
事务并预留后续清理结果容量；旧 raw BEGIN 仍不能映射封存的 V2 对象。映射意图
和活动映射都阻止内容换版。缓存 map-begin 仅可重放仍然 pending 的同一事务，
结束或已确认的历史回执不得重新授予映射。

新 SDK reader lifecycle 将原映射生命周期与完整 V2 分开保留，原结构 ABI 不变。
不确定的 BEGIN 用原引用及 operation ID 重试；取得 mapping ID 后复用原确认、
解除和终结状态机。provider 只接收服务返回的 allocation binding 与引用中的
精确子区间，返回只读视图。此契约的代码、CLI、失败注入及真实 guest 验证分别
记录，定义接口不构成已完成声明。

## 版本化全局对象引用编码（AM2 接入基础）

`lingqu_object_service.h` 保留 64-byte V1 布局、版本常量和既有名字，新增固定
256-byte V2 及 header-only 编解码 API。V2 保留原对象元数据字段，并携带完整
allocation key、generation、home ID/incarnation、逻辑大小和访问权限。
V2 payload offset 相对 allocation；V1 的 arena offset 语义保持不变，禁止
仅改版本号升级旧引用。编码明确使用 little-endian，拒绝未知版本/长度/权限位、
非规范字符串和范围溢出，失败不修改输出。key hash 沿用现有算法，完整 key
参与后续身份比较，hash 不能独立标识对象。

这些函数不读取 payload、不连接服务、不授予 holder 或访问权限，也不证明
当前内容版本、checksum 或 backing 身份。服务绑定及 reader SDK 使用下述显式
协议，实际跨 guest 数据闭环、W5 接入及重启身份域仍须验证。原生验证入口为
`make -C apps/mem_service object-ref-v2-smoke`，安装态入口为
`installed-object-ref-v2-smoke`；夹具通过不代表完整 AM2 已验收。

## 受管理 session 的数据可见性

V2 引用的服务端绑定沿用同一个 record table，并将逻辑 object key 显式绑定到
allocation 的完整身份与视图。新增 core 发布序列为 begin/stage/seal：只有持有
唯一 holder 的 allocation owner，且没有遗留 mapping transaction，才能 begin；
begin 推进 content version 并使旧引用失效。stage 登记同代同版本的多个对象视图，
seal 要求写入映射已确认解除且至少有一份有效视图。失败不恢复旧版本，因为数据
可能已被写入；放弃发布须走现有 retire/release/reclaim，不能让旧引用重新有效。

resolve/acquire 必须匹配 record 中登记的完整 V2、当前 ACTIVE allocation 和
已封存版本。V2 模式关闭旧 raw acquire 和已封存对象的旧 mapping BEGIN 入口，
防止它们绕过版本检查；专用 V2 reader mapping 使用独立 map-begin 准入。
core API 由服务单一串行化域调用，不提供独立锁。seal 表示可信 writer 提交发布
元数据，core 不读取或验证 payload；SDK 仍须先完成 provider publish、可见性与
mapping 清理。wire 与 SDK 显式接入见下文；core 夹具不证明 W5、重启恢复或
实际跨 guest 数据访问通过。

完整 service 包含有界的大型 record/幂等表，生产 snapshot restore 的临时 service
必须使用可检查分配失败且所有出口都释放的堆对象，避免嵌套大栈帧。独立单进程
CLI restore fixture 的多个 service 使用静态 scratch 并逐次初始化；不提高系统
栈限制来掩盖恢复路径的栈容量问题。

映射失败后的清理状态不能丢失。中立 map wrapper 返回
`MEM_SERVICE_MAPPING_CLEANUP_REQUIRED` 时，输出 binding 保留 owner/handle，
`mapped=true` 表示仍有资源需要清理，base/len 清零以禁止访问；调用方必须保留
binding 并重试 unmap。SDK 同时保留对象 key/generation，清零可访问地址、长度与
权限；任何 unmap 失败也转入该不可访问状态。完成清理前不得 release holder 或
cancel/finish 映射事务。provider 自身的 import/部分 VMA 回滚仍须按同一原则接通。

映射生命周期扩展按以下契约实施：服务在 provider 建立映射前登记 pending 意图，
分配本次服务生命周期内不复用的 mapping ID，绑定对象 generation 和 holder session。
成功建立后确认 active，解除前进入 closing，provider 确认解除后删除记录；pending
仅在确认没有遗留 provider 资源时允许 cancel。任何尚存映射事务均阻止该 holder
release。`import_mappings` 统计已确认 active/closing 映射，pending/closing 同时计入
`in_flight`，pending 为零不能替代映射成功确认。服务只接收生命周期元数据。
此扩展需要 core、wire、SDK、CLI 和真实 guest 一起验证；持久化与重启 reconciliation
仍按恢复阶段实现，不能把进程内计量宣称为崩溃后的 kernel 资源枚举。

受管理 SDK 的 map/unmap 在进程内编排上述事务和 provider 调用。每次新 map 使用
全局唯一 operation ID；调用方将零初始化的 lifecycle 与 mapping 成对保留，禁止
复制、并发使用或用历史应答重建该上下文。map 只调用一次，失败后通过同一上下文
重试 unmap。BEGIN 应答不确定时保留 operation ID，清理先重放该 BEGIN 取得 ID；
此恢复仍受服务 readiness 检查约束。BEGIN 禁用 wire 内部自动重试，防止已提交但
丢失的应答被后续 readiness 拒绝覆盖。FINISH/CANCEL 应答不确定时重放原终结
操作，不能把 NOT_FOUND 当作清理成功。CLOSE 或状态查询失败时撤销 SDK 访问，但保留
实际 provider 映射，等待控制状态可确认后再清理；这不构成 CPU 页权限撤销。
旧 raw map/unmap 接口保留供底层集成，调用它们不会自动登记服务端映射计数。

managed map 先只读查询服务端 allocation，逐项核对不可变绑定和完整 opaque
descriptor；差异返回 STALE_REF，查询失败直接返回错误，两者都不创建事务或占用
幂等记录。随后 BEGIN 再检查 generation、ACTIVE 和 holder，防止查询后发生退役
或换代；同一 ACTIVE generation 的已发布绑定禁止改写。provider 只在 pending
确认后收到核对过的服务端快照。可变引用计数与版本统计不参与绑定比较。此查询
只发生在建立映射时，数据访问不新增控制 RPC；BEGIN 后的不确定结果仍保留清理上下文。

`object-session` 的引用跟踪以 `(key, generation, session_id)` 为单位，独立于
最近一次 inspect 返回的对象。切换 key 或使用显式 session_id 不得丢失尚未释放
的引用；退出时逐项报告已知未释放的 holder，并返回失败，不自动替客户端 release。
同一 session 内已成功执行的 acquire/release 幂等重放不再次改变本地引用状态。
映射仅消费配置中 session_id 自己的引用；同一进程代其他 session 执行 acquire
不授予本 session 映射权限。该跟踪属于 CLI 诊断，服务端仍是引用状态权威；
它不提供跨进程同名 session 的并发协调或崩溃恢复。

`object-session` 的 `publish_data` 和 `wait_visible` 操作接受 `key`、`offset`、
`len`，以及 `seed` 或 `expect_checksum`，在客户端进程调用中立 provider 范围接口。
写入方先调用 `publish_data`，再通知另一客户端；读取方先调用 `wait_visible`，
再校验字节。等待预算取 session 的 `request_timeout_ms`。`publish` 仍表示 home
provider 的分配确认。原始 `write`/`read` 保留用于可见性和 checksum 负例。

OBMM session 的 `provider_node_id`、`provider_node_count`（2 至 8）和非零
`provider_generation` 标识 peer-canary 组。参与者同时启动，使用同一 generation
导出 canary、交换 bootstrap descriptor，并校验所有 peer 的 checksum 后绑定本地
SDK channel。此诊断 bootstrap 不分配应用对象，也不设置服务目录的 ready 状态。
canary 大小使用 `provider_import_region_bytes`，必须满足部署的 OBMM pool 粒度。
受管理对象保留逻辑大小；provider 的对齐 backing 可更大，SDK 数据操作以逻辑大小
检查边界，allocation-stats 按实际发布的 backing 地址跨度计量物理占用。
SDK 向 provider 请求逻辑视图长度，同时单独传递 backing 的完整描述和跨度。
严格 OBMM 映射保留完整 backing 地址区间，对视图之外的完整页设置 PROT_NONE；
页内 padding 仍受 SDK 字节范围约束，CPU 页保护无法提供子页隔离。
已发布 reservation 进入 quarantine 后，`quarantined_bytes` 同样包含 backing
的对齐空间。尚未发布的取消请求保持 `in_flight`，已确认 backing/address 占用为
零；这两个计数无法证明 home 从未创建资源，仍必须等待 home 的取消确认。

## 受管理访问 owner（AM2 实施契约）

writer 可调用 `mem_service_mapping_owner_unmap` 停止新 borrow、排空 pins 并
完成原映射解除，同时保留同一 owner 的 holder。该中间状态用于 V2 writer
在全部 stage 后执行 SEAL；unmap 成功不代表内容已 seal。忙碌和不确定清理
继续保留 owner，重试不得释放 holder 或建立新映射。成功后不能再 borrow，
仍须在 SEAL 成功或明确放弃发布后调用原 close 释放 holder，再 destroy。
旧 close 继续一次完成 unmap/release；公开结构、wire 和原调用行为保持不变。

新增中立 SDK 模块 `mem_service_mapping_owner.h/.c`。一个 access domain 借用一个
已就绪 provider channel，并用同一 mutex 串行化域内全部生命周期和 provider
callback；channel、registry 和 endpoint 必须保留到 domain 销毁，域外不得并发
操作它们。多个 mapping owner 共用该 domain，避免每对象各自加锁后仍竞争同一
provider。该约定不把已有 raw provider 接口宣称为线程安全。

owner 接管已经成功 map 的 SDK mapping/lifecycle 及其同一 holder，成功时清零
调用方的两个原结构，失败时不改变原归属。接管前核对服务端当前 mapping 事务，
拒绝关闭后的旧快照、域内重复 mapping/holder，不另建对象或映射。CPU borrow 与
compute borrow 各自持有有界访问 pin；计算开始后额外计量 in-flight pin，只有
平台确认原 operation 的 completion 才能结束。超时和取消请求不能释放该 pin。
borrow 只保护寿命，应用仍须为重叠数据的读写建立 publish/acquire 顺序。

close 先停止新 borrow，在用 pin 返回 EBUSY；已有 borrow 继续有效。最后 pin
退出后依次执行原 managed unmap 和同一 holder 的幂等 release，失败保留 owner
供重试。close 不释放 owner 的进程内句柄；线程退出后显式 destroy，避免与仍在
调用的线程竞争 free。borrow 本身只由其持有线程释放，平台 callback 不得重入
domain API。统计区分 CPU pins、compute pins、in-flight pins 和清理状态。

该模块显式 opt-in，使用 pthread；旧 SDK source 集合与 wire/record ABI 不变。
CLI 接入沿用 ub_sim 的 managed PTO 入口，先通过原生并发/故障 fixture，再要求
真实双节点同 backing、并发在用拒绝关闭及最终资源回归。此段定义实现和验收要求，
当前不构成已完成声明。

原生并发与故障诊断入口为 `make -C apps/mem_service mapping-owner-smoke`，以及
`python3 -m unittest tests.test_mem_service_mapping_owner`。安装态附加查询
`pkg-config --variable=mapping_owner_sources lingqu-mem-service` 和
`--variable=mapping_owner_libs`，与原 `sdk_sources` 一起链接；不启用该模块的
消费者不新增 pthread 依赖。fixture 使用生产 owner、真实线程和 mutex，控制与
provider 边界由替身承接，不能计作实际 guest 或崩溃恢复证明。

## 受管理资源的清理容量（现有协议）

V2 控制接入使用独立中立 operation `0x7e reference-transition`，action 为
begin/stage/seal/resolve/acquire/map-begin。begin/seal 的 key 指 allocation，stage/resolve
的 key 指逻辑 object，acquire 的 key 必须等于完整 reference_hex 中的 allocation
key。map-begin 同样使用 allocation key，额外创建只读映射事务。V2 通过 512 个
hex 字符传递；只传元数据，不代理 payload。新 SDK/CLI 明确 opt-in，旧端拒绝
未知 action；reader 的 provider 映射由客户端进程内 SDK 执行。

新操作在幂等重放前核对当前 allocation generation、content version 和活动 home
incarnation，resolve/acquire 还须核对已登记完整视图。begin 预留首个 stage 与 seal
的结果容量，首个有效 stage 和 seal 可使用各自预留；acquire 预留 release。
无效 stage/seal 不得消耗其他操作的清理位置。通用 record retention 不删除
managed-view 或该协议的幂等结果，旧 metadata 写入不得覆盖 managed-view。
这些约束属于当前 daemon 生命周期，未提供重启后身份域或持久化恢复认证。

daemon 接纳新请求时，必须在幂等结果表中预留已有资源完成正常清理所需的空间：
每个 ALLOCATING/ACTIVE 对象保留一次 retire，每个 holder 保留一次 release，
pending/active/closing 映射分别保留 3/2/1 次后续状态迁移。新 allocate/acquire
按最多增加一条义务估算，mapping BEGIN 按三条估算，reference BEGIN 按两条
估算，并另计本次请求的幂等结果。
普通请求不能使用这些预留位置；有效的清理迁移释放相应义务后，才可将应答写入。
无效或未推进状态的清理请求不能消耗最后的清理空间，返回可重试的
`CAPACITY_EXCEEDED reason=cleanup_capacity_reserved`，不缓存该临时拒绝。

已缓存的精确重放和只读查询继续可用，既有 readiness/代际检查仍生效。保留历史
结果，禁止通过任意淘汰旧 BEGIN 来腾出位置。已有 managed identity（含 retired
记录）时禁止 snapshot restore 覆盖幂等历史；通用记录保留策略也不能删除 managed
请求的结果。`allocation-stats` 暴露容量、已用量、清理预留量、非预留可用量及
预留缺口。此契约只约束单次 daemon 生命周期内的新接纳；历史超额状态、重启恢复、
幂等历史的可回收协议和连续运行容量仍需独立完成，不能据此宣布长期容量问题解决。

## Provider allocation work polling

`poll-allocation` returns the lowest allocation generation greater than
`after_generation` bound to the caller's active `node_id` and `incarnation`.
Eligible states are ALLOCATING and RETIRING with zero holders. No work returns
NOT_FOUND; a provider scans from zero again after reaching the end. Polling is
read-only and does not claim, allocate, or reclaim resources. A single worker
per provider incarnation must reconcile reservations by `(key, generation)`
before retrying work and confirm results through publish/reclaim. A response
is a snapshot: cancellation can race execution and must be reconciled before
resource reuse. This API carries metadata only and does not prove backing or
data-plane readiness. It uses the existing trusted-control-endpoint security
boundary; a self-reported node identity is not authentication.

`allocate-object` accepts an optional provider-neutral `home_node`. When it is
present, the daemon binds a new allocation to that node's current active
provider-directory incarnation. When it is absent, `allocation_home_provider`
remains the default. The typed SDK exposes this without changing the v1 request
struct ABI through `mem_service_client_allocate_object_at_home()`; the existing
`mem_service_client_allocate_object()` keeps the default placement behavior.
Unknown, expired, or unready homes fail closed before an allocation intent is
created. Replays cannot move an existing key to another home. Each selected
home still requires its own allocation worker and independent physical-resource
ownership; placement metadata alone does not coordinate overlapping address
ranges across homes.

`mem_service` owns the guest-side memory/object metadata service used by LLM
inference guest harnesses and is being promoted into a standalone Memory
Service process.

## Reserved payload in-place publication

An in-process model runtime may reserve output bytes from the local payload
arena, let a compute provider fill that range, and publish it through
`mem_service_range_flow_publish_runtime_output()`. Set
`publish_payload_in_place` and pass the reserved `publish_payload_offset` in
`mem_service_obmm_range_flow_request`. The payload pointer must equal the local
OBMM slot base plus that offset, and the complete range must remain within the
already reserved arena. A successful in-place publish records
`payload_mode=in_place` and skips the payload copy.

KV output can use an independent in-place reservation. Before computation,
call `mem_service_model_kv_state_alloc()` with the complete KV payload length
(including its header). Bind that reserved range to the compute provider, then
set `publish_kv_in_place` and `publish_kv_offset` after successful completion.
Publication validates the exact local pointer, block alignment, and the entire
tier-rounded reservation against arena and slot bounds before reading its
checksum. It skips allocation and copy, reporting `payload_mode=in_place` and
`publication_copy_bytes=0`. Zero-initialized requests retain the allocation and
copy path. The in-process request struct has grown; source consumers must rebuild.

The reservation validator and allocator are exercised through:

```sh
python3 -m unittest discover -s tests -p test_mem_service_kv_reserved.py
```

This executable test covers reservation validation and the actual publisher
with host-side visibility/notification fixtures. It measures explicit payload
copies, arena growth and the resulting record offset, including the legacy copy
path and invalid-pointer/checksum/visibility failures. Guest integration and
end-to-end in-place publication require separate W5 runtime evidence.

This contract currently trusts the in-process caller that performed the arena
reservation. Allocation tokens and concurrent owner validation are required
before exposing the path to unrelated callers or concurrent dispatch owners.

## Remote token visibility

Range and terminal-token readers share the bounded remote refresh helpers in
`mem_service_cluster_read.c`. An existing imported mapping requires explicit
metadata refresh before lookup and payload refresh before copying token bytes.
The metadata refresh confirms the publication sequence after copying records;
sync failures and inconsistent publications leave the read unsuccessful.
Local slots do not require remote synchronization. Token step, bounds, kind,
and checksum checks remain required after refresh.

Run the following command from the repository root for
the executable stale-import, local-read, sync-failure, publication-consistency,
checksum, bounds, and address-overflow regression cases.

```sh
python3 -m unittest discover -s tests -p test_mem_service_terminal_token_visibility.py
```

## Model/runtime naming boundary

Layer-range dispatch, hidden-state handoff, per-range KV publication and
resolution, decode-round barriers, object-backed operands, and their logs are
model-neutral runtime infrastructure. Their source files, public and private
types, functions, stage names, and test fixtures must use `model_range` or
another model-neutral name. Qwen3 and DeepSeek names are reserved for model
adapters, model geometry, tokenizer behavior, Engram behavior, and
model-specific operator execution. The descriptor opcode and wire field layout
are stable across this naming boundary; renaming the runtime API must not
change their numeric values or serialized representation.

It now has a core-only app build, a minimal Unix-socket daemon/client path, and
a Qwen3 adapter inspect build:

- `mem_service.c` implements the DB/object service, GSVA-mediated access
  metadata, and runtime object metadata paths.
- `mem_service_internal.h` contains the private include aggregate and service
  private compatibility shims shared by the split implementation units.
- `mem_service_compiler.h` contains local compiler annotations used by split
  implementation units.
- `mem_service_wire.h` contains the versioned request/response envelope,
  operation IDs, payload checksum, and stable error-code contract used by the
  daemon/client path.
- `mem_service_wire_client.c` and `mem_service_wire_client.h` contain the
  lightweight Unix-socket client transport, default endpoint, and stable status
  naming helpers. External clients can link this unit without linking the
  daemon, record core, or Qwen3 adapter.
- `mem_service_client.c` and `mem_service_client.h` contain the model-neutral
  typed C client wrapper for object, prefix, KV, runtime handoff, execution
  artifact, generic training artifact RPC, and pretraining-specific
  dataset/sample/checkpoint/gradient/optimizer-state plus training-step
  commit helpers. External
  serving/pretraining clients can link this unit plus
  `mem_service_wire_client.c` without linking the daemon, record core, or Qwen3
  adapter.
- The typed client also exposes `mem_service_client_materialize_object()` for
  large sealed payloads. The daemon validates the recorded version, checksum,
  and backing block, writes to a new caller-selected path, and never overwrites
  an existing destination. This keeps large object bytes out of the 4 KiB
  text-kv response envelope while allowing serving runtimes to restore objects
  owned by mem_service.
- `apps/mem_service/examples/mem_service_serving_example.c` is the installable
  serving SDK smoke client. It uses only the typed client API to publish/query
  prefix, KV, runtime handoff, and execution artifacts through a standalone
  daemon.
- `apps/mem_service/examples/mem_service_pretraining_example.c` is the
  installable pretraining SDK smoke client. It uses only the pretraining typed
  client API to publish/query dataset shard, sample batch, checkpoint, gradient
  bucket, optimizer-state, and training-step commit refs through a standalone
  daemon.
- `tests/test_mem_service_daemon_runtime.py` includes a pretraining worker
  runtime gate that compiles an external client and verifies worker0/worker1
  publish, typed resolve, global-step commit marker, checkpoint restart
  recovery, stale/checksum fail-closed behavior, and idempotency conflict
  through `linqu_mem_service serve --store`.
- `mem_service_wire_payload.h` contains the shared text key/value payload
  reader, writer, and schema validator used by both the CLI client and daemon
  fixture gate. It is a compatibility helper for the current minimal payload
  format, not the final binary typed schema.
- `mem_service_wire_schema.h` contains the public operation-to-payload schema
  contract for the current text key/value protocol, including required fields,
  field types, and selector rules such as `resolve-kv` accepting either `key`
  or `block_hash`.
- `apps/mem_service/wire-schema.txt` is the checked-in manifest generated by
  `linqu_mem_service wire-schema`; it freezes the current operation/field
  surface for release diffing and install smoke checks.
- `apps/mem_service/api-abi-policy.txt` is the checked-in manifest generated by
  `linqu_mem_service api-abi-policy`; it freezes the current client API/ABI
  version, public record ABI size, wire/header/schema version, and old/new plus
  upgrade/rollback policy for release diffing and install smoke checks.
- `apps/mem_service/admin-output-schema.txt` is the checked-in manifest
  generated by `linqu_mem_service admin-output-schema`; it freezes the current
  status/list-records/metrics/audit/snapshot/restore text output, Prometheus
  metric prefix/type contract, and fail-closed status fields for release
  diffing and install smoke checks.
- `apps/mem_service/upgrade-rollback-policy.txt` is the checked-in manifest
  generated by `linqu_mem_service upgrade-rollback-policy`; it freezes the
  current-version-only upgrade/rollback policy, same-version restart/restore
  gates, and old-server runtime-binary certified status for release diffing
  and install smoke checks.
- `apps/mem_service/ops-certification-policy.txt` is the checked-in manifest
  generated by `linqu_mem_service ops-certification-policy`; it freezes the
  production-ops admission boundary and keeps real systemd, Prometheus/
  Alertmanager, rpm, and deployment upgrade/rollback certification fail-closed
  until external Linux CI supplies those gates. `linqu_mem_service
  ops-certification-generate-evidence` probes the local Linux/systemd/rpm/
  metrics environment and emits a fail-closed evidence file; `linqu_mem_service
  ops-certification-verify --evidence-file <path>` validates that evidence.
  `linqu_mem_service ops-certification-linux-ci-smoke --rpm-file <path>
  --upgrade-rollback-marker <path> --evidence-file <path>` runs the generator,
  persists the evidence, and verifies it as a single CI gate. Local fixtures
  cover the positive/negative parser behavior and the non-Linux fail-closed
  path.
- `scripts/run_mem_service_linux_ops_ci.sh --rollback-rpm <previous-rpm>
  [--rpm-file <current-rpm>]` is the recommended real-Linux CI wrapper for
  deployment certification. In source-tree mode it runs
  `linux-ops-deployment-smoke` followed by `linux-ops-certification-bundle`.
  When run from an installed `share/` tree, it self-locates the installed host
  binary and installed manifests, requires `--rpm-file <current-rpm>`, then
  performs the rpm/systemd/metrics/alert/evidence/bundle flow without a source
  checkout. Both modes emit
  `ops-certification-linux-ci.evidence` plus
  `ops-certification-upgrade-rollback.marker` plus
  `linqu-mem-service-ops-certification-bundle.tar`.
  `scripts/run_mem_service_linux_ops_ci.sh --preflight` checks the Linux/root/
  systemd/rpm/promtool toolchain and rollback rpm prerequisites before running
  the destructive deployment and rollback flow. After CI publishes the evidence
  artifact, `scripts/verify_mem_service_linux_ops_evidence.sh
  --evidence-file <path>` rebuilds `linqu_mem_service_host` if needed and
  re-runs `ops-certification-verify --evidence-file <path>` as an independent
  artifact verifier. The app Makefile also exposes
  `linux-ops-evidence-verify OPS_CERTIFICATION_EVIDENCE=<path>` for CI systems
  that call release gates through `make`. Once the evidence is verified,
  `linux-ops-certification-bundle` packages the verified evidence, upgrade/
  rollback marker, rpm, release manifest, package manifest, and ops policy into
  `linqu-mem-service-ops-certification-bundle.tar` for release audit and
  cross-machine handoff. `scripts/verify_mem_service_ops_certification_bundle.sh
  --bundle-file <path>` and the Make target
  `linux-ops-certification-bundle-verify OPS_CERTIFICATION_BUNDLE=<path>`
  validate that handoff bundle and re-run evidence verification from the
  extracted artifact.
- The installed layout now ships the release verification scripts under
  `share/lingqu/mem_service/scripts/`: Linux ops CI, Linux ops evidence
  verifier, ops bundle verifier, remote-transport CI, remote-transport
  evidence verifier, remote-transport bundle verifier, release certification
  verifier, a release certification CI wrapper, an installed-layout selfcheck,
  and an installed SDK verifier. `package-manifest` records `release_script_root`,
  each `release_script`, `linux_ops_ci`, `linux_ops_ci_preflight`,
  `remote_payload_production_transport_ci`,
  `remote_payload_production_transport_ci_preflight`, and
  `file_class=release_scripts count=10`; install, tar, deb, and rpm smokes
  verify those scripts are present and executable.
  `scripts/verify_mem_service_installed_layout.sh --no-runtime` validates the
  installed `bin/`, `libexec/`, `share/`, `etc/`, systemd, config, deploy, and
  manifest layout without requiring the source tree.
  `scripts/verify_mem_service_installed_sdk.sh` validates the installed
  `pkg-config` metadata, compiles serving/pretraining clients from installed
  SDK sources, and can run them against the installed host daemon without a
  source checkout. `--preflight` checks the installed SDK layout, compiler,
  `pkg-config` discovery, non-empty `Cflags`, non-empty `sdk_sources`, example
  sources, and host binary without compiling or starting the daemon.
  `scripts/run_mem_service_remote_transport_ci.sh` also self-locates the
  installed host binary and installed manifests when it is run from an
  installed `share/` tree, so remote transport evidence and its bundle can be
  generated without a source checkout.
  `scripts/run_mem_service_linux_ops_ci.sh` has the same installed self-location
  behavior for real Linux ops certification; installed mode requires the current
  rpm artifact through `--rpm-file` instead of rebuilding it from source.
  `scripts/run_mem_service_release_certification_ci.sh --preflight` checks the
  destructive release-certification prerequisites first: Linux/systemd/root,
  rpm/promtool toolchain, rollback rpm, remote transport source, partition
  marker, and producer/consumer separation. When those local checks pass it
  delegates to the installed SDK, Linux ops, and remote transport wrappers with
  `--preflight`, so the release-level gate uses the same installed/source-tree
  mode checks as the child stages. `--preflight --dry-run` prints that
  preflight plan without touching systemd or rpm, which is useful for
  validating installed-package CI argument routing on non-Linux developer
  hosts. The `linux_ops_ci`,
  `linux_ops_ci_preflight`, `remote_payload_production_transport_ci`,
  `remote_payload_production_transport_ci_preflight`,
  `installed_sdk_preflight`, `installed_sdk_preflight_scope`,
  `release_certification_ci`, and `release_certification_preflight` manifest
  fields make those release gates machine-discoverable without reading this
  README.
  The evidence, bundle, release verifiers, and installed release CI wrapper
  first resolve the installed `libexec/lingqu/mem_service/linqu_mem_service_host`
  relative to that `share/` script directory, then fall back to the source-tree
  app directory only when the installed host binary is absent or `--app-dir` is
  supplied.
- `mem_service_daemon.c` contains the model-neutral Unix-socket service loop,
  public wire schema checks, the minimal object,
  prefix, KV, runtime handoff, execution artifact, and training artifact RPC
  handlers, plus the read-only `status`/`list-records` admin handlers.
- `mem_service_daemon.h` contains the public daemon/client helper contract used
  by the CLI entrypoint.
- `mem_service_runtime_config.h` contains runtime wait defaults, environment
  parsing, and neutral run-id resolution.
- `mem_service_cluster_payload_contract.h` contains the device-independent
  cluster metadata payload wire format shared by guest and host service
  deployments.
- `mem_service_cluster_payload.c` contains the cluster metadata payload
  snapshot, compact summary, and local publish helpers compiled as a
  standalone guest runtime translation unit.
- `mem_service_cluster_payload.h` contains the private cluster payload helper
  contract used by observe and Qwen3 data-flow split units.
- `mem_service_cluster_read.c` contains stable cluster payload read, compact
  summary read, and slot record lookup helpers compiled as a standalone guest
  runtime read-side translation unit.
- `mem_service_cluster_read.h` contains the private cluster read helper
  contract used by observe and Qwen3 data-flow split units.
- `mem_service_guest_runtime.h` contains guest OBMM cluster runtime state,
  mapped slots, queue descriptors, and region layout constants.
- `mem_service_cluster_runtime.h` contains the private guest cluster runtime
  accessor and activation contract used by split transport/model units.
- `mem_service_cluster_runtime.c` contains guest OBMM cluster bootstrap,
  export/import slot activation, and pool layout helpers compiled as a
  standalone transport runtime translation unit. It can derive backend-neutral
  GSVA descriptors from the current OBMM-backed cluster mappings, but GSVA is
  not owned by the OBMM pool backend.
- `mem_service_cluster_utils.c` contains cluster environment parsing, wait
  throttling, and OBMM region range update/sync helpers compiled as a
  standalone guest runtime utility translation unit.
- `mem_service_cluster_utils.h` contains the private guest cluster utility
  contract used by runtime, payload, and model data-flow split units.
- `mem_service_object_contract.h` contains device-independent OBMM object
  kinds, fixed payload sizes, and layout constants that must stay reusable by
  guest and host service deployments.
- `mem_service_records.c` contains the internal record-table allocation,
  lookup, and member helpers compiled as a standalone core translation unit.
- `mem_service_record_table.h` contains the private core record-table helper
  contract used by generic metadata and model/transport split units; it keeps
  record allocation and lookup dependencies explicit while the core moves
  toward a host-buildable library boundary.
- `mem_service_qwen3_records.c` contains Qwen3 streaming runtime record
  recycling policy compiled as a standalone model-adapter translation unit; it
  must stay out of the generic record core.
- `mem_service_qwen3_records.h` contains the private Qwen3 record recycling
  helper contract used by the model adapter and OBMM object publication path.
- `mem_service_qwen3_record_policy.h` contains Qwen3 runtime record retention
  constants used by the model adapter record policy.
- `mem_service_model_runtime.h` contains model-neutral payload checksum, KV
  allocation, and OBMM pool reporting helpers used by every range-flow model
  adapter.
- `mem_service_qwen3_runtime.h` contains only the private Qwen3 placement and
  Engram helper contract.
- `mem_service_qwen3_runtime.c` implements the model-neutral runtime helpers
  declared by `mem_service_model_runtime.h` plus Qwen3 placement and Engram
  helpers. The remaining mixed implementation is an internal split boundary;
  consumers must include the header that matches the helper they call.
- `mem_service_qwen3_placement.h` contains the Qwen3 layer-range placement
  contract used by the runtime range, KV, and object handoff flows.
- `mem_service_model_range_wait_flow.c` contains model-neutral runtime range
  input wait, scheduler work-item resolution, and mapped payload view helpers.
- `mem_service_model_range_publish_flow.c` contains model-neutral runtime range
  output, KV-state object publication, and downstream descriptor publication.
- `mem_service_model_range_kv_state_flow.c` contains model-neutral range KV
  publication and previous-step resolution helpers.
- `mem_service_model_terminal_token_flow.c` contains model-neutral terminal
  token publication and wait helpers; model adapters provide the request.
- `mem_service_qwen3_engram_publish_flow.c` contains Qwen3 engram candidate
  publish and decision-state publish helpers compiled as a standalone model
  data-flow translation unit.
- `mem_service_qwen3_engram_wait_flow.c` contains Qwen3 engram candidate,
  selected-token, history, and state wait helpers compiled as a standalone
  model data-flow translation unit.
- `mem_service_model_decode_barrier.c` contains model-range decode-round
  publish and active-topology all-node wait helpers.
- `mem_service_keys.c` contains device-independent key construction helpers
  compiled as a standalone core translation unit for guest and host service
  deployments.
- `mem_service_keys.h` contains the private key construction helper contract
  used by the prefix/KV metadata core.
- `mem_service_gsva_access.h` contains the backend-neutral GSVA region and
  buffer descriptor contract used by mem_service runtime flows and by backend
  adapters such as `ub-ssd-gsva-v1`.
- `mem_service_object_refs.c` contains device-independent checksum and Lingqu
  object-reference projection helpers compiled as a standalone core
  translation unit for guest and host service deployments.
- `mem_service_object_refs.h` contains the private checksum/object-reference
  helper contract used by runtime backends and Qwen3 data-flow code.
- `mem_service_obmm_objects.c` contains OBMM object payload generation, kind
  naming, payload arena allocation, and object record publication helpers
  compiled as a standalone runtime-adjacent translation unit.
- `mem_service_obmm_objects.h` contains the private OBMM object helper contract
  used by guest runtime and Qwen3 data-flow code.
- `mem_service_metadata.c` contains the prefix/KV metadata state machine used
  by both local metadata APIs and runtime-backed publication paths; it is
  compiled as a standalone core translation unit and explicitly depends only on
  the public service contract plus record helpers.
- `mem_service_cluster_queue.c` contains guest OBMM SPSC queue barriers,
  object descriptor publish/wait helpers, and pending descriptor matching
  compiled as a standalone transport queue translation unit.
- `mem_service_cluster_queue.h` contains the private transport queue helper
  contract used by runtime-backed Qwen3 and object publication flows.
- `mem_service_cluster_observe.c` contains cluster metadata fetch, observe,
  and readiness summarization across local and remote payload snapshots
  compiled as a standalone transport observe translation unit.
- `mem_service_cluster_observe.h` contains the private cluster observe helper
  contract shared by the guest service core and validation tests.
- `mem_service_obmm_object_flow.c` contains the guest OBMM object publish,
  descriptor exchange, remote resolve, and Qwen3 range handoff validation flow
  compiled as a standalone transport object-flow translation unit.
- `mem_service_obmm_object_flow.h` contains the private object-flow helper
  contract used by the guest service core.
- `mem_service_qwen3.c` is the private adapter from mem_service placement/KV
  semantics to the model-neutral `llm_infer` Qwen3 topology helpers.
- `mem_service.h` exposes the service API consumed by guest apps.
- `mem_service_core.h` exposes the model-neutral core service include surface
  for apps and future daemon/client code.
- `mem_service_qwen3.h` exposes the Qwen3 runtime range/KV/engram adapter API
  to guest inference code that opts into that model path.
- `lingqu_object_service.h` defines the object-service payload contract.
- `apps/mem_service/release-manifest.txt` freezes the current minimal release
  contract: service version, core binary path, optional Qwen3 adapter path,
  binary self-description command, public headers, client SDK source files,
  SDK example files, installed `pkg-config` metadata, config/deploy artifacts,
  wire/schema versions, wire schema manifest checksum, admin output schema
  checksum, release-readiness command/gate, operation IDs, and status IDs.
- `apps/mem_service/package-manifest.txt` freezes the current
  `installed-layout-v1` package contract: install roots, file classes, required
  contract checksums, binary self-description gate, release-readiness gate,
  required fixture gates, and not-certified boundaries for
  cross-version/runtime-environment claims. It
  also freezes the installed release-script payload so a published package can
  carry its own post-release evidence verification entrypoints without
  requiring a source checkout for the default verifier path.
- `release-readiness` emits a stable text-kv readiness report for release
  tooling. It reports the installed SDK contract, serving/pretraining runtime
  gate, Linux ops certification status, cross-host remote transport status, and
  `overall_status=not-certified` until external evidence is provided. With
  `--ops-evidence-file <path> --remote-transport-evidence-file <path>`, it
  replays the same ops and remote transport evidence validators used by the
  standalone verifier commands and emits `overall_status=certified` only when
  both pass. Bundle extraction and tar-safety checks remain in the installed
  release verifier scripts; the readiness CLI consumes the verified evidence
  files as the final machine-readable release gate. The same report also
  exposes `release_certification_verify`, so release tooling that starts from
  binary self-description can discover the full bundle verifier entrypoint
  without scraping the checked-in release manifest.
  `release-readiness-fixtures` keeps the default fail-closed report, certified
  report, and stale evidence rejection in the package required gate set.
- `make installed-sdk-example-smoke DESTDIR=<dir> PREFIX=/usr` installs the
  release layout, then compiles the serving and pretraining examples from the
  installed public headers, installed SDK sources, and installed example
  sources. This proves external serving/pretraining clients can build against
  the installed SDK boundary without depending on daemon-private source files.
  The install layout also writes `lib/pkgconfig/lingqu-mem-service.pc`, whose
  `Cflags` and `sdk_sources` variables expose the installed header and source
  roots to external builds without requiring callers to hard-code layout paths.
- `make installed-sdk-pkgconfig-smoke DESTDIR=<dir> PREFIX=/usr` verifies that
  installed SDK discovery metadata is actually consumable by external builds:
  it uses `pkg-config --define-prefix` to read the installed `Cflags` and
  `sdk_sources`, then compiles the serving and pretraining examples from those
  discovered values.
- `make installed-sdk-runtime-smoke DESTDIR=<dir> PREFIX=/usr` builds on that
  installed SDK layout, starts the installed host daemon through its installed
  binary path, runs the installed serving and pretraining example clients
  against the daemon over a Unix socket, and checks the resulting service
  status. This proves the installed daemon and external clients cooperate at
  runtime without relying on source-tree private paths.
- `scripts/verify_mem_service_installed_sdk.sh` is the source-checkout-free
  equivalent shipped in the package. It derives the installed prefix from
  `share/lingqu/mem_service/scripts/`, uses installed `pkg-config` metadata to
  compile the serving/pretraining examples, and by default runs the resulting
  clients against the installed host daemon. The release certification CI
  wrapper runs this SDK gate before the Linux ops and remote transport phases.
- `apps/mem_service/packaging/linqu-mem-service.spec` is the rpm package spec
  used by `make package-rpm` and `make package-rpm-smoke`. The rpm smoke is a
  Linux rpm-toolchain gate: it requires `rpmbuild`, `rpm2cpio`, and `cpio`; on
  hosts without those tools it fails closed rather than certifying an rpm
  artifact.
- `make linux-ops-certification-smoke` is the Linux CI evidence verification
  gate. It first runs `package-rpm-smoke`, then requires the real deployment
  upgrade/rollback marker, and finally runs `ops-certification-linux-ci-smoke`
  to persist and verify the evidence file. It is expected to fail closed on
  developer hosts without the rpm/systemd/upgrade-rollback environment.
- `make linux-ops-upgrade-rollback-smoke OPS_CERTIFICATION_ROLLBACK_RPM=<rpm>`
  is the privileged real-Linux upgrade/rollback gate. It requires Linux, root,
  systemd, rpm, curl, the built current rpm, and a readable rollback rpm; it
  installs current, restarts/scrapes both systemd units, installs the rollback
  rpm with `--oldpackage`, restarts/scrapes both units again, reinstalls
  current, and only then writes the upgrade/rollback marker consumed by ops
  certification.
- `make linux-ops-deployment-smoke OPS_CERTIFICATION_ROLLBACK_RPM=<rpm>` is the
  privileged real-Linux deployment gate. It runs the upgrade/rollback gate,
  installs the built current rpm, daemon-reloads systemd, starts both
  `linqu_mem_service.service` and `linqu_mem_service.host.service`, scrapes
  metrics from the service and host ports, checks the installed Prometheus
  rules, and then runs the same `ops-certification-linux-ci-smoke` evidence
  verifier.
- `scripts/run_mem_service_linux_ops_ci.sh --rollback-rpm <rpm>` is the
  reusable Linux CI wrapper around `linux-ops-deployment-smoke`. It keeps the
  rollback rpm and output directory explicit, then reports the expected
  evidence and upgrade/rollback marker artifact paths for CI collection.

Consumer-side build and validation entrypoints (these live in the `ub_sim`
repository, which consumes this component by source; see
`docs/integration-ub-sim.md` in this repository):

- `ub_sim/guest-linux/aarch64/scripts/build_initramfs.sh` links the mem-service
  CLI with
  `mem_service_daemon.c`, `mem_service_client.c`,
  `mem_service_wire_client.c`, `mem_service_metadata.c`,
  `mem_service_keys.c`, `mem_service_object_refs.c`, and
  `mem_service_records.c` into the core guest app binary.
- `ub_sim/guest-linux/aarch64/scripts/build_initramfs.sh` links the Qwen3
  adapter sources and `components/llm_infer/llm_infer.c` only into the Qwen3
  adapter inspection binary.
- `apps/mem_service` builds `/bin/linqu_mem_service` for core smoke/self-test,
  wire fixture validation, store/journal fixture validation, Unix-socket
  `serve`, optional `serve --store` metadata/ref snapshot+journal recovery,
  `serve --config` storage-root catalog layout and derived store recovery,
  `health`, `ready`,
  `status`, `list-records`,
  `put-object`, `get-object`,
  `register-prefix`, `lookup-prefix`, `publish-kv`, `resolve-kv`,
  `publish-runtime-handoff`, `resolve-runtime-handoff`,
  `register-execution-artifact`, `query-execution-artifact`,
  `register-training-artifact`, `query-training-artifact`,
  `wire-schema`, `wire-schema-fixtures`, `journal-fixtures`,
  `durable-catalog-fixtures`,
  `compat-old-new-matrix`, `compat-old-new-fixtures`, `admin-output-schema`,
  `admin-output-fixtures`, `api-abi-policy`, `api-abi-fixtures`,
  `package-manifest`, `package-fixtures`, `release-manifest`, and
  `release-fixtures`, and
  `/bin/linqu_mem_service_qwen3` for Qwen3 topology inspection.
- `apps/mem_service` also exposes `make install-smoke DESTDIR=<dir>
  PREFIX=/usr`, which installs the core daemon binary, public headers, client
  SDK source files, serving/pretraining SDK examples, release manifest, wire
  schema manifest, package manifest, admin output schema, config
  schema/example, the deploy runtime config copied to
  `etc/lingqu/mem_service/mem_service.conf`, and deployment manifest into an
  `installed-layout-v1` package layout. The runtime config source is
  `share/lingqu/mem_service/config/mem_service.runtime.conf`, separate from the
  `/tmp`-oriented example config, and defaults to `/run/lingqu` plus
  `/var/lib/lingqu/mem_service` paths. The host service has its own
  `share/lingqu/mem_service/config/mem_service.host.runtime.conf` copied to
  `etc/lingqu/mem_service/mem_service.host.conf`, using
  `/run/lingqu/mem_service_host.sock`,
  `/var/lib/lingqu/mem_service_host`, and metrics port `9901` so both systemd
  units can be active in the same Linux CI run. It also installs the service units into
  `usr/lib/systemd/system/` while keeping the same files under
  `share/lingqu/mem_service/deploy/` as checked deployment manifests.
  The same install step generates `usr/lib/pkgconfig/lingqu-mem-service.pc`
  with the active `PREFIX`, so package installs use `/usr` while local
  installs keep their chosen prefix in SDK discovery metadata.
  `installed-sdk-example-smoke` reuses that installed layout and compiles both
  SDK examples only from the installed include/source/example roots.
  `installed-sdk-pkgconfig-smoke` performs the same compile through
  `pkg-config --define-prefix`, which is the expected integration path for
  external serving/pretraining build systems.
  `installed-sdk-runtime-smoke` then starts the installed host daemon and runs
  those installed serving/pretraining clients against it over the public Unix
  socket API. The
  installed systemd units point at that `/etc` runtime config path and declare
  `RuntimeDirectory=lingqu` plus service-specific `StateDirectory` values, so
  tar/deb/rpm package smokes verify the units and config are present instead of
  relying on share-only example files.
- Guest app runners in `ub_sim` provide the CLI surface that exercises the
  component inside QEMU guests.
- `ub_sim`'s `run_app mem_service` runner runs the standalone metadata smoke
  path, wire fixture gate, wire schema fixture gate, store/journal fixture
  gates, compat fixture gates, and release fixture gate.
- `tests/test_mem_service_record_recycling.py` validates record capacity, recycling,
  KV payload sizing, and object-ref naming contracts.

## Productization Split Contract

### 受管理重放历史容量续作

现有 64 项内存幂等表及清理预留保持不变。容量续作采用独立于可压缩 journal 的
只追加重放历史，先实现内部 `mem_service_replay_history.h/.c` 与
`tests/mem_service_replay_history.c` 的 `--case <name> <new-history-path>`
边界夹具，再接入 daemon。
内部文件不属于安装 SDK。daemon 接入沿用 `serve --store`，只将受管理操作的
已完成结果移入历史；未配置 store 的服务继续使用原容量拒绝。旧 metadata
record retention 的行为保持不变，受管理历史不进入通用 record GC。

历史帧使用显式小端编码，包含序号、前帧校验、operation、request checksum、
status、精确 key/response 长度和字节；校验用于检测意外损坏，不提供对抗篡改认证。
查找须完整验证日志与调用方的已提交前缀，保留旧操作结果及冲突判据。
append 成功要求文件与创建目录项均已同步；不确定写入使句柄停止服务，不能
据此删除内存记录。损坏、截断、替换文件、并发写者及不完整末帧均拒绝，禁止
自动截掉历史。内存占用不随历史条数增长，磁盘容量仍受文件系统限制。

缓存回收顺序为历史 append/fsync、写入带前缀 checkpoint 的 snapshot 并同步
文件及目录、最后清除已归档缓存；任一步失败保留缓存并拒绝该次新工作。
历史启用后的磁盘快照使用独立 magic `mem_service_store_history_v1`，旧服务
须拒绝；原 v1 snapshot/wire 保持不变。历史服务的旧完整 snapshot 导出/恢复
入口拒绝，避免生成缺少外部历史的假完整备份。重启先校验历史，再导入剩余
缓存/journal，已归档应答不占用内存槽。无法配对的文件保留并拒绝启动。
当前接入属于待验证实现；历史结果不授予当前映射权限，provider/readiness/
generation 的重放前准入继续执行。managed backing 恢复、地址重用及真实百轮
仍须独立完成 drain/epoch/旧上下文验收。

managed checkpoint 重启时保留已有 recovery 标记，并隔离所有非终态分配及
仍带 holder/backing/descriptor/address/写入义务的终态记录。仅当所有分配已
完整回收、无 mapping 事务且原 recovery 标记为零时，允许接纳新分配；provider
目录不从 checkpoint 恢复，须重新注册才能满足对应 readiness。历史视图仍由
当前分配代际校验，载入历史视图不恢复 payload。`allocate-object`、
`acquire-object` 和 `publish-allocation` 的成功重放统一核对当前代际；已退役、
被替换或隔离的分配返回 `version_conflict / managed_generation_retired`，同时
保留历史记录，清理应答仍可精确重放。该行为覆盖内存缓存与磁盘重放历史。

`mem_service` is being split toward a product-grade Lingqu data service that can
run as a guest component and as a host-side service for streaming LLM inference
and LLM pre-training data paths.

Keep the implementation layers separated:

- Core metadata: key construction, record tables, prefix/KV metadata state,
  object-reference projection, and validation. This layer must not depend on
  QEMU, OBMM device files, or Qwen3-specific topology.
- Service API: versioned wire envelope, stable operation IDs, stable error
  codes, daemon lifecycle commands, lightweight client transport, typed C
  client wrappers, shared text key/value payload helpers, public operation
  payload schemas, and
  object/prefix/KV/runtime
  handoff/execution/training artifact
  request/response payloads plus SDK-level pretraining typed wrappers,
  including the fixed `training-step-commit` artifact kind used as the current
  minimal global-step committed marker. The
  lightweight client transport and typed client
  also expose explicit timeout plus opt-in max-attempts/backoff/timeout-retry
  controls for deployed callers, and mutating object/prefix/KV/runtime/
  execution/training RPCs accept optional `idempotency_key` values for safe
  replay of completed requests. The SDK examples configure retry/backoff
  explicitly and use idempotency keys for all mutating calls. The
  `wire-fixtures`
  CLI freezes the current envelope layout, operation/status IDs, checksum
  values, header initialization behavior, 24 canonical request payload
  fixtures, 24 real handler response fixtures for the current RPC surface, and
  the current request schema checks, including minimal in-process idempotency
  replay/conflict behavior.
  The `wire-schema` CLI emits the checked-in release schema manifest, and
  `wire-schema-fixtures` freezes its current length/checksum plus operation,
  field, and selector counts. The `admin-output-schema` CLI emits the
  checked-in admin output schema manifest, and `admin-output-fixtures` freezes
  its current length/checksum plus Prometheus metric prefix/type and
  fail-closed status fields. These are still text key/value schema manifests,
  not cross-version binary typed schemas.
  Runtime handoff, execution artifact, and training artifact query requests
  can now carry expected session, model, artifact kind, artifact id, version,
  and checksum bindings; mismatches fail closed with stable wire status codes.
  The `store-fixtures` and `journal-fixtures` CLI gates freeze the current
  minimum save/load path for committed metadata/object-ref records plus an
  append-only `<store>.journal` stream for completed idempotency records and
  bounded audit records.
  `serve --store <path>` uses that path for restart recovery, including
  cross-restart replay/conflict behavior for completed idempotency keys and
  retained audit events from both the snapshot and journal; it does not make
  Memory Service own durable payload bytes. Full `export-snapshot`/
  `restore-snapshot` carries idempotency and audit records when the snapshot
  fits in the wire payload; paged snapshot export remains record-only.
  The `status`, `list-records`,
  `metrics`, `audit-log`, `inspect-object`, `export-snapshot`,
  `export-snapshot-page`, `export-snapshot-to`, `restore-snapshot`, and
  `metrics-export` commands
  expose the current minimal admin surface, including page assembly and
  transactional paged restore for large snapshots plus fixed request-latency
  histogram, idempotency replay/conflict counters, and Prometheus text metrics
  export. The current `audit-log` RPC exposes a bounded retained ring for
  mutating and fail-closed operations, persisted in `--store`, `<store>.journal`,
  and full snapshots; `compat-matrix` exposes the current release compatibility
  rules for wire/schema, retry, idempotency, audit, snapshot, and journal
  behavior,
  `compat-baseline-v1` freezes the current old-v1-client to current-server
  baseline, and `compat-old-new-matrix` freezes a v1 old/new schema-profile
  matrix for all 24 operations. `api-abi-policy` freezes the current client
  API/ABI version, public record ABI size, and old/new/upgrade/rollback policy.
  `admin-output-schema` freezes the current minimal admin output contract for
  deployed collectors and admin clients.
  `upgrade-rollback-policy` freezes the current-version-only admission rule,
  required release gates, old-server runtime binary compatibility, and
  cross-version upgrade certification.
  `deployment-fixtures` validates the current
  systemd-like service manifest and `/metrics` Prometheus HTTP response
  envelope, and `alert-rules` emits the checked-in Prometheus alert rules for
  service-down, error, fail-closed, checksum-mismatch, and latency signals.
  `alert-fixtures` freezes the current alert rule count, length, and checksum.
  `alert-integration-fixtures` checks those rules against the current exported
  Prometheus text/http metric names with a synthetic `/metrics` payload; it is
  not a real Prometheus/Alertmanager deployment smoke.
  `ops-certification-policy` is the fail-closed production-ops admission
  contract: real Linux systemd, Prometheus/Alertmanager, rpm packaging, and
  deployment upgrade/rollback remain `not-certified` until external Linux CI
  supplies those named gates through the checked `ops-certification-evidence-v1`
  file generated by `ops-certification-generate-evidence` or the one-command
  `ops-certification-linux-ci-smoke` gate, then verified by
  `ops-certification-verify --evidence-file`.
  The install/package contract now includes the default runtime config at
  `etc/lingqu/mem_service/mem_service.conf`; it is copied from the checked
  `configs/mem_service.runtime.conf` deploy config, while
  `configs/mem_service.example.conf` remains the developer `/tmp` example. The
  host unit uses `configs/mem_service.host.runtime.conf`, copied to
  `etc/lingqu/mem_service/mem_service.host.conf`, with a distinct socket,
  state directory, and metrics port from the main unit. Both release and
  package manifests list the service and host runtime config sources. The
  package layout also installs
  `lib/systemd/system/linqu_mem_service.service` and
  `lib/systemd/system/linqu_mem_service.host.service` as the enableable unit
  files, with the share/deploy copies retained as contract manifests. The
  systemd unit files declare `RuntimeDirectory=lingqu` and service-specific
  `StateDirectory` values so Linux systemd owns the default `/run/lingqu`
  socket directory and `/var/lib/lingqu/*` state directories.
  `serve --metrics-listen tcp:<ipv4>:<port>` exposes the real HTTP scrape
  listener covered by runtime tests. The portable service-manager lifecycle
  smoke covers config startup, ready/health, HTTP scrape, collector metrics
  parse, SIGTERM stop, and socket cleanup. `storage_root` now creates the current durable
  catalog layout (`catalog/manifest.txt`, `blocks/`, `quarantine/`) and derives
  `catalog/store.snapshot` when `store` is omitted; `sealed-local-block-v1`
  writes inline and server-side `payload_path` payloads into
  `blocks/<checksum>.block` and verifies them on read. `sealed-chunked-block-v1`
  writes large payloads into chunk directories and fail-closed quarantines
  corrupt chunks. `transport-loopback-block-v1` writes payloads under
  `remote-blocks/<checksum>.transport/` with a transport manifest, verifies
  through that backend on read, and fail-closed quarantines corrupt payloads.
  `transport-tcp-block-v1` fetches payloads from `tcp:<ipv4>:<port>` into
  `remote-blocks/<checksum>.tcp/`, then validates and quarantines through the
  same sealed transport contract. Object records now treat the default
  OBMM-backed runtime pool and `ub-ssd-gsva-v1` as peer backend choices.
  GVA/GSVA describes how mem_service exposes and moves caller buffers; it is
  not owned by either backend. `ub-ssd-gsva-v1` is a first-class object backend
  reference for payloads committed into the simulated UB SSD through the GSVA
  data path. Metadata-only `put-object --backend ub-ssd-gsva-v1`
  records the SSD device CNA, owner node, block hi/lo, block version, byte
  range, and checksum in the mem_service object record; `get-object`,
  `inspect-object`, snapshot export, and store restart preserve that
  reference. Data-plane `put-object --backend ub-ssd-gsva-v1 --backend-write 1`
  accepts an explicit `--backend-buffer-*` GSVA descriptor and submits
  `SSD_OP_BLOCK_WRITE` to `/dev/ub_ssd0` or `--backend-device-path`; successful
  SSD completion owns the stored block version, bytes, and checksum.
  Data-plane `get-object --backend-read 1` uses the same explicit GSVA buffer
  descriptor shape and submits `SSD_OP_BLOCK_READ` into the caller-provided
  destination buffer. Missing devices or unsupported ioctl paths fail closed as
  `status=unsupported`, and incomplete descriptors fail as
  `status=invalid_session`. The remaining integration gap is automatic
  descriptor sourcing from llm_infer/runtime buffer metadata so serving code
  does not have to pass raw GSVA fields manually. The release contract records
  `remote_payload_network_transport=tcp-loopback-certified` and
  `remote_payload_network_transport_gate=network-transport-block-fixtures`;
  `make network-transport-block-smoke` is the explicit Make entrypoint for that
  socket-using gate.
  Cross-machine production network transport remains fail-closed as
  `remote_payload_production_network_transport=not-certified` until an external
  evidence file passes `remote-transport-verify --evidence-file <path>`.
  `remote-transport-generate-evidence --source tcp:<ipv4>:<port>
  --producer-host <host> --consumer-host <host> --network-partition-marker
  <path> --evidence-file <path>` is the consumer-side evidence generator: it
  fetches the producer payload through `transport-tcp-block-v1`, validates the
  local sealed copy, corrupts it to prove fail-closed quarantine, rejects
  loopback sources, and requires a CI-produced network-partition marker.
  `scripts/run_mem_service_remote_transport_ci.sh` is the reusable wrapper for
  that cross-host run; it builds `linqu_mem_service_host` if needed, runs the
  generator, and re-runs `remote-transport-verify --evidence-file <path>` on
  the generated artifact, then creates and verifies
  `linqu-mem-service-remote-transport-bundle.tar`.
  `scripts/run_mem_service_remote_transport_ci.sh --preflight` checks the app
  directory, partition marker, non-loopback TCP source, producer/consumer host
  separation, and `make` availability before running the cross-host probe.
  After CI publishes the evidence and bundle artifacts,
  `scripts/verify_mem_service_remote_transport_evidence.sh --evidence-file
  <path>` rebuilds/locates `linqu_mem_service_host` and re-runs
  `remote-transport-verify --evidence-file <path>` without needing the producer
  payload source again. `remote-transport-certification-bundle` packages the
  verified evidence, release manifest, package manifest, and a bundle manifest
  into `linqu-mem-service-remote-transport-bundle.tar`; after publication,
  `remote-transport-certification-bundle-verify REMOTE_TRANSPORT_BUNDLE=<path>`
  or `scripts/verify_mem_service_remote_transport_bundle.sh --bundle-file
  <path>` extracts the bundle safely, checks the manifest contract, and verifies
  the embedded evidence again. A release that publishes both Linux ops and
  remote transport certification bundles can be checked with
  `release-certification-verify` or
  `scripts/verify_mem_service_release_certification.sh --ops-bundle-file <path>
  --remote-transport-bundle-file <path>`; that command replays both bundle
  verifiers, then calls `release-readiness --ops-evidence-file
  --remote-transport-evidence-file` on the extracted evidence and fails closed
  unless the final readiness report reaches `overall_status=certified`. The
  `remote-transport-evidence-fixtures` gate defines
  the required evidence schema: non-loopback source address, cross-host
  topology, `transport-tcp-block-v1`, TCP/IPv4, payload round-trip, checksum
  validation, corruption fail-closed behavior, distinct producer/consumer
  hosts, and network-partition fail-closed behavior.
  Real systemd environment smoke, production collector/alert environment
  integration smoke, and cross-machine remote transport-backed block storage
  remain deployment work. Product restore admission is now guarded by
  `restore-policy-fixtures`: full snapshot and paged snapshot restore use
  transactional staging, bad magic, out-of-order pages, record-count mismatch,
  and cancelled staged commits fail closed, and live state remains unchanged
  until a valid commit succeeds. Artifact query payload ownership is now an
  explicit opt-in contract: serving and pretraining clients
  can send `expected_owner`, and the daemon rejects owner mismatches with
  `invalid_model_binding` through the existing fail-closed query path. The
  release contract records this as
  `payload_ownership_scope=artifact-query-expected-owner`, with
  `serving-fail-closed-fixtures` and `pretraining-fail-closed-fixtures` as the
  evidence gate.
  This layer must stay model-neutral and
  callable by external
  serving/pretraining processes.
- Release/deployment: the current `version`, `version-fixtures`,
  `release-readiness`, `release-readiness-fixtures`, `release-manifest`,
  `wire-schema`,
  `admin-output-schema`, `upgrade-rollback-policy`, `package-manifest`,
  `api-abi-policy`,
  `compat-matrix`, `compat-old-new-fixtures`, `ops-certification-policy`,
  `config-fixtures`, `install-smoke`, and `installed-sdk-example-smoke`
  surfaces prove the minimum publishable layout for the daemon binary, public
  headers, client SDK sources, SDK examples, release manifest, wire schema
  manifest, installed binary self-description contract, admin output schema
  artifact, upgrade/rollback policy artifact,
  API/ABI policy artifact,
  compatibility matrix, v1 compatibility baseline, old/new schema-profile
  matrix, config schema/example, systemd-like deployment
  manifest, host daemon artifact under `libexec`, host service manifest,
  deployment fixture, `host-artifact-smoke`,
  `installed-host-service-manager-smoke`, Prometheus text metrics export format,
  Prometheus metric prefix/type contract, `metrics_listen` config, `/metrics`
  scrape path contract, collector scrape contract, loopback-only TCP metrics
  listener contract, local-only `auth_mode=none` config boundary,
  Prometheus alert rules artifact, synthetic alert integration,
  production-ops certification policy,
  fixture, installed-layout-v1 package contract, service-manager lifecycle contract, durable catalog layout contract,
  current-version-only upgrade/rollback gate, and explicit client
  retry policy.
  They are not yet a real systemd environment smoke,
  production collector/alert environment integration gate, or real deployment
  upgrade/rollback smoke; tar/deb packaging and old-server runtime-binary
  compatibility are certified by in-tree gates, and rpm packaging has a
  checked spec plus `package-rpm-smoke` gate that requires a Linux rpm
  toolchain.
- Transport/runtime: OBMM pool mapping, queue descriptors, cluster bootstrap,
  and guest handoff timing. This layer can depend on guest runtime facilities.
- Model adapters: Qwen3 range/KV/engram placement and payload sizing. New model
  families must be added as adapters rather than renaming or specializing the
  service core. Model-specific retention and recycling policies belong here.
- Deployment apps: guest CLI/app entrypoints and future host daemon entrypoints.
  They should consume the same core APIs and expose explicit command-line
  validation surfaces.

New code should move device-independent logic out of `mem_service.c` first,
then split transport and model adapters behind explicit headers. Do not add new
W4/W5-named public APIs to `mem_service`; W5 is a workload family, not the
service boundary.
