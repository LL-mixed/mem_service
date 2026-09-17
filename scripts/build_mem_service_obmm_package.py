#!/usr/bin/env python3
"""Build and verify a source-free Memory Service OBMM deployment package."""

import argparse
import hashlib
import io
import pathlib
import tarfile


FORMAT = "linqu-mem-service-obmm-package-v1"
CORE_PATH = "usr/bin/linqu_mem_service"
PROVIDER_PATH = "usr/libexec/lingqu/mem_service/linqu_mem_service_provider_obmm"
CONFIG_PATH = "etc/lingqu/mem_service/providers/obmm/worker.conf"
MANIFEST_PATH = "usr/share/lingqu/mem_service/obmm-package-manifest.txt"
EXPECTED_PATHS = {CORE_PATH, PROVIDER_PATH, CONFIG_PATH, MANIFEST_PATH}
CONFIG_KEYS = {
    "connect",
    "node_id",
    "state_file",
    "incarnation",
    "readiness_generation",
    "allocation_granularity_bytes",
    "fast_allocation",
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def clean_value(name: str, value: str) -> str:
    if not value or "\n" in value or "\r" in value or "=" in value:
        raise ValueError(f"invalid {name}")
    return value


def parse_kv(data: bytes, label: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for raw in data.decode("utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ValueError(f"invalid {label} line: {raw}")
        key, value = line.split("=", 1)
        if not key or key in result:
            raise ValueError(f"duplicate or empty {label} key: {key}")
        result[key] = value
    return result


def add_bytes(archive: tarfile.TarFile, name: str, data: bytes, mode: int) -> None:
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    info.mtime = 0
    archive.addfile(info, io.BytesIO(data))


def read_regular_file(path: pathlib.Path, label: str) -> bytes:
    if not path.is_file() or path.is_symlink():
        raise ValueError(f"{label} must be a regular file: {path}")
    return path.read_bytes()


def build(args: argparse.Namespace) -> int:
    core = read_regular_file(args.core, "core binary")
    provider = read_regular_file(args.provider, "provider binary")
    config = read_regular_file(args.config, "provider config")
    if not core or not provider:
        raise ValueError("package binaries must be non-empty")
    config_values = parse_kv(config, "provider config")
    if set(config_values) != CONFIG_KEYS:
        missing = sorted(CONFIG_KEYS - set(config_values))
        extra = sorted(set(config_values) - CONFIG_KEYS)
        raise ValueError(f"provider config keys mismatch missing={missing} extra={extra}")

    fields = {
        "package_format": FORMAT,
        "service_revision": clean_value("service revision", args.service_revision),
        "provider": "obmm",
        "provider_binary": PROVIDER_PATH,
        "provider_binary_sha256": sha256(provider),
        "core_binary": CORE_PATH,
        "core_binary_sha256": sha256(core),
        "provider_config": CONFIG_PATH,
        "provider_config_sha256": sha256(config),
        "platform_linkage": "static",
        "platform_revision": clean_value("platform revision", args.platform_revision),
        "obmm_uapi_version": clean_value("OBMM UAPI version", args.uapi_version),
        "source_repo_required": "0",
        "infer_required": "0",
        "model_required": "0",
    }
    manifest = "".join(f"{key}={value}\n" for key, value in fields.items()).encode()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.output, "w", format=tarfile.PAX_FORMAT) as archive:
        add_bytes(archive, CORE_PATH, core, 0o755)
        add_bytes(archive, PROVIDER_PATH, provider, 0o755)
        add_bytes(archive, CONFIG_PATH, config, 0o640)
        add_bytes(archive, MANIFEST_PATH, manifest, 0o644)
    print(
        f"status=pass action=build format={FORMAT} output={args.output} "
        f"sha256={sha256(args.output.read_bytes())}"
    )
    return 0


def verify(args: argparse.Namespace) -> int:
    package = read_regular_file(args.package, "package")
    with tarfile.open(fileobj=io.BytesIO(package), mode="r:") as archive:
        members = archive.getmembers()
        names = [member.name for member in members]
        if len(names) != len(set(names)) or set(names) != EXPECTED_PATHS:
            raise ValueError(f"unexpected package members: {sorted(names)}")
        if any(not member.isfile() for member in members):
            raise ValueError("package may contain only regular files")
        payload = {
            member.name: archive.extractfile(member).read()
            for member in members
        }
    manifest = parse_kv(payload[MANIFEST_PATH], "manifest")
    required = {
        "package_format": FORMAT,
        "provider": "obmm",
        "provider_binary": PROVIDER_PATH,
        "core_binary": CORE_PATH,
        "provider_config": CONFIG_PATH,
        "platform_linkage": "static",
        "source_repo_required": "0",
        "infer_required": "0",
        "model_required": "0",
    }
    for key, expected in required.items():
        if manifest.get(key) != expected:
            raise ValueError(f"manifest {key} mismatch")
    for key in ("service_revision", "platform_revision", "obmm_uapi_version"):
        clean_value(key, manifest.get(key, ""))
    hashes = {
        "core_binary_sha256": sha256(payload[CORE_PATH]),
        "provider_binary_sha256": sha256(payload[PROVIDER_PATH]),
        "provider_config_sha256": sha256(payload[CONFIG_PATH]),
    }
    for key, expected in hashes.items():
        if manifest.get(key) != expected:
            raise ValueError(f"manifest {key} mismatch")
    config_values = parse_kv(payload[CONFIG_PATH], "provider config")
    if set(config_values) != CONFIG_KEYS:
        raise ValueError("provider config schema mismatch")
    print(
        f"status=pass action=verify format={FORMAT} package={args.package} "
        f"sha256={sha256(package)} service_revision={manifest['service_revision']} "
        f"platform_revision={manifest['platform_revision']} "
        f"obmm_uapi_version={manifest['obmm_uapi_version']}"
    )
    return 0


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)
    create = commands.add_parser("build", help="build a deterministic OBMM package")
    create.add_argument("--core", required=True, type=pathlib.Path)
    create.add_argument("--provider", required=True, type=pathlib.Path)
    create.add_argument("--config", required=True, type=pathlib.Path)
    create.add_argument("--output", required=True, type=pathlib.Path)
    create.add_argument("--service-revision", required=True)
    create.add_argument("--platform-revision", required=True)
    create.add_argument("--uapi-version", required=True)
    create.set_defaults(handler=build)
    check = commands.add_parser("verify", help="verify package layout and hashes")
    check.add_argument("--package", required=True, type=pathlib.Path)
    check.set_defaults(handler=verify)
    return root


def main() -> int:
    args = parser().parse_args()
    try:
        return args.handler(args)
    except (OSError, UnicodeError, ValueError, tarfile.TarError) as error:
        print(f"status=fail reason={error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
