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
- Source revision: `d9492ed29eb84bc47f6016323118b18c6ea6cd8c`
- Checked GSVA export v1 adds a fixed-width 120-byte request and a separate
  ioctl. The copied headers match this tested kernel revision. Kernels that
  lack the new ioctl cannot provide managed-worker allocation receipts.
- Segment enumeration v1 adds a 144-byte request with kernel-instance identity,
  revision-bound cursor and active/retired export-resource records. Worker
  ledger v2 requires this interface before creating its first persistent frame.
- `kernel_ub/include/uapi/ub/gsva.h`
  - SHA-256:
    `65a58c319f284e920ce5fdaab5b7d695a1dfec037ececaeb85b69f896f6e42ab`
- `kernel_ub/include/uapi/ub/obmm.h`
  - SHA-256:
    `a39b3cf927c7a2c3a385dd44a39b2cb880075aa53090bb0b85ee01108be801b9`

The aggregate directory checksum is calculated from files sorted by name.
Each input line is `<sha256><two spaces><repository-relative path><newline>`.
Checksums are informational provenance; the downstream build and runtime tests
remain the compatibility gate.
