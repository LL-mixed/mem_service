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

`serve-allocations --config <file>` is the managed-allocation worker, built
with `GVA_MANAGER_ROOT` pointing to the shared platform library directory.
Its strict config contains `connect`, `node_id`, `incarnation`,
`readiness_generation`, `state_file`, and `allocation_granularity_bytes`.
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
revocation and recovery require separate validation. Released addresses are
not reused during a worker session.

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
