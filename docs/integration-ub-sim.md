# ub_sim 使用侧适配手册

本文面向 `ub_sim`（qemu+UB 多节点 PP 模拟器）的维护者：说明 ub_sim 如何以
**源码方式**消费本仓库（`mem_service`），如何构建 guest 二进制并启动多节点
qemu+UB PP 运行。`ds4` 的安装态 SDK 消费方式见
[integration-ds4.md](integration-ds4.md)。

> 路径约定：`<mem_service>` 指本仓库检出根目录，`<ub_sim>` 指 ub_sim 检出
> 根目录。ub_sim 侧的脚本均位于 `<ub_sim>/guest-linux/aarch64/` 下。

## 1. 消费契约：`MEM_SERVICE_ROOT`

worker 日志 v2 要求平台版本化 segment 枚举接口，并在分配前持久保存内核
实例身份。SDK 的 vendored UAPI 同步平台对应声明，必须匹配支持该 ioctl 的
内核及 gva_manager。旧日志不自动升级；新增 `reconcile-allocation-state`
只读比较资源身份，不解除隔离或恢复 worker，业务 SDK/wire 不变。

OBMM worker 的 state_file 改为版本化完整资源记录，每个阶段同步对象/provider
身份、segment、export 回执及 descriptor。新增 provider CLI
`inspect-allocation-state --config <path>`，只读核对完整日志及原配置身份；
损坏、半写、旧格式或并发 writer 拒绝。结果明确 physical_state=unknown，
不能据此恢复数据准入或删除日志。既有 state_file 仍阻止 worker 直接重启。
该改动要求重建 managed worker，业务 SDK/wire 与 DS4 transfer 路径不变。

managed store V2 持久化完整 allocation、holder、holder 的精确节点/incarnation
归属、mapping、代际及 V2 绑定，恢复后保留隔离资源并报告
managed_recovery_required。V1 checkpoint 仍可加载，其未绑定 holder 按保守规则
处理；原 wire snapshot 导出/覆盖不能替代该恢复域。写入不确定后停止状态变更，
provider/kernel 对账仍须实际验证。core 消费者必须重新链接，安装 SDK 新增可选
node-aware acquire API；本段不表示故障恢复矩阵已经验收。

运行期 provider 失联会隔离该 home 的未终结分配及精确归属于该 incarnation 的
holder 所引用分配；旧版未绑定 holder 继续在任一 provider 失联时保守隔离。
保留所有映射与引用，锁存 managed_recovery_required。
重新注册不解除隔离。查询和已有 mapping 清理仍可执行，release/retire 不再被目录
readiness 拦住，但仍检查原代际与事务。该 core 源码变更不修改 wire 或结构布局，
须重新链接；完整持久化对账与实际 guest 故障恢复仍须独立验证。

服务恢复协调使用 `recover-allocation --key <key> --node-id <home>
--incarnation <current> --generation <g> --fenced-incarnation <old>
--backing-gone <0|1>`，安装 SDK 对应
`mem_service_client_recover_allocation()`。服务要求当前 home 活动、原 home 身份
完全匹配、全部 required provider ready，并逐个核对 holder 节点已以新
incarnation 加入。原 home 未替换时只进入 RETIRING，仍由 worker 执行正常
unexport/segment retire/reclaim；原 home 已替换时要求 replacement 明确确认旧
backing 不存在，随后才直接退役。旧 holder 身份、V1/损坏或写入不确定的恢复域
继续锁住准入。该接口只落实服务状态机；ub_sim 的 OBMM worker 仍须提供 kernel
库存、旧实例失效及映射/计算排空证明后才能调用，当前不能据此宣布生产恢复完成。

