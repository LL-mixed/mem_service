# Vendored dependency provenance

The files listed here are copied into this repository because `mem_service`
must build independently from `ub_sim`. They do not evolve independently.
Any update must preserve the source revision, refresh the recorded checksum,
and run the full standalone and downstream contract tests.

## ub_sim sources

- Source repository: `ub_sim`
- Source revision: `ef391e7a9590cc7bb0ee71869932bee2d6e6ef3c`
- `common/obmm_common.h`
  - SHA-256:
    `1bcb8627b9712c274256e63b0abe20f51ebad8db2bbbeea089b602f7fd62dc2c`
  - Standalone adjustment: include paths point at repository-root vendored
    headers instead of the former `guest-linux/aarch64` layout, and the
    `libobmm` dependency resolves via the `vendor/obmm` submodule and the
    vendored `kernel_ub` headers.  libobmm is consumed via the
    `vendor/obmm` submodule pinned to the same upstream revision
    `53011eed10716b422d2ac29199f68b55f7c5bdc5` used by ub_sim.
- `libs/obmm_queue/`
  - Aggregate SHA-256:
    `7cc88a0541d8061ac683be923c17fcb730f140bf58cd1cb76e80aa0fd057051a`

## kernel_ub UAPI sources

- Source repository: `ub_sim` submodule `guest-linux/kernel_ub`
- Source revision: `d7898f5e0b682d5af7ff76de9a8592c717f544b7`
- Checked GSVA export v1 adds a fixed-width 120-byte request and a separate
  ioctl. The copied headers match this tested kernel revision. Kernels that
  lack the new ioctl cannot provide managed-worker allocation receipts.
- `kernel_ub/include/uapi/ub/gsva.h`
  - SHA-256:
    `eddda83cb2d3e6da65ce7afa81bcb29466115428cd5dc50a4fd0b5aa8676f8a6`
- `kernel_ub/include/uapi/ub/obmm.h`
  - SHA-256:
    `cbf21ea39a4f6256379d656d68f1bf4158077cc73314bf7a11332aabb82218f9`

The aggregate directory checksum is calculated from files sorted by name.
Each input line is `<sha256><two spaces><repository-relative path><newline>`.
Checksums are informational provenance; the downstream build and runtime tests
remain the compatibility gate.
