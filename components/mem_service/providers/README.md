# Memory Service Provider Layout

This directory contains transport and storage providers for `mem_service`.
Providers implement the neutral contract from the parent directory; they do
not define object identity, KV semantics, placement policy, wire operations,
or service readiness.

## Layout

- One provider uses `mem_service_provider_<name>.c` and
  `mem_service_provider_<name>.h`.
- A provider-specific executable entry point uses
  `mem_service_provider_<name>_cli.c` and builds as
  `linqu_mem_service_provider_<name>`. Its CLI is diagnostic and operational;
  applications still use the transport-neutral mem service SDK.
- Provider-private tests use the same basename with `_test` before the file
  extension. Cross-provider conformance tests stay in the repository-root
  `tests/` directory (for example
  `tests/mem_service_tcp_provider_conformance.c`).
- Shared provider helpers are allowed only after two providers need the same
  mechanism. They use the `mem_service_provider_common_*` prefix.
- Provider-backed daemon configuration is a strict text contract consumed by
  the provider CLI. Checked-in examples live under
  `apps/mem_service/configs/providers/<name>/`; machine-local deployment
  instances stay outside the repository.

## Boundaries

- Provider headers may include vendor or platform APIs. Core files must never
  include provider headers.
- Providers receive opaque region and transfer requests through the neutral
  contract. Provider-specific connection keys remain inside the opaque
  descriptor.
- Providers may register capabilities and topology costs. They may not alter
  object metadata or choose model policy.
- A provider must fail closed when it cannot prove region ownership, bounds,
  completion, version, or checksum.
- Build targets opt in to providers explicitly. Adding a source file here must
  not make every `mem_service` binary link that provider.
- The installed source SDK keeps neutral `sdk_sources` free of provider
  dependencies. Provider consumers query
  `payload_provider_<name>_sources` and `payload_provider_<name>_libs` from
  `lingqu-mem-service.pc` and opt in explicitly.
- A provider probe may report device availability, but service data-plane
  readiness requires a completed provider-specific peer canary and checksum
  validation: transfer completion for transfer providers, or mapped-range
  visibility for mapping providers.
- Provider control traffic may exchange opaque descriptors and completions.
  Application payload bytes must use the provider data plane.
- Peer mapping and peer transfer are independent data-plane capabilities.
  A mapping provider exposes a bounded process mapping plus explicit publish,
  invalidate, and visibility operations; it must not emulate mapping by routing
  bytes through a transfer provider. A transfer provider exposes submitted
  copies and completions; it must not claim that a completed transfer created a
  process mapping.
- OBMM remote mappings use the SIM_DEC/GVA/GSVA mapping path and are independent
  of URMA. The OBMM provider owns OBMM export/import, mmap lifecycle, cache
  maintenance, and range visibility. The cluster queue and object protocols
  remain above the provider contract.
- A process may register only memory that it owns or has explicitly mapped.
  Consequently, a model runtime uses the neutral provider SDK in the model
  process for hot-path buffers. A separate daemon remains the control plane
  and must not claim zero-copy ownership of another process's heap.
- Applications exchange only the neutral serialized region descriptor. They
  do not parse provider bytes or include provider headers.
- A data-plane channel binds only when the complete configured data-plane
  registry is ready. A healthy edge cannot hide a missing full-mesh peer.
- Connection-oriented providers expose a two-phase server lifecycle:
  `listen` first makes the endpoint reachable, and `accept` completes the
  peer connection only after the application control plane has announced
  readiness. The compatibility `endpoint_open(..., server=true)` operation
  performs both phases for standalone canaries.
- Provider verification and region registration happen only after both peers
  have entered the connection phase. Applications must not use timing delays
  to hide a listener/connect race.

## OBMM Functional Conformance

### Worker 持久资源记录

日志 v3 在首次资源操作前通过平台枚举接口取得 16-byte kernel_instance，
并在所有帧中保存同一值。v3 还在定长帧保留区保存 endpoint 与 state path 的长度、
双摘要及分配模式，供控制面隔离核对启动配置。零身份、跨帧身份变化及旧 v1 格式
均拒绝；v2 缺少配置绑定，仍可由 inspect/resume/recovery 读取，但
`quarantine-allocation-state` 必须失败关闭。旧日志不能凭当前配置或内核补写出生
身份。平台不支持枚举或返回无效结果时，worker 在创建日志和刷新 provider 前停止。
`reconcile-allocation-state --config <path>` 只读关联完整日志与一致内核库存，
按对象 generation 保留最后记录及最后已知资源身份，逐项比较完整 segment
与当前 export 绑定。缺失、不同内核实例、忙碌或身份冲突均拒绝；库存中未被
日志认领的 segment 单独计数，禁止擅自回收。该阶段不修改 service/kernel，
不恢复 worker、不提供 fencing 证明，成功匹配仍保持 reconciliation_required。