V2 writer 的 `object-session` 顺序为 acquire、`begin_reference key=<allocation>
generation=<g> version=<old-version> idempotency_key=<id>`、map/write、
`publish_reference key=<logical> offset=<n> len=<n> kind=<n> owner=<n> producer=<n>
idempotency_key=<id>`、unmap、`seal_reference key=<allocation> generation=<g>
version=<new-version> idempotency_key=<id>`、release。版本值显式指定，避免丢失应答
后将一次重试变成另一次换版。publish_reference 使用 SDK 的
`mem_service_client_prepare_managed_reference()`，从 begin 回执和实际映射字节
生成 V2，确认 provider publish 后登记。CLI 保留每个完整请求用于原样重试；
同一发布 ID 改变视图参数会拒绝。首次准备成功后该 payload 禁止再写，未确认 stage
禁止 seal。多个视图应先全部写好，再逐项发布。此命令序列不需要手填 reference_hex。
loopback 的匿名 payload 不能证明跨进程数据共享；真实 guest 的读写闭环另行验收。

V2 reader 新增 `mem_service_client_map_managed_reference()` 与配套 unmap；需要
故障恢复归属的 guest 使用对应 `_at_node` 入口，并在 map 与不确定清理重试中
传入同一 `(holder_node_id, provider_incarnation)`。
使用独立 reference lifecycle 保留引用和不确定的 BEGIN。客户端先 resolve、
acquire，再发起只读映射；服务在 `map-begin` 中核对完整登记引用、封存版本、
实际 holder 和活动 home。映射采用 allocation-relative 子区间，原 raw 入口
仍拒绝封存 V2。`object-session` 提供 `acquire_reference key=<logical-key>
idempotency_key=<id>`、`map_reference key=<allocation-key>`；后续读、可见性、
unmap/release 沿用既有操作。该接入不改变 gitlink/lock 发布要求，也不构成
实际 guest 的 V2 发布/读取或 W5 验收证明。

严格 OBMM 固定地址映射允许非零 allocation-relative offset：中立 provider
请求的 requested_address 必须等于 backing 基址加 offset，返回精确视图 base/len。
整个 backing 地址空间继续保留，视图外完整页不可访问，子页边界仍由 SDK 检查。
此 provider 改动不开放已封存 V2 对象的 raw mapping BEGIN；reader 必须等待
服务与 SDK 的完整引用准入接通。文件映射边界夹具不替代实际 OBMM guest 验收。

V2 的服务 core 增加 begin/stage/seal、resolve/acquire 元数据状态机，使用现有
record table 的专用 managed-view kind。写入前推进 content version；封存后按
登记的完整视图及当前 allocation 核对引用。core record/allocation 内存结构新增
字段，不能混用不同版本的 core 编译产物；旧 V1 投影明确拒绝 managed-view。
V2 的 wire/SDK 入口已显式接入，实际 guest 与 W5 接入仍待验证；ub_sim
不得直接调用 core 绕过统一 SDK 或消费未提交、未锁定的子仓库源码。

全局引用新增 opt-in 的 256-byte V2 编码，位于既有公开头
`lingqu_object_service.h`，不增加链接依赖。V1 布局和版本常量不变，旧消费者
必须继续拒绝 V2。V2 offset 相对 allocation，禁止把旧 arena offset 直接升级。
完整 key、generation、home/incarnation、逻辑大小和权限用于后续权威绑定核对；
解码成功不授予 holder，不证明数据版本或 payload checksum。ub_sim 须在服务端
绑定及 SDK acquire/map 接通后显式采用，当前默认消费者不变。

V2 writer 在 stage 后可调用 `mem_service_mapping_owner_unmap`：它排空访问并
解除映射，保留原 holder 以满足 SEAL 的唯一 owner 要求。调用失败时保留句柄
重试；成功后禁止新 borrow，SEAL 仍须使用原 version 和幂等操作身份。最后
使用 close/release/destroy 结束生命周期，不能把 unmap 当作完成内容发布。

AM2 的统一访问 owner 采用新增中立 `mem_service_mapping_owner.h/.c`，显式链接
该源文件及 `-pthread`。同一 provider channel 的多个 owner 共用 access domain；
平台 setup/teardown 与域内运行分阶段，运行中不绕过 domain 调用 raw provider。
成功 adopt 转移当前 mapping/lifecycle 及同一 holder，CPU/compute borrow 保持
原映射。close 停止新借用，全部访问退出后才解除映射并释放 holder；失败保留句柄。
最终 destroy 要求所有调用线程已退出。业务仍通过 SDK/compute adapter 使用对象，
不解析 provider 身份。此为待实现的接入契约，实际并发能力以对应 guest 证据为准。

