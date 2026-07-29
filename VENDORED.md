# Vendored dependency provenance

The files listed here are copied into this repository because `mem_service`
must build independently from `ub_sim`. They do not evolve independently.
Any update must preserve the source revision, refresh the recorded checksum,
and run the full standalone and downstream contract tests.

## ub_sim sources

- Source repository: `ub_sim`
- Source revision: `734e24388eaa2c6d1cde07073f69abcf95947b9c`
- `common/obmm_common.h`
  - SHA-256:
    `95b9645e04e685039e68d49cefcf5c1d62ee17d029c1c58d1d3433d9c03e4635`
  - Standalone adjustment: include paths point at repository-root vendored
    headers instead of the former `guest-linux/aarch64` layout.
- `libs/obmm_queue/`
  - Aggregate SHA-256:
    `7cc88a0541d8061ac683be923c17fcb730f140bf58cd1cb76e80aa0fd057051a`

## kernel_ub UAPI sources

- Source repository: `ub_sim` submodule `guest-linux/kernel_ub`
- Source revision: `92d6c59c9b1612e39b46d57b59ad8c3a318e6f78`
- `kernel_ub/include/uapi/ub/gsva.h`
  - SHA-256:
    `8986cec72ff5d252b729c44b746fcb6252cfe6d27e9d23e1e2a1a76190045236`
- `kernel_ub/include/uapi/ub/obmm.h`
  - SHA-256:
    `50b1b9b081a030345bcc692a264c9593fa288dd30335b1370db354aa3421960c`

The aggregate directory checksum is calculated from files sorted by name.
Each input line is `<sha256><two spaces><repository-relative path><newline>`.
Checksums are informational provenance; the downstream build and runtime tests
remain the compatibility gate.