`recover-allocation-state --config <old> --replacement-config <new>` 实现
lost-home 的保守恢复。两份配置必须保持 endpoint、node、state file、粒度及分配
模式一致，replacement 必须使用新 incarnation。命令独占并完整校验旧 ledger，
要求当前 kernel instance 与出生身份不同，读取稳定的完整库存，并拒绝新库存复用
任一旧 segment ID。replacement provider 必须已经登记且整个 required provider
目录 ready；随后命令通过 recovery poll 枚举旧 incarnation 的全部 QUARANTINED
义务，逐项调用 `mem_service_client_recover_allocation()`，并确认 ledger 中每个
当前对象均已退役。全部成功后，旧 ledger 原子重命名为
`.recovered-<old>-by-<new>` 并同步目录；中途失败保留原 ledger，成功后的重试从
归档重新验证服务终态。只有归档完成后才能使用 replacement 配置启动新 worker。

该命令只覆盖 home guest/kernel 已替换、旧 backing 随旧 kernel 消失的恢复。
相同 kernel instance、旧 segment 身份复用、活动 ledger writer、不完整服务恢复域
或 holder fencing 未完成均拒绝。`reconcile-allocation-state` 继续保持只读。

`fence-holder-state --config <old> --replacement-config <new>` 在 holder 节点执行
imported remote holder 的物理撤销。两份配置必须绑定相同 endpoint、node、state
file、粒度和分配模式，
replacement 使用新 incarnation 且已经登记，完整 required provider directory 必须
ready。命令通过 holder recovery poll 按 generation 枚举精确旧 incarnation 的
QUARANTINED holder，逐项解析服务保存的严格 GSVA descriptor，并向本机 `/dev/obmm`
提交 `LOCAL_REVOKE`。QEMU 完成 route quarantine、在途访问排空、home fence/
writeback、cache/TLB 失效、CPU window unmap 和 tombstone 后才返回成功。随后命令
使用确定性幂等键调用 `mem_service_client_fence_allocation_holder()`。物理撤销失败、
descriptor 不完整、服务回复冲突或 receipt 不完整时立即停止；物理失败路径绝不
提交 receipt。重试通过 QEMU tombstone 和服务持久 receipt 幂等收敛。

`fence-terminated-holder-state --config <old> --replacement-config <new>
--terminated-kernel-instance <32-hex>` 只覆盖外部编排器已明确确认旧 guest/QEMU
终止后的 holder 恢复。调用者提供被终止实例的 kernel identity；该参数本身不构成
终止证明。命令要求 replacement incarnation 已登记且 required provider directory
ready，当前 kernel identity 与被终止实例不同，两次完整 inventory 均稳定且为空。
随后对每个旧 holder 的严格 descriptor 提交本地撤销检查：只接受 replacement 中
精确 route 不存在，或该 route 当场完成完整 `LOCAL_REVOKE`；其他结果不提交
receipt。每笔 receipt 前再次核对同一 kernel inventory revision。旧 guest 是否已经
退出必须由平台 supervisor 的进程/VM 终止回执证明；网络 lease 到期、provider
替换或调用者提供一个不同的随机 identity 均不足以调用本命令。

`prepare-holder-rejoin --config <old> --replacement-config <new>` 在上述 fencing
完成后为 replacement worker 准备同一路径重启。它只接受恰好包含一个完整
`worker-start` 帧的旧 ledger；任何 home allocation 帧、损坏、截断或活动 writer
均失败关闭。replacement provider 必须已经登记且 required provider directory
ready，旧 incarnation 的 home recovery 与 holder recovery poll 还必须同时为空。
全部条件满足后，命令把旧 ledger 原子归档为
`.holder-fenced-<old>-by-<new>` 并同步目录；成功重试只重新验证归档和控制面终态。
归档后才能用 replacement 配置和原 state path 启动新的 `serve-allocations`。
旧 ledger 含本地资源时必须走 `resume-allocations` 或
`recover-allocation-state`，禁止使用该命令跳过资源对账。