PTO 平台 adapter 可通过 OBMM provider 的 mapping pin 接口借用当前 SDK 严格
import 视图。pin 保留原 VMA/import/fd，provider 在 pin 存在时拒绝解除映射，
PTO registration 确认注销后方可释放 pin。接口仅供平台适配层使用，模型不解析
provider 字段；要求与 endpoint 操作串行化。home export 视图、legacy mapping
和非 OBMM binding 明确拒绝，未提供隐式 alias。中立 SDK/wire 布局保持不变，
实际 managed CPU/PTO 联合验证仍由 ub_sim 完成。

OBMM 平台诊断 `mem_service_provider_obmm_endpoint_probe_descriptor()` 及
`object-session` 的 `op=probe_descriptor key=<key>` 从当前可读映射的 handle
构造 24 组必定无效的 descriptor 副本，直接检查 provider map 拒绝及资源不变。
调用须与 endpoint 操作串行；CLI 要求有效 holder 和当前映射。它不接受外部
descriptor，不改变业务 SDK/wire，不提供动态身份轮换验收。非 OBMM endpoint
返回 UNSUPPORTED，不能用 loopback fixture 计作实际 OBMM 通过。

daemon 的幂等表为已接纳的对象、holder 和 mapping 预留后续清理应答容量。
新工作可能在物理表尚有空位时返回 `CAPACITY_EXCEEDED`；调用方应完成已有生命周期，
不能以新 operation ID 无限重试。有效 teardown 使用预留容量，无效 teardown
不能消耗该容量。只读 `allocation-stats` 提供 `idempotency_capacity`、
`idempotency_used`、`idempotency_cleanup_reserved`、`idempotency_available`、
`idempotency_reservation_deficit`。这些附加文本字段不改变既有 client record ABI；
旧 SDK 可忽略。已有 managed identity 时 snapshot restore 被拒绝，以保留重放历史。
此接纳保护不提供无限幂等历史、重启恢复或资源强制撤销。

容量续作通过既有 `serve --store` 接入内部重放历史。服务在归档应答及含前缀
checkpoint 的磁盘快照都确认同步后才回收缓存；无 store 时保留原有容量拒绝。
缓存归档通过内部有界批量接口完整校验历史一次，预检所有冲突后追加并同步；
失败保留原缓存和旧 checkpoint，半写历史不自动修剪。wire/SDK、磁盘帧格式
及请求截止时间不变，实际 guest 延迟改善需独立运行验证。
历史启用后的 store 使用新 magic，旧二进制不能回滚读取；完整备份需同时保留
store 与其 `.replay-history` 文件，原 wire snapshot 导出/恢复不支持该组合。
此项不证明 managed backing 跨重启恢复。ub_sim 必须在独立服务测试/提交/发布
后，同时更新 gitlink、lock 及直接链接 daemon 的构建源清单，才能进行 guest 验收。
历史 store 重启后控制服务可供查询，`managed_recovery_required=1` 且
`data_plane_ready=0`；在 AM3 资源对账接通前不接纳新 managed 数据工作，避免
将空的运行期 allocation 表或历史成功应答解释成有效 backing。

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

诊断 `op=unmap key=<key> probe_unmapped=1` 在当前可读映射仍有效时读取首字节，
保存原地址；确认 managed unmap 完成后，在同一进程重新读取该地址，要求目标
地址发生同步 SIGSEGV/SIGBUS。仅解除映射成功且实际故障被捕获才通过；清理
失败时不执行探针，保留原清理归属。未指定选项时原 unmap 行为不变。loopback
结果只证明 CLI 探针机制，真实 OBMM 必须独立运行。该探针覆盖解除映射后、
新映射建立前的 CPU 访问，不证明同址新映射建立后的裸指针隔离或旧 token 拒绝。