`recover-allocation-state` 对旧 HOME holder 使用不同的物理证明：新 kernel instance
的两次稳定 inventory 均不存在旧 ledger 记录的 segment，并且拒绝任何旧 segment
身份复用。在该证明成立后，命令先用确定性幂等键提交旧 HOME holder receipt，再
请求服务 reclaim。服务仍有任一 remote holder 时 reclaim 会拒绝，必须由对应节点
先完成上述 `fence-holder-state`。

`resume-allocations --config <path>` 覆盖同一 kernel instance 中稳态 worker 崩溃
后的 reservation 续作。它独占并完整校验现有 ledger，读取稳定 kernel inventory，
先对全部对象完成只读预检和库存快照复核，再执行可证明的阶段续作：

- `published` 在 segment/export/descriptor 与服务 ACTIVE/RETIRING 身份完全一致时
  恢复 slot；
- `reserved`、带完整 segment 的 `reserve-unknown`、`export-unknown` 与 `exported`
  根据库存是否存在 export，继续 checked export 或 generation-bound publish；
- `release-intent`、`unexported` 与 `retired` 根据 export/segment 的实际终态继续
  unexport、retire 和服务 reclaim；
- `export-no-backing`、`unbacked-retire-intent`、`unbacked-retired`、`reserve-empty`、
  `capacity-reject-intent` 与 `cancel-empty` 完成物理 retire 和未发布对象取消；
- `reclaimed` 与 `cancel-confirmed` 只核对终态，不恢复 slot。

每个外部操作的完成证明先同步为新 ledger 帧；失败后的下一次启动从新阶段继续。
`reserve-intent` 未记录 segment 身份，无法把任一库存项唯一绑定到该事务，继续
fail-closed。截断、校验失败或半写帧同样拒绝，禁止删除尾部或推断操作未发生。
成功后进程持有原 ledger 锁并进入现有 poll/reclaim 循环。远端 holder 的排空由
上述 `fence-holder-state` 独立执行，worker resume 不代替该物理证明。

`quarantine-allocation-state --config <path>` 用于上述无法安全续作的状态。命令
独占现有 ledger，要求首个 `worker-start` 帧完整且与 endpoint、state file、node、
incarnation、粒度及分配模式全部一致。后续帧完整时逐项校验；截断或校验失败只
记录为 `ledger_integrity=torn|invalid`，不据此推断事务结果。命令精确注销首帧
绑定的 provider incarnation，使服务立即执行 provider-loss 隔离；旧实例已经过期
或被新 incarnation 替换时按幂等成功处理。它不打开 `/dev/obmm`、不修改 ledger、
不解释或回收 segment，也不解除任何隔离。输出固定声明
`scope=provider-control-plane`、`backing_reconciled=0` 和
`resource_reconciliation_required=1`；物理资源仍须后续 reconciliation、fencing
或 lost-home recovery 处理。首帧损坏、配置不匹配、活动 writer 或控制面结果不确定
时拒绝执行。

`serve-allocations` 的 state_file 使用版本化、固定长度、字段级小端记录。
每个阶段保存 node/incarnation、对象 key/generation、逻辑尺寸及对齐、完整
segment 身份、实际 export 回执和已构造的 opaque descriptor；不序列化指针。
记录带严格递增序号、零保留区和校验和，每条记录同步后才进入下一资源操作。
写入或同步不确定后停止 worker，保留原文件；不得删除半写尾部或推断资源为空。
旧文本日志继续拒绝作为可恢复记录。stdout 的原阶段行保持兼容。

`inspect-allocation-state --config <path>` 只读校验完整日志，核对配置中的原
node/incarnation 和粒度，逐条展示资源身份；损坏、截断、并发写入及格式不匹配
返回失败。该命令不登记 provider、不打开设备、不恢复指针或解除隔离；
`physical_state=unknown` 明确表示日志校验尚未完成 kernel/provider 对账。
普通 `serve-allocations` 继续拒绝已有 state_file；只有上述显式 resume 命令可以在
完整三方身份核对后续作。lost-home 恢复成功会归档旧 state file，之后只能用具有
新 incarnation 的 replacement 配置创建新的 ledger。

worker 对 provider refresh 和 allocation poll 的 wire `TIMEOUT` 保持原
incarnation、ledger 与物理资源并重试，避免控制面短时不可调度被误判为 provider
进程崩溃。服务端 lease 仍是失联上限；超过 lease 后，后续 refresh 会返回 provider
身份失效，worker 按原 fail-closed 路径停止并保留 ledger。非超时传输错误、服务端
拒绝和 incarnation 冲突不重试。

### 固定地址子区间

缓存刷新必须保持当前视图的权限：`UPDATE_RANGE` 对 READ 视图使用 READONLY，
对可写视图使用 READWRITE，不能从 backing 的较宽权限推导访问权。刷新页范围
仍从实际 view offset 和请求范围计算，检查失败不能报告可见。`--cached-visibility`
无设备夹具核对只读 home 刷新、可写刷新、页对齐与 ioctl 失败；实际 guest 的
只读发布结果消费须独立验收。

严格 GSVA 的固定地址请求以视图首字节为 requested_address，必须精确等于
descriptor 的 remote_uba 加 offset；offset 与 len 继续受完整 backing 边界约束。
provider 保留完整地址 reservation，仅开放覆盖视图的页，并返回精确的视图
base/len。视图前后的完整页保持 PROT_NONE；同页内剩余字节由 SDK 范围检查约束。
旧非严格 descriptor 的固定地址非零 offset 请求仍拒绝，禁止隐式回退。

此能力只解决 provider 映射几何，不授予 V2 holder 或证明当前 content version。
服务与 SDK 必须另行完成已登记引用的映射准入。无设备边界夹具通过生产中立
channel 调用 provider，并使用真实文件共享映射核对非零偏移、只读保护、边界
拒绝和清理；文件替身结果不能计作实际 OBMM guest 的共享可见性认证。
定向命令为 OBMM provider test executable 的 `--fixed-subrange`，完整 Python
入口沿用 `tests.test_mem_service_obmm_provider`。

### Compute attachment ownership

平台 compute adapter 可从当前 SDK mapping binding 取得不透明 pin。provider
核对实际 ops/context、handle、可访问视图和读写权限；只接纳已建立 managed route
的严格 GSVA 映射，包括远端 import 和本机 home export。pin 借用现有 control fd、
mem_id 和逻辑视图，不新建 import、不复制 payload。这些字段仅交给平台 adapter，
业务代码继续使用统一 SDK/memref。legacy mapping 返回 `-EOPNOTSUPP`，不合成 PA
或隐式导入。本机 home 路径依赖相同 managed GSVA 生命周期已经登记的 home route；
缺少 strict descriptor、route 或真实 VMA 身份时，后续 strict PTO 注册失败关闭。

pin 存在时，provider unmap 在改变任何 VMA/fd/视图归属前返回 `-EBUSY`。
endpoint close 保留上下文并停止新接纳，已有 pin 仍可释放；所有 pin 释放后才可
重试实际 unmap/close。adapter 必须先确认 PTO registration 注销，再释放 pin。
调用方将 pin acquire/release 与 endpoint 的其他操作串行化；本接口不宣称
provider 或 SDK 现有单 owner 结构已支持并发，不替代服务 holder 和 mapping 事务。
已有 resources_v1、mapping binding、wire 布局保持不变。

`object-session` 的 `op=probe_descriptor key=<key>` 诊断从当前可读映射的
provider handle 取得真实 descriptor，向同一 provider map 实现逐项提交
24 组格式、必填身份字段、对齐和溢出负例，不接受用户提供的 descriptor。
探针要求 endpoint 可用且仍有空闲 slot，避免把容量拒绝误计作格式校验。
每项必须在 handle 分配前失败，保留原映射及 endpoint 资源计数；异常结果
将 endpoint 标记为 closing，已取得资源继续由原有 cleanup 路径持有。
此操作与 endpoint 的其他操作串行化，不发送有效 map 请求，不创建服务映射
事务，也不改变 holder。CLI 还核对原映射首个逻辑页的 checksum，实际验收须
继续完整读写及最终 kernel 资源核对。该负例集不验证合法格式的旧身份、
动态 token 轮换或 consumer kernel 的拒绝时点。