旧 descriptor 实跑使用同一 session 的 `capture_mapping key=<old>` 和
`probe_retired_mapping key=<old>`：先在完整 allocation 映射中校验数据并保存
快照，再确认旧代退役、新代同址映射的数据正确，解除新映射但保持其 holder。
探针检查两代状态后将保存的原 descriptor 直接交给 provider，要求 CPU 读故障
及清理成功；普通 map 失败不算通过。调用方随后重新映射并校验新对象。探针
拒绝手工地址/descriptor、V2 子视图及覆盖快照，清理失败保留 binding 并停止
后续操作。该 CLI 诊断不改变受管理 SDK 的正常准入检查。

受管理 map 在 BEGIN 事务创建前，通过既有只读 inspect-allocation
查询核对服务端当前绑定：key/generation、ACTIVE、home/incarnation、size/alignment、
capabilities、provider-backed、地址/范围和完整不透明 descriptor。任一绑定不匹配
返回 STALE_REF；查询失败直接返回错误，均不创建 pending mapping 或幂等记录。
BEGIN 随后检查 generation、ACTIVE 和 holder，同代 ACTIVE 绑定不可改写；查询后
退役/换代不会使旧快照获准映射。BEGIN 后的不确定结果继续保留清理上下文。
live_refs、holder 数量等可变统计不参与绑定比较。每次建立映射增加一次元数据
查询，CPU 正常 load/store 不增加 RPC；raw mapping 兼容入口仍由平台调用方负责
提供有效引用，不获得此受管理绑定核对保证。该补齐不替代 AM3 的撤销与重用协议。

诊断配置可使用 `op=map key=<key> fault=<name> expect_status=stale_ref`。
它只改动当前已 acquire 视图的临时副本，原缓存与服务记录保持不变；该副本经同一
managed SDK 处理。`fault=descriptor fault_byte=<N>` 翻转指定字节，默认 N=0；
其他名称为 `descriptor_length`、`descriptor_oversize`、`address`、`address_len`、
`size`、`alignment`、`capabilities`、`home`、`incarnation`。未知名称、其他期望状态
或对非 descriptor 使用 fault_byte 均拒绝配置；字节不在实际 descriptor 内时诊断
失败。调用方不能通过此接口指定替代地址或 descriptor 内容。验收必须继续执行
无 fault 的真实映射与读写，不能仅根据预期拒绝判断对象可用性。

诊断操作 `op=probe_conflict key=<key>` 要求当前 session 已 acquire 且持有可读映射。
OBMM 路径复用该映射已持有的设备句柄，经 provider 的同一严格映射实现执行
`MAP_FIXED_NOREPLACE`；不再次 import，不注册重复 GSVA 路由。只有实际 mmap
返回 EEXIST、探针没有未完成清理且原映射首个逻辑页的 checksum 保持不变才返回
OK。若 mmap 意外创建了 VMA，只清理新创建的范围；清理失败时保留范围、停止
接收新工作，并在资源统计中计入，原句柄要等探针 VMA 清理确认后才能关闭。
session-loopback 仅用重复 managed SDK 请求验证 fixture，不能证明 OBMM 行为。
原映射仍由后续显式操作解除；缺少有效 holder/映射时拒绝探针。
GSVA aperture 拒绝普通匿名映射，此探针不尝试在 aperture 中创建匿名页。
真实 OBMM 验收还须核对 provider 的 `obmm-map` mmap 失败记录、errno=EEXIST、
随后完整对象数据校验和资源回收。平台诊断入口
`mem_service_provider_obmm_endpoint_probe_conflict()` 只接受当前 endpoint 的映射
handle，须与 endpoint 操作串行化；它不新增业务 SDK API 或 wire opcode，
不作为 DS4 serving 的调用入口，也不接受调用方提供的地址或 descriptor。