endpoint 资源快照按当前进程持有的 export/import handle 与实际 VMA 归属计量，
独立于服务端对象/映射事务计数。分别报告 export/import 字节、VMA 保留字节、
可访问视图字节及待清理映射；失败清理的剩余资源继续计入。快照调用须与该
endpoint 的 map/unmap/close 串行化，关闭中仍允许查询，已关闭的 endpoint
不能伪造空快照。此接口覆盖当前 endpoint，其他进程、孤儿资源及重启后的
kernel 全量枚举由恢复阶段核对。诊断 CLI 通过 object-session 的 stats 输出。

严格映射的部分失败按每个实际创建的 VMA 记录归属；回滚仅解除这些范围，保留
解除失败的范围、import ID 和 slot。失败结果的 handle 仅用于重试清理，任何范围
访问均被拒绝。确认所有 VMA 已解除后才关闭映射 fd，再执行 unimport；slot 在
全部完成后才可复用。close 错误保留隔离状态，禁止盲目重试可能已复用的 fd。
`endpoint_close_checked()` 返回清理结果；失败保留 endpoint 上下文并拒绝新的
映射/注册，旧 void 关闭入口保留兼容并报告失败。此契约需通过故障注入与真实
guest 验证；跨进程崩溃恢复仍由恢复阶段实现。

`serve-allocations --config <file>` is the managed-allocation worker, built
with `GVA_MANAGER_ROOT` pointing to the shared platform library directory.
Its strict config contains `connect`, `node_id`, `incarnation`,
`readiness_generation`, `state_file`, and `allocation_granularity_bytes`.
Optional `fast_allocation=0|1` defaults to 0. When enabled, the checked export
uses only the kernel's already-cleared cached pool pages without slow-path expansion;
pool pressure can therefore reject an allocation even when other system RAM
is available. This provider setting is useful for bounded pool validation.
Crash validation may add both `fault_injection_phase=<phase>` and
`fault_injection_mode=sync-exit|torn-exit`. The pair is optional and incomplete,
duplicated, or unknown values are rejected. Supported phases are
`reserve-intent`, `reserved`, `exported`, `published`, `release-intent`,
`unexported`, `retired`, and `reclaimed`; they cover the durable boundaries
after allocation intent, address reservation, backing export, service publish,
release intent, physical unexport, segment retirement, and service reclaim.
`sync-exit` fsyncs the complete selected phase frame and exits with code 86
before the next operation. `torn-exit` fsyncs exactly half of that frame and
exits with the same code, so the preceding complete frame remains the last
trusted state. At `reserve-intent`, both exits still happen before the allocation
ioctl. All modes bypass normal deregistration so that recovery tests must use
the explicit resume or quarantine path and preserve any ambiguous allocation as
reconciliation-required. This validation-only control has no default.
The granularity must match the deployed OBMM pool profile (2097152 for the
2 MiB guest profile); it is a power of two, at least the system page size.
Logical object sizes remain unchanged. Backing and address reservations are
rounded up to this granularity, while SDK operations remain bounded by the
logical size. Physical capacity statistics include the rounded reservation.
For strict GSVA mappings, the SDK requests the logical view while retaining
the complete backing descriptor. Whole pages outside that view are protected
with `PROT_NONE` in separately created VMAs; no `mprotect` or VMA splitting
is used, since OBMM forbids both. Padding within the final page remains SDK-bounds-checked.
The diagnostic `object-session` operations `probe_readonly` and `probe_guard`
require synchronous CPU protection faults in the mapping-owning process.
OBMM mappings are `VM_DONTCOPY`, so a child-process fault is insufficient
evidence. A backing without a full padding page reports guard unsupported.
It requires an existing aperture
and active, canary-verified provider registration from infrastructure bootstrap;
it does not create readiness by registering itself. It refreshes that same
registration, polls its bound work, allocates/exports through gva_manager and
publishes through the SDK. No inference process participates.

The worker's normal-release path retains each reservation by key, generation,
provider incarnation and the exact published descriptor. RETIRING work is
eligible only with zero holders. It must durably record release intent, finish
unexport and kernel segment retirement, and then confirm reclaim. Failed or
uncertain steps stop processing and retain state for reconciliation. This
path relies on clients unmapping before releasing their holders; forced
revocation and recovery require separate validation.

地址复用由 kernel 区间分配器确认。worker 按 aperture 的 `(node_id,
node_count)` 将全局地址范围切成互斥 node slice，使用服务生成的全局 allocation
generation 选择首个对齐候选，再以精确 `requested_home_va` 循环探测该 slice。
只有 `EBUSY` 且 descriptor 全零时才允许尝试下一个候选；worker 不维护第二份
空闲表或单调游标，最终占用和复用结果仍以内核为准。该规则允许非连续地址，
同时避免不同 home provider 独立执行 kernel first-fit 时选中同一 GSVA。匹配的
平台必须在 managed unmap 时停止新访问、等待完整 CPU/PTO 调用退出、完成
fence/route/TLB 清理；SDK 确认全部映射终结后才允许 holder release。worker
确认 unexport、segment retire 和 service reclaim 后继续分配；同址新对象必须
使用内核返回的新 segment/token 和服务 generation。任何不确定清理均停止
worker，保留状态；已有 state 文件仍禁止重启，不提供跨重启重用或强制撤销。
此接入须经实际 guest 复用/旧身份测试验收。

Capacity rejection before the first kernel allocation uses the existing
generation-checked retire and home reclaim confirmations. The worker records
`capacity-reject-intent`, cancels the unpublished request, confirms that no
reservation exists, and continues serving. A rounded-size overflow, aligned
address overflow, or a request that cannot fit anywhere in the aperture is a deterministic
capacity rejection. The rejected allocation becomes RETIRED, with no
descriptor or address; `inspect-allocation`/`object-session wait_state` expose
that terminal state, and the worker reports `reason=address_capacity`.
运行期区间耗尽只接受平台 `gva_manager_allocate_segment()` 的特定契约：
直接 ALLOC_SEGMENT ioctl 返回 ENOSPC，且零初始化的 descriptor 仍全零，
证明本次未创建 segment。worker 先同步 `reserve-empty`，再清除本次意图并
执行上述取消确认。匹配内核仅在区间保留前返回 ENOSPC；成功保留后的输出
丢失返回 EFAULT。此规则不泛化到 export、其他 errno、非零 descriptor 或
其他 ioctl；这些情况仍须核对，禁止推断为空。取消应答不确定也停止 worker。

Managed backing uses the platform's checked GSVA export v1 ioctl. A successful
NO_BACKING receipt is distinct from an ioctl error: the kernel confirms that
the pool/sgtable attempt left no export backing. The worker records this
receipt, retires its known segment, and only then cancels and confirms the
unpublished service object (`reason=backing_allocation`). Only confirmed segment
retirement makes the interval available to the kernel allocator again.
Failed retirement or cancellation retains the reservation.
Old kernels reject the new ioctl; the worker never retries ordinary export.
The kernel also pins the segment during checked export and while its export
handle exists, preventing early segment retirement. These changes require a
matching platform build and their own actual guest validation.

Cancelling an unpublished allocation leaves it RETIRING until its bound home
worker confirms cleanup. A late publish attaches the reservation to that
RETIRING identity without making it acquirable. A running worker with no
reservation may confirm an empty cancellation; restarting with an existing
state file remains forbidden, so absence from a new process is never accepted
as evidence of cleanup.

Before reserving resources it
creates and durably records an exclusive state file. Existing state prevents
restart until reconciliation is implemented; do not delete it while resources
may remain live. Failed or uncertain operations retain resources and stop the
worker. Normal release does not certify crash recovery or standalone
bootstrap. One home allocator is required per address domain until global
multi-home coordination is connected.

OBMM descriptor v2 extends the 48-byte v1 prefix to 96 bytes. It retains
the export token separately from the GSVA segment token, and carries the
segment ID, epoch, flags, owner, node count, cache policy, p_tag and access
rights. Core continues treating the entire descriptor as opaque. Strict
descriptors require mappings at their UBA, reject incompatible requested
addresses and permissions, and use the GSVA import operation. v1 remains
explicitly supported for existing ordinary OBMM objects; failed v2 decoding
or import must never retry as v1. Descriptor construction is not proof of
allocation ownership or readiness; the home worker must supply actual kernel
allocation/export results and retain lifecycle ownership.

### 受管理 GSVA 导入的版本要求

严格描述符使用 simulator 私有 import v4：GSVA lease token 和物理 export
token 分别传入内核；`0x8` 导入标志要求新内核显式支持，旧内核按未知标志拒绝。
内核使用新增 `SIM_DEC_OP_GSVA_MAP_V2`（`0x0f`）传给 QEMU，旧映射 opcode、
长度和 token 语义保留。失败不重试旧版导入。该路径要求匹配的平台组件；隔离
开发快照已通过 64 KiB 双 guest 双向访问和回收，正式发布认证仍待完成。