取消 ALLOCATING 对象会进入 RETIRING，等待 home 清理确认；迟到 publish 只登记
待清理 reservation，不重新开放 acquire。当前 worker 重启遇到已有 state 文件时
拒绝运行，异常资源保留待核对。正常生命周期的地址复用交给内核空闲区间
分配器，worker 不再自行推进地址游标。消费平台必须完成 managed V2 unmap
的完整 CPU/PTO 排空、fence/route/TLB 清理及 holder 终结，并支持分配 ioctl
的 ENOSPC 无资源契约；其他不确定结果继续隔离。真实同址重用/旧身份拒绝
须单独验收，该接入不构成恢复或强制撤销完成声明。

地址管理接入新增 `mem_service_client_poll_allocation()` 和诊断命令
`poll-allocation --node-id <id> --incarnation <u64> --after-generation <u64>`。
它只返回绑定到该 provider 代际的待办元数据，按 generation 扫描，无任务返回
NOT_FOUND。provider 必须按对象 key/generation 核对已有 reservation，再经已有
publish/reclaim 确认；查询不提供独占领取保证。此新增接口尚不表示 OBMM provider
已自动执行这些任务。服务 wire 版本保持 1，新增 operation 为 `0x7c`；旧服务会
拒绝未知操作，消费者不得静默退回手工 descriptor 发布流程。

多 home 部署通过 `allocate-object --home-node <node_id>` 或
`mem_service_client_allocate_object_at_home()` 为每个新对象指定中立 home 节点；
省略时继续使用 daemon 的 `allocation_home_provider` 默认值。服务只接纳 provider
目录中仍处于 active 的节点，并把当前 incarnation 固定到 allocation；同一对象
不能在重放时切换 home。每个 home 仍须运行自己的 `serve-allocations` worker，
部署还必须给各 home 提供不重叠的实际地址分配权威。仅有 per-request placement
不能证明跨 home GSVA 区间不会冲突。

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

### 受管理 V2 引用控制接口（显式接入）

新增 `mem_service_client_reference_transition()` 与 CLI
`reference-transition`，使用 `0x7e` metadata-only RPC。调用者将
`mem_service_reference_request` 清零后填写 action-specific 字段；固定字符串
要求 NUL 终止并保持尾部零填充。`begin` / `seal` 的 key 是 allocation key；
`stage` / `resolve` 的 key 是逻辑对象 key；`acquire` 的 key 必须与
`reference_hex` 内完整 allocation key 相同。V1 消费者不会自动升级。

有序流程为：allocation owner 持有唯一 holder → `begin`（传当前 version，
成功后递增）→ 完成 provider 写入与可见性操作 → `stage` 一个或多个 V2 引用
→ `seal` → reader `resolve` / `acquire` → 现有 `release-object`。引用 metadata
不携带 payload；`seal` 的元数据成功回执不能单独证明设备写入完成。

BEGIN / STAGE / SEAL / ACQUIRE 必须携带 idempotency key。当前 readiness、
generation、content version、home incarnation 检查发生在缓存回执返回前。
开始新版本后，旧版 resolve/acquire 会失败；provider 实例更换后，旧成功回执
也不能获得当前引用。BEGIN 为首次 STAGE 和 SEAL 预留回执容量，ACQUIRE
为 RELEASE 预留容量；通用 record retention 不回收受管理引用。

本接口当前覆盖 daemon 生命周期内的控制协议及 reader SDK 映射编排。
实际跨 guest provider 可见性闭环、W5 V2 数据链路和跨 daemon 重启验证仍需完成；
不能将上述 metadata 测试计入这些验收项。下游消费仍遵守 clean commit、
远端可获取、gitlink 与 lock 一致的原有要求。

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

服务完整排空后，managed checkpoint 可在无 recovery 标记、无资源或 mapping
义务时重新接纳新代际；provider 目录仍须重新建立。`allocate-object`、
`acquire-object`、`publish-allocation` 的成功历史应答若指向已退役或替换的
代际，返回 `version_conflict`，reason 为 `managed_generation_retired`。
调用方须创建新的操作身份，不能依赖旧成功应答重新取得资源。未排空 checkpoint
仍保持隔离；此变更不恢复 worker 或旧映射，不替代实际 kernel/fencing 验收。