endpoint 打开时从 sysfs memory windows 枚举最多 64 个对齐的本机 import PA
候选。首批候选保持原有按窗口顺序的放置，备用候选按窗口轮询加入，因此可覆盖
多个不连续 window。远端 import 遇到明确的 `EEXIST` 地址冲突时自动尝试下一个
未被当前 endpoint 使用的候选；其他错误立即失败关闭，所有候选冲突时报告池耗尽。
该候选池只处理本机 import backing PA，与 GSVA 地址分配权威相互独立。

受管理导入的正常 unimport 使用既有 UNMAP opcode 的 version=2：仅释放本地
视图，完成 fence、删除 coherence/route 和刷新 TLB，不留下对象退役 tombstone。
实际 Retire 事件继续保留 tombstone。旧 version=1 保持原退役语义；旧 QEMU
拒绝 version=2，内核不得转入 legacy unmap。2026-09-11 的 ub_sim r20 开发组合
已通过两个 guest 的同代重复映射、页保护与双向共享回收；正式子仓库版本尚未发布。

GSVA CPU window 使用对应 acquire/fence 操作，不调用 legacy shadow-window
的 `SYNC_IMPORT_RANGE`。定向测试核对两种 token、旧端拒绝标志、可见性顺序、
ioctl/error/checksum 失败以及无效范围在触发操作前被拒绝；测试不访问设备：

```sh
python3 -m unittest tests.test_mem_service_obmm_provider.MemServiceObmmProviderTest.test_gsva_import_and_visibility_boundaries
```

`tests/mem_service_obmm_provider_conformance.c` is the authoritative OBMM
functional test. It runs inside at least two QEMU guests with `/dev/obmm`; a
host protocol fixture or Linux cross-compile is not functional evidence. The
test must observe degraded readiness before the peer canary, exchange only the
opaque neutral region descriptor through the provider control plane, then
reach ready state and exercise register, export, map, publish, invalidate,
wait-visible, unmap, and deregister through the neutral channel API. Corrupt
descriptors, out-of-bounds mappings, and checksum mismatches must fail closed.
The runner must also retain QEMU SIM_DEC mapping evidence and verify that no
guest reports a kernel fault.

## RoCE Mesh Configuration

`linqu_mem_service_provider_roce mesh-serve --config <path>` accepts a strict
line-oriented file. Unknown, duplicate, malformed, or incomplete fields fail
closed. The fields are:

- `version=1`;
- one local `listen=unix:<path>`;
- optional `store`, `storage_root`, and loopback-only `metrics_listen`;
- `verify_bytes`, `verify_iterations`, and `timeout_ms`;
- one or more
  `endpoint=<server|client>,<local-ip>,<peer-ip>,<port>,<device>` entries.

Every configured endpoint must complete a checked peer transfer before the
daemon starts accepting SDK requests.

## TCP Data-Plane Provider

`linqu_mem_service_provider_tcp` is the persistent TCP implementation of the
same neutral peer-transfer contract. It is an explicitly selected provider,
never an automatic fallback for RoCE or another failed provider.

The server and client canary commands are:

```text
linqu_mem_service_provider_tcp server-canary \
  --local-ip <ip> --peer-ip <ip> --port <port>
linqu_mem_service_provider_tcp client-canary \
  --local-ip <ip> --peer-ip <ip> --port <port>
```

Both commands also accept `--bytes`, `--iterations`, and `--timeout-ms`.
`protocol-fixtures` checks descriptor versioning and corruption rejection.
The real canary registers process-owned memory, transfers payload bytes,
waits for the receiver's checksum-bearing completion, and reports throughput.

The connection uses `TCP_NODELAY` and stays open across transfers. The sender
returns completion only after the receiver has copied the complete payload
into the registered region and validated its checksum. The provider also
advertises the neutral receive-fence capability: after an application sends
control metadata, the receiver may wait on the target registered slice and
continue as soon as that exact payload and checksum are ready. The sender's
strict completion and ACK semantics remain unchanged, while the ACK return
trip can overlap receiver work.

Initial providers are expected to be:

- a deterministic loopback provider for contract tests;
- an OBMM peer-mapping provider for QEMU eight-node PP;
- a separate UB/URMA peer-transfer provider where explicit transfer is used;
- a RoCE full-mesh provider for DGX PP;
- a TCP data-plane provider that is never an automatic fallback.
