import shutil
import socket
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT
SERVICE_DIR = ROOT / "components" / "mem_service"
CLI_SOURCE = ROOT / "apps" / "mem_service" / "mem_service.c"

HOME_NODE = "node-a"
HOME_INCARNATION = 7


def _tmp_parent() -> Path:
    private_tmp = Path("/private/tmp")
    if private_tmp.exists():
        return private_tmp
    return Path(tempfile.gettempdir())


def _free_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _parse_kv(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in text.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
    return values


# Strict same-VA target for loopback mappings: page-aligned, outside the
# default heap/stack/dyld ranges on macOS arm64 and inside the 39-bit
# user space of Linux aarch64, so the anonymous mmap hint is honored
# exactly on both (probed on macOS: 16 GB hints are stable across runs).
DATA_MAP_ADDRESS = 0x400000000
DATA_MAP_ADDRESS_DEC = str(DATA_MAP_ADDRESS)
DATA_MAP_LEN = 16384


def _pattern(seed: int, length: int) -> bytes:
    return bytes(((seed + i) & 0xFF) for i in range(length))


def _fnv1a64(data: bytes) -> int:
    """Same FNV-1a-64 as mem_service_provider_checksum64."""
    checksum = 1469598103934665603
    for byte in data:
        checksum ^= byte
        checksum = (checksum * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return checksum


@unittest.skipUnless(shutil.which("cc"), "host cc is required")
class MemServiceObjectSessionTests(unittest.TestCase):
    """M1.2 object-session CLI behavior (plan §3.7).

    Real processes per test: the daemon holds the managed table and
    provider directory, and `object-session --config` CLI processes run
    deterministic producer/consumer/provider sequences through the client
    SDK. Covered end to end here: the three-session allocation
    choreography over unix and TCP transports, wait_state coordination
    and bounded timeout, expect_status negative paths, inspect field
    assertions, stats reporting, provider-gating fail-closed behavior,
    and config validation. The M1.3 data plane is covered through the
    in-process session-loopback provider: strict same-VA map, seeded
    write/read pattern checks with FNV-1a checksums, the
    acquire→map→unmap→release ordering rules, end-of-session leak
    detection, and provider-kind validation (provider=obmm is rejected
    on this host build, which lacks MEM_SERVICE_OBJECT_SESSION_OBMM).
    """

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="msvc_objsess_", dir=str(_tmp_parent())))
        self.binary = self.root / "linqu_mem_service"
        self.socket = self.root / "session.sock"
        self._compile_host_binary()

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def _compile_host_binary(self):
        cmd = [
            "cc",
            "-O2",
            "-Wall",
            "-Wextra",
            f"-I{ROOT}",
            f"-I{ROOT / 'libs' / 'obmm_queue'}",
            str(CLI_SOURCE),
            str(SERVICE_DIR / "mem_service_daemon.c"),
            str(SERVICE_DIR / "mem_service_client.c"),
            str(SERVICE_DIR / "mem_service_wire_client.c"),
            str(SERVICE_DIR / "mem_service_metadata.c"),
            str(SERVICE_DIR / "mem_service_provider.c"),
            str(SERVICE_DIR / "mem_service_keys.c"),
            str(SERVICE_DIR / "mem_service_object_refs.c"),
            str(SERVICE_DIR / "mem_service_ub_ssd_gsva_backend.c"),
            str(SERVICE_DIR / "mem_service_ub_ssd_gsva_io.c"),
            str(SERVICE_DIR / "mem_service_records.c"),
            str(SERVICE_DIR / "mem_service_allocation.c"),
            str(SERVICE_DIR / "mem_service_provider_directory.c"),
            "-lm",
            "-o",
            str(self.binary),
        ]
        subprocess.run(cmd, cwd=REPO_ROOT, check=True, capture_output=True, text=True)

    def _run_client(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(self.binary), *args],
            cwd=REPO_ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )

    def _write_config(self, name: str, text: str) -> Path:
        path = self.root / name
        path.write_text(text)
        return path

    def _start_daemon(self, config_path: Path) -> subprocess.Popen:
        proc = subprocess.Popen(
            [str(self.binary), "serve", "--config", str(config_path)],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        deadline = time.time() + 5.0
        while time.time() < deadline:
            if proc.poll() is not None:
                stdout, stderr = proc.communicate(timeout=1)
                if "Operation not permitted" in stderr:
                    raise unittest.SkipTest("sandbox forbids socket bind in subprocess")
                self.fail(
                    f"mem_service daemon exited rc={proc.returncode}\n"
                    f"stdout={stdout}\nstderr={stderr}"
                )
            health = self._run_client("health", "--connect", f"unix:{self.socket}")
            if health.returncode == 0 and "status=ok" in health.stdout:
                return proc
            time.sleep(0.05)
        self._stop_server(proc)
        self.fail("mem_service daemon did not become ready")

    def _start_home_daemon(self, extra_config: str = "") -> subprocess.Popen:
        lines = [
            f"listen=unix:{self.socket}",
            f"required_provider={HOME_NODE}",
            "provider_lease_ms=30000",
            f"allocation_home_provider={HOME_NODE}",
        ]
        if extra_config:
            lines.append(extra_config)
        config = self._write_config("daemon.conf", "\n".join(lines) + "\n")
        return self._start_daemon(config)

    def _stop_server(self, proc: subprocess.Popen) -> tuple[str, str]:
        proc.terminate()
        try:
            stdout, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate(timeout=5)
        return stdout, stderr

    @property
    def _connect(self) -> str:
        return f"unix:{self.socket}"

    def _register_home(self, connect: str | None = None,
                       incarnation: int = HOME_INCARNATION) -> subprocess.CompletedProcess:
        result = self._run_client(
            "provider-register",
            "--node-id", HOME_NODE,
            "--incarnation", str(incarnation),
            "--readiness-generation", "1",
            "--capabilities", "1",
            "--connect", connect or self._connect,
        )
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn("status=ok", result.stdout)
        return result

    def _allocation_stats(self) -> dict[str, str]:
        result = self._run_client("allocation-stats", "--connect", self._connect)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn("status=ok", result.stdout)
        return _parse_kv(result.stdout)

    def _daemon_metrics(self) -> dict[str, int]:
        result = self._run_client("metrics", "--connect", self._connect)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        values: dict[str, int] = {}
        for name, value in _parse_kv(result.stdout).items():
            if value.isdigit():
                values[name] = int(value)
        return values

    def _write_session(self, name: str, connect: str, ops: list[str],
                       session_id: str | None = None,
                       header_extra: str = "") -> Path:
        lines = [f"session_id={session_id or name.rsplit('.', 1)[0]}",
                 f"connect={connect}"]
        if header_extra:
            lines.append(header_extra)
        lines.extend(f"op={op}" for op in ops)
        return self._write_config(name, "\n".join(lines) + "\n")

    def _run_session(self, config: Path) -> subprocess.CompletedProcess:
        return self._run_client("object-session", "--config", str(config))

    def _run_sessions_concurrent(
        self, configs: list[Path]
    ) -> list[subprocess.CompletedProcess]:
        results: list[subprocess.CompletedProcess | None] = [None] * len(configs)

        def run_one(index: int, config: Path) -> None:
            results[index] = self._run_session(config)

        threads = [
            threading.Thread(target=run_one, args=(i, config))
            for i, config in enumerate(configs)
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=40)
        for i, thread in enumerate(threads):
            if thread.is_alive():
                self.fail(f"object-session {configs[i]} did not finish in time")
        return [result for result in results if result is not None]

    # Bring up a daemon holding one published, provider-backed, mappable
    # object whose home address sits inside the loopback provider's
    # strict fixed-address range, so data-plane sessions can map it.
    def _start_active_object(self, key: str = "obj-1") -> subprocess.Popen:
        daemon = self._start_home_daemon()
        self._register_home()
        producer = self._write_session(
            "producer.conf",
            self._connect,
            [
                f"allocate key={key} idempotency_key=p-alloc-1 "
                "size_bytes=4096 capabilities=map",
            ],
        )
        provider = self._write_session(
            "provider.conf",
            self._connect,
            [
                f"publish key={key} node_id={HOME_NODE} "
                f"incarnation={HOME_INCARNATION} generation=1 "
                f"descriptor_hex=deadbeef address={DATA_MAP_ADDRESS_DEC} "
                f"address_len={DATA_MAP_LEN}",
            ],
        )
        producer_r = self._run_session(producer)
        self.assertEqual(producer_r.returncode, 0,
                         producer_r.stderr + producer_r.stdout)
        provider_r = self._run_session(provider)
        self.assertEqual(provider_r.returncode, 0,
                         provider_r.stderr + provider_r.stdout)
        return daemon

    # Full producer/consumer/provider choreography: allocate parks in
    # ALLOCATING, the provider session publishes, the consumer waits on
    # ACTIVE and takes a holder reference, the producer retires, the last
    # release drains to RETIRING and the provider's reclaim retires the
    # identity. Every step is a separate real CLI process. The scratch
    # barrier-1 object makes the cross-process ordering deterministic:
    # the producer cannot retire before the consumer holds a reference
    # (it waits for barrier-1 ALLOCATING, which the consumer only
    # creates after its acquire), and the provider cannot reclaim while
    # the consumer still holds one (it waits for barrier-1 RETIRED,
    # which the consumer only retires after its release).
    def test_three_session_choreography_unix(self):
        daemon = self._start_home_daemon()
        try:
            self._register_home()
            producer = self._write_session(
                "producer.conf",
                self._connect,
                [
                    "allocate key=obj-1 idempotency_key=p-alloc-1 size_bytes=4096 capabilities=map",
                    "wait_state key=obj-1 state=active timeout_ms=20000 poll_ms=50",
                    "wait_state key=barrier-1 state=allocating timeout_ms=20000 poll_ms=50",
                    "retire key=obj-1 idempotency_key=p-ret-1 expected_generation=1",
                    "wait_state key=obj-1 state=retired timeout_ms=20000 poll_ms=50",
                    "stats",
                ],
            )
            provider = self._write_session(
                "provider.conf",
                self._connect,
                [
                    "wait_state key=obj-1 state=allocating timeout_ms=20000 poll_ms=50",
                    f"publish key=obj-1 node_id={HOME_NODE} incarnation={HOME_INCARNATION} "
                    "generation=1 descriptor_hex=deadbeef address=4096 address_len=8192",
                    "wait_state key=obj-1 state=retiring timeout_ms=20000 poll_ms=50",
                    "wait_state key=barrier-1 state=retired timeout_ms=20000 poll_ms=50",
                    f"reclaim key=obj-1 node_id={HOME_NODE} incarnation={HOME_INCARNATION} "
                    "generation=1 confirmed=1",
                ],
            )
            consumer = self._write_session(
                "consumer.conf",
                self._connect,
                [
                    "wait_state key=obj-1 state=active timeout_ms=20000 poll_ms=50",
                    "acquire key=obj-1 idempotency_key=c-acq-1 expected_generation=1",
                    "inspect key=obj-1 expect_state=active expect_generation=1 expect_holder_count=1",
                    "allocate key=barrier-1 idempotency_key=c-bar-1 size_bytes=4096 capabilities=map",
                    "wait_state key=obj-1 state=retiring timeout_ms=20000 poll_ms=50",
                    "release key=obj-1 idempotency_key=c-rel-1 expected_generation=1",
                    # Generations come from a table-level counter: obj-1
                    # is 1, barrier-1 (allocated second) is 2.
                    "retire key=barrier-1 idempotency_key=c-bar-ret expected_generation=2",
                ],
            )
            producer_r, provider_r, consumer_r = self._run_sessions_concurrent(
                [producer, provider, consumer]
            )
            self.assertEqual(producer_r.returncode, 0,
                             producer_r.stderr + producer_r.stdout)
            self.assertEqual(provider_r.returncode, 0,
                             provider_r.stderr + provider_r.stdout)
            self.assertEqual(consumer_r.returncode, 0,
                             consumer_r.stderr + consumer_r.stdout)

            self.assertIn("session=producer op=1 action=allocate key=obj-1 "
                          "status=ok state=allocating generation=1",
                          producer_r.stdout)
            self.assertIn("session=producer op=4 action=retire key=obj-1 "
                          "status=ok state=retiring generation=1",
                          producer_r.stdout)
            self.assertIn("session=producer op=6 action=stats status=ok "
                          "live_objects=0", producer_r.stdout)
            self.assertIn("session=producer result=ok ops=6", producer_r.stdout)

            self.assertIn("session=provider op=2 action=publish key=obj-1 "
                          "status=ok state=active generation=1",
                          provider_r.stdout)
            self.assertIn("session=provider op=5 action=reclaim key=obj-1 "
                          "status=ok state=retired generation=1",
                          provider_r.stdout)
            self.assertIn("session=provider result=ok ops=5", provider_r.stdout)

            self.assertIn("session=consumer op=2 action=acquire key=obj-1 "
                          "status=ok state=active generation=1",
                          consumer_r.stdout)
            self.assertIn("session=consumer op=3 action=inspect key=obj-1 "
                          "status=ok state=active generation=1",
                          consumer_r.stdout)
            self.assertIn("session=consumer op=7 action=retire key=barrier-1 "
                          "status=ok state=retired generation=2",
                          consumer_r.stdout)
            self.assertIn("session=consumer result=ok ops=7", consumer_r.stdout)

            stats = self._run_client("allocation-stats", "--connect", self._connect)
            self.assertEqual(stats.returncode, 0, stats.stderr + stats.stdout)
            parsed = _parse_kv(stats.stdout)
            self.assertEqual(parsed.get("live_objects"), "0", stats.stdout)
            self.assertEqual(parsed.get("quarantined_objects"), "0", stats.stdout)
        finally:
            self._stop_server(daemon)

    # The same session machinery runs over the TCP control channel; the
    # provider session's publish declares node_id matching the allowlisted
    # connection source, so the anti-spoofing check accepts it.
    def test_session_over_tcp_control_channel(self):
        port = _free_tcp_port()
        lines = [
            f"listen=tcp:127.0.0.1:{port}",
            "auth_mode=trusted-guest-network",
            f"node_id={HOME_NODE}",
            f"network_peer={HOME_NODE}@127.0.0.1",
            f"required_provider={HOME_NODE}",
            "provider_lease_ms=30000",
            f"allocation_home_provider={HOME_NODE}",
        ]
        config = self._write_config("daemon-tcp.conf", "\n".join(lines) + "\n")
        daemon = subprocess.Popen(
            [str(self.binary), "serve", "--config", str(config)],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        connect = f"tcp:127.0.0.1:{port}"
        try:
            deadline = time.time() + 5.0
            while time.time() < deadline:
                if daemon.poll() is not None:
                    stdout, stderr = daemon.communicate(timeout=1)
                    if "Operation not permitted" in stderr:
                        raise unittest.SkipTest(
                            "sandbox forbids socket bind in subprocess"
                        )
                    self.fail(f"daemon exited rc={daemon.returncode}\n"
                              f"stdout={stdout}\nstderr={stderr}")
                health = self._run_client("health", "--connect", connect)
                if health.returncode == 0 and "status=ok" in health.stdout:
                    break
                time.sleep(0.05)
            else:
                self.fail("TCP daemon did not become ready")

            self._register_home(connect=connect)
            producer = self._write_session(
                "producer-tcp.conf",
                connect,
                [
                    "allocate key=obj-tcp idempotency_key=t-alloc-1 size_bytes=2048 "
                    "capabilities=map,block_io",
                    "wait_state key=obj-tcp state=active timeout_ms=20000 poll_ms=50",
                    "retire key=obj-tcp idempotency_key=t-ret-1",
                    "wait_state key=obj-tcp state=retired timeout_ms=20000 poll_ms=50",
                ],
                session_id="producer-tcp",
            )
            provider = self._write_session(
                "provider-tcp.conf",
                connect,
                [
                    "wait_state key=obj-tcp state=allocating timeout_ms=20000 poll_ms=50",
                    f"publish key=obj-tcp node_id={HOME_NODE} incarnation={HOME_INCARNATION} "
                    "generation=1 descriptor_hex=cafe address=8192 address_len=2048",
                    "wait_state key=obj-tcp state=retiring timeout_ms=20000 poll_ms=50",
                    f"reclaim key=obj-tcp node_id={HOME_NODE} incarnation={HOME_INCARNATION} "
                    "generation=1 confirmed=1",
                ],
                session_id="provider-tcp",
            )
            producer_r, provider_r = self._run_sessions_concurrent(
                [producer, provider]
            )
            self.assertEqual(producer_r.returncode, 0,
                             producer_r.stderr + producer_r.stdout)
            self.assertEqual(provider_r.returncode, 0,
                             provider_r.stderr + provider_r.stdout)
            self.assertIn("session=producer-tcp op=1 action=allocate "
                          "key=obj-tcp status=ok state=allocating",
                          producer_r.stdout)
            self.assertIn("session=producer-tcp result=ok ops=4",
                          producer_r.stdout)
            self.assertIn("session=provider-tcp result=ok ops=4",
                          provider_r.stdout)
        finally:
            self._stop_server(daemon)

    # expect_status turns managed-layer rejections into deterministic
    # assertions: stale generation, missing key and key conflict all
    # surface as wire statuses the session can pin down.
    def test_expect_status_negative_paths(self):
        daemon = self._start_home_daemon()
        try:
            self._register_home()
            seed = self._write_session(
                "seed.conf",
                self._connect,
                [
                    "allocate key=obj-neg idempotency_key=s-alloc-1 size_bytes=4096 capabilities=1",
                    "acquire key=obj-missing idempotency_key=s-acq-1 expect_status=not_found",
                    "acquire key=obj-neg idempotency_key=s-acq-2 expected_generation=99 "
                    "expect_status=stale_ref",
                    "allocate key=obj-neg idempotency_key=s-alloc-other size_bytes=4096 "
                    "capabilities=map expect_status=version_conflict",
                    "release key=obj-neg idempotency_key=s-rel-1 session_id=stranger "
                    "expect_status=not_found",
                    "inspect key=obj-missing expect_status=not_found",
                    "stats",
                ],
                session_id="seed",
            )
            result = self._run_session(seed)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("op=2 action=acquire key=obj-missing status=not_found",
                          result.stdout)
            self.assertIn("op=3 action=acquire key=obj-neg status=stale_ref",
                          result.stdout)
            self.assertIn("op=4 action=allocate key=obj-neg status=version_conflict",
                          result.stdout)
            self.assertIn("op=5 action=release key=obj-neg status=not_found",
                          result.stdout)
            self.assertIn("session=seed result=ok ops=7", result.stdout)
        finally:
            self._stop_server(daemon)

    # A default expect_status=ok mismatch aborts the session at the
    # failing op and reports the expected status; later ops never run.
    def test_unexpected_status_aborts_session(self):
        daemon = self._start_home_daemon()
        try:
            self._register_home()
            session = self._write_session(
                "abort.conf",
                self._connect,
                [
                    "acquire key=obj-nope idempotency_key=a-acq-1",
                    "allocate key=obj-nope idempotency_key=a-alloc-1 size_bytes=4096 capabilities=1",
                ],
                session_id="abort",
            )
            result = self._run_session(session)
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            self.assertIn("op=1 action=acquire key=obj-nope status=not_found "
                          "state=- generation=- mismatch=status "
                          "expected_status=ok", result.stdout)
            self.assertIn("session=abort result=failed op=1", result.stdout)
            self.assertNotIn("action=allocate", result.stdout)
        finally:
            self._stop_server(daemon)

    # inspect expect_* assertions: a wrong holder count fails with a
    # field-level mismatch; a matching assertion passes.
    def test_inspect_field_assertions(self):
        daemon = self._start_home_daemon()
        try:
            self._register_home()
            session = self._write_session(
                "inspect.conf",
                self._connect,
                [
                    "allocate key=obj-ins idempotency_key=i-alloc-1 size_bytes=4096 capabilities=1",
                    "inspect key=obj-ins expect_state=allocating expect_holder_count=2",
                    "inspect key=obj-ins expect_state=allocating expect_holder_count=0",
                ],
                session_id="inspect",
            )
            result = self._run_session(session)
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            self.assertIn("op=2 action=inspect key=obj-ins status=ok "
                          "state=allocating generation=1 mismatch=holder_count "
                          "expected_holder_count=2", result.stdout)
            self.assertIn("session=inspect result=failed op=2", result.stdout)
        finally:
            self._stop_server(daemon)

    # wait_state with a state that never arrives ends inside the bounded
    # timeout with status=timeout and a state mismatch line.
    def test_wait_state_timeout_is_bounded(self):
        daemon = self._start_home_daemon()
        try:
            self._register_home()
            session = self._write_session(
                "wait.conf",
                self._connect,
                [
                    "allocate key=obj-wait idempotency_key=w-alloc-1 size_bytes=4096 capabilities=1",
                    "wait_state key=obj-wait state=active timeout_ms=600 poll_ms=50",
                ],
                session_id="wait",
            )
            started = time.monotonic()
            result = self._run_session(session)
            elapsed = time.monotonic() - started
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            self.assertLess(elapsed, 10.0)
            self.assertIn("op=2 action=wait_state key=obj-wait status=timeout "
                          "state=allocating generation=1 mismatch=state "
                          "expected_state=active", result.stdout)
            self.assertIn("session=wait result=failed op=2", result.stdout)
        finally:
            self._stop_server(daemon)

    # Before the home provider registers, the data-plane gate keeps
    # allocate fail-closed; the same session grammar pins that outcome,
    # and a second session succeeds once the provider is registered.
    def test_allocate_fail_closed_until_provider_registers(self):
        daemon = self._start_home_daemon()
        try:
            gated = self._write_session(
                "gated.conf",
                self._connect,
                [
                    "allocate key=obj-gated idempotency_key=g-alloc-1 size_bytes=4096 "
                    "capabilities=1 expect_status=internal",
                ],
                session_id="gated",
            )
            result = self._run_session(gated)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("status=internal", result.stdout)

            self._register_home()
            open_session = self._write_session(
                "open.conf",
                self._connect,
                [
                    "allocate key=obj-gated idempotency_key=g-alloc-1 size_bytes=4096 capabilities=1",
                    "wait_state key=obj-gated state=allocating timeout_ms=2000",
                ],
                session_id="open",
            )
            result = self._run_session(open_session)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("state=allocating", result.stdout)
            self.assertIn("session=open result=ok ops=2", result.stdout)
        finally:
            self._stop_server(daemon)

    # Config validation: every malformed shape fails with exit 2 and a
    # config error naming the cause; none of them contact the daemon.
    def test_config_validation(self):
        daemon = self._start_home_daemon()
        try:
            cases = {
                "missing-session": "connect=unix:/x\nop=stats\n",
                "missing-connect": "session_id=x\nop=stats\n",
                "no-ops": f"session_id=x\nconnect={self._connect}\n",
                "unknown-line": f"session_id=x\nconnect={self._connect}\nbogus=1\n",
                "unknown-action": f"session_id=x\nconnect={self._connect}\nop=frobnicate key=k\n",
                "unknown-field": f"session_id=x\nconnect={self._connect}\n"
                                 "op=allocate key=k idempotency_key=i size_bytes=1 capabilities=1 bogus=1\n",
                "duplicate-field": f"session_id=x\nconnect={self._connect}\n"
                                   "op=allocate key=k key=k2 idempotency_key=i size_bytes=1 capabilities=1\n",
                "missing-required": f"session_id=x\nconnect={self._connect}\n"
                                    "op=allocate key=k size_bytes=1 capabilities=1\n",
                "bad-capabilities": f"session_id=x\nconnect={self._connect}\n"
                                    "op=allocate key=k idempotency_key=i size_bytes=1 capabilities=dma\n",
                "zero-capabilities": f"session_id=x\nconnect={self._connect}\n"
                                     "op=allocate key=k idempotency_key=i size_bytes=1 capabilities=0\n",
                "bad-u64": f"session_id=x\nconnect={self._connect}\n"
                           "op=allocate key=k idempotency_key=i size_bytes=-1 capabilities=1\n",
                "wait-timeout-unbounded": f"session_id=x\nconnect={self._connect}\n"
                                          "op=wait_state key=k state=active timeout_ms=999999\n",
                "wait-expect-status": f"session_id=x\nconnect={self._connect}\n"
                                      "op=wait_state key=k state=active timeout_ms=100 expect_status=ok\n",
                "bad-hex": f"session_id=x\nconnect={self._connect}\n"
                           "op=publish key=k node_id=n incarnation=1 generation=1 descriptor_hex=abc "
                           "address=1 address_len=1\n",
                "bad-confirmed": f"session_id=x\nconnect={self._connect}\n"
                                 "op=reclaim key=k node_id=n incarnation=1 generation=1 confirmed=2\n",
                "bad-expect-status": f"session_id=x\nconnect={self._connect}\n"
                                     "op=inspect key=k expect_status=banana\n",
                "bad-request-timeout": f"session_id=x\nconnect={self._connect}\n"
                                       "request_timeout_ms=0\nop=stats\n",
            }
            for name, text in cases.items():
                with self.subTest(case=name):
                    config = self._write_config(f"bad-{name}.conf", text)
                    result = self._run_session(config)
                    self.assertEqual(result.returncode, 2,
                                     f"{name}: rc={result.returncode} "
                                     f"stdout={result.stdout}")
                    self.assertIn("config error", result.stderr,
                                  f"{name}: {result.stderr}")
        finally:
            self._stop_server(daemon)

    # Usage errors: missing --config or a nonexistent path exits 2.
    def test_usage_errors(self):
        result = self._run_client("object-session")
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage:", result.stderr)

        result = self._run_client("object-session", "--config",
                                  str(self.root / "does-not-exist.conf"))
        self.assertEqual(result.returncode, 2)
        self.assertIn("config error", result.stderr)

    # Data plane through the in-process loopback provider: acquire gates
    # map, the write pattern is read back and pinned by an FNV-1a
    # checksum computed in the test, and unmap gates release. The mapped
    # base must equal the published home address (strict same-VA).
    def test_data_plane_map_write_read_loopback(self):
        daemon = self._start_active_object()
        try:
            seed = 7
            length = 4096
            checksum = _fnv1a64(_pattern(seed, length))
            consumer = self._write_session(
                "consumer.conf",
                self._connect,
                [
                    "wait_state key=obj-1 state=active timeout_ms=20000 poll_ms=50",
                    "acquire key=obj-1 idempotency_key=c-acq-1 "
                    "expected_generation=1",
                    "map key=obj-1 flags=readwrite",
                    f"write key=obj-1 offset=0 len={length} seed={seed}",
                    f"read key=obj-1 offset=0 len={length} seed={seed} "
                    f"expect_checksum=0x{checksum:016x}",
                    "unmap key=obj-1",
                    "release key=obj-1 idempotency_key=c-rel-1 "
                    "expected_generation=1",
                ],
                session_id="consumer",
                header_extra="provider=session-loopback",
            )
            result = self._run_session(consumer)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("action=map key=obj-1 status=ok "
                          f"base=0x{DATA_MAP_ADDRESS:016x} len={DATA_MAP_LEN}",
                          result.stdout)
            self.assertIn("action=write key=obj-1 status=ok offset=0 "
                          f"len={length} checksum=0x{checksum:016x}",
                          result.stdout)
            self.assertIn("action=read key=obj-1 status=ok offset=0 "
                          f"len={length} checksum=0x{checksum:016x}",
                          result.stdout)
            self.assertIn("action=unmap key=obj-1 status=ok", result.stdout)
            self.assertIn("session=consumer result=ok ops=7", result.stdout)
        finally:
            self._stop_server(daemon)

    # Every data-plane ordering violation fails closed at the offending
    # op; each scenario is a separate session config against one active
    # object. Data ops carry no expect_status, so a rejection always
    # aborts the session with result=failed at that op.
    def test_data_plane_state_machine_rejections(self):
        daemon = self._start_active_object()
        scenarios: list[tuple[str, list[str], str, str]] = [
            ("map-before-acquire",
             ["map key=obj-1"],
             "action=map key=obj-1 status=not_found note=no_holder",
             "result=failed op=1"),
            ("unmap-not-mapped",
             ["unmap key=obj-1"],
             "action=unmap key=obj-1 status=not_found note=not_mapped",
             "result=failed op=1"),
            ("write-not-mapped",
             ["write key=obj-1 offset=0 len=16 seed=1"],
             "action=write key=obj-1 status=not_found note=not_mapped",
             "result=failed op=1"),
            ("read-not-mapped",
             ["read key=obj-1 offset=0 len=16"],
             "action=read key=obj-1 status=not_found note=not_mapped",
             "result=failed op=1"),
            ("double-map",
             ["acquire key=obj-1 idempotency_key=dm-acq-1",
              "map key=obj-1",
              "map key=obj-1"],
             "action=map key=obj-1 status=capacity_exceeded "
             "note=already_mapped",
             "result=failed op=3"),
            ("release-while-mapped",
             ["acquire key=obj-1 idempotency_key=r-acq-1",
              "map key=obj-1",
              "release key=obj-1 idempotency_key=r-rel-1"],
             "action=release key=obj-1 status=internal note=mapping_active",
             "result=failed op=3"),
            ("write-not-writable",
             ["acquire key=obj-1 idempotency_key=w-acq-1",
              "map key=obj-1 flags=read",
              "write key=obj-1 offset=0 len=16 seed=1"],
             "action=write key=obj-1 status=unsupported note=not_writable",
             "result=failed op=3"),
            ("read-not-readable",
             ["acquire key=obj-1 idempotency_key=nr-acq-1",
              "map key=obj-1 flags=write",
              "read key=obj-1 offset=0 len=16"],
             "action=read key=obj-1 status=unsupported note=not_readable",
             "result=failed op=3"),
            ("write-out-of-bounds",
             ["acquire key=obj-1 idempotency_key=o-acq-1",
              "map key=obj-1",
              f"write key=obj-1 offset=0 len={DATA_MAP_LEN + 1} seed=1"],
             "action=write key=obj-1 status=capacity_exceeded "
             "note=out_of_bounds",
             "result=failed op=3"),
            ("read-out-of-bounds",
             ["acquire key=obj-1 idempotency_key=ob-acq-1",
              "map key=obj-1",
              f"read key=obj-1 offset={DATA_MAP_LEN} len=1"],
             "action=read key=obj-1 status=capacity_exceeded "
             "note=out_of_bounds",
             "result=failed op=3"),
            ("read-pattern-mismatch",
             ["acquire key=obj-1 idempotency_key=pm-acq-1",
              "map key=obj-1",
              "write key=obj-1 offset=0 len=256 seed=7",
              "read key=obj-1 offset=0 len=256 seed=9"],
             "action=read key=obj-1 status=checksum_mismatch "
             "note=pattern_mismatch",
             "result=failed op=4"),
            ("read-checksum-mismatch",
             ["acquire key=obj-1 idempotency_key=cm-acq-1",
              "map key=obj-1",
              "write key=obj-1 offset=0 len=256 seed=7",
              "read key=obj-1 offset=0 len=256 expect_checksum=0xdeadbeef"],
             "action=read key=obj-1 status=checksum_mismatch "
             "note=checksum_mismatch",
             "result=failed op=4"),
        ]
        try:
            for name, ops, marker, failure in scenarios:
                with self.subTest(scenario=name):
                    config = self._write_session(
                        f"neg-{name}.conf",
                        self._connect,
                        ops,
                        session_id=f"neg-{name}",
                        header_extra="provider=session-loopback",
                    )
                    result = self._run_session(config)
                    self.assertEqual(result.returncode, 1,
                                     f"{name}: {result.stderr}{result.stdout}")
                    self.assertIn(marker, result.stdout,
                                  f"{name}: {result.stdout}")
                    self.assertIn(f"session=neg-{name} {failure}",
                                  result.stdout, f"{name}: {result.stdout}")
        finally:
            self._stop_server(daemon)

    # End-of-session leak detection: a live mapping or a live holder
    # fails the session even though every op succeeded.
    def test_data_plane_end_of_session_leaks(self):
        daemon = self._start_active_object()
        try:
            mapped = self._write_session(
                "leak-mapped.conf",
                self._connect,
                [
                    "acquire key=obj-1 idempotency_key=lm-acq-1",
                    "map key=obj-1",
                ],
                session_id="leak-mapped",
                header_extra="provider=session-loopback",
            )
            result = self._run_session(mapped)
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            self.assertIn("session=leak-mapped result=failed "
                          "reason=active_mapping", result.stdout)

            held = self._write_session(
                "leak-held.conf",
                self._connect,
                ["acquire key=obj-1 idempotency_key=lh-acq-1"],
                session_id="leak-held",
                header_extra="provider=session-loopback",
            )
            result = self._run_session(held)
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            self.assertIn("session=leak-held result=failed reason=live_holder",
                          result.stdout)
        finally:
            self._stop_server(daemon)

    # Without a provider= line, data-plane ops are rejected at config
    # load (exit 2 before any connection): the session never starts
    # instead of silently falling back to a unavailable data plane.
    def test_data_plane_requires_provider_line(self):
        for action in ("map key=obj-1",
                       "unmap key=obj-1",
                       "write key=obj-1 offset=0 len=16 seed=1",
                       "read key=obj-1 offset=0 len=16"):
            with self.subTest(action=action):
                config = self._write_config(
                    f"no-provider-{action.split()[0]}.conf",
                    f"session_id=no-provider\nconnect={self._connect}\n"
                    f"op={action}\n",
                )
                result = self._run_session(config)
                self.assertEqual(result.returncode, 2,
                                 f"{action}: rc={result.returncode} "
                                 f"stdout={result.stdout}")
                self.assertIn("map/unmap/write/read ops require a provider= "
                              "line", result.stderr,
                              f"{action}: {result.stderr}")

    # provider=obmm is compile-gated: this host build lacks
    # MEM_SERVICE_OBJECT_SESSION_OBMM, so it is rejected at config load;
    # the loopback provider rejects obmm-only tuning keys; unknown kinds
    # are rejected outright. Config errors exit 2 before any connection.
    def test_provider_kind_validation(self):
        cases = {
            "obmm-unbuilt": ("provider=obmm\n",
                             "provider=obmm requires a build with "
                             "MEM_SERVICE_OBJECT_SESSION_OBMM"),
            "loopback-with-device": ("provider=session-loopback\n"
                                     "provider_device=/dev/obmm\n",
                                     "only valid for provider=obmm"),
            "unknown-kind": ("provider=bogus\n",
                             "unknown provider kind"),
        }
        for name, (header, detail) in cases.items():
            with self.subTest(case=name):
                config = self._write_config(
                    f"prov-{name}.conf",
                    f"session_id=prov-{name}\nconnect={self._connect}\n"
                    f"{header}op=stats\n",
                )
                result = self._run_session(config)
                self.assertEqual(result.returncode, 2,
                                 f"{name}: rc={result.returncode} "
                                 f"stdout={result.stdout}")
                self.assertIn(detail, result.stderr, f"{name}: {result.stderr}")

    # Boundary semantics (plan M1.4): zero-length and oversized
    # write/read payloads are rejected at config load (exit 2) before
    # any connection; the session never starts.
    def test_data_plane_zero_length_rejected_at_config(self):
        over_max = 16 * 1024 * 1024 + 1
        cases = {
            "write-zero": f"op=write key=obj-1 offset=0 len=0 seed=1\n",
            "read-zero": f"op=read key=obj-1 offset=0 len=0\n",
            "write-oversized": f"op=write key=obj-1 offset=0 len={over_max} seed=1\n",
            "read-oversized": f"op=read key=obj-1 offset=0 len={over_max}\n",
        }
        for name, op_line in cases.items():
            with self.subTest(case=name):
                config = self._write_config(
                    f"bounds-{name}.conf",
                    f"session_id=bounds-{name}\nconnect={self._connect}\n"
                    f"provider=session-loopback\n{op_line}",
                )
                result = self._run_session(config)
                self.assertEqual(result.returncode, 2,
                                 f"{name}: rc={result.returncode} "
                                 f"stdout={result.stdout}")
                self.assertIn("len out of bounds", result.stderr,
                              f"{name}: {result.stderr}")

    # Exact-fit boundaries and non-zero offsets (plan M1.4): write two
    # adjacent halves with different seeds, read them back independently
    # with pattern verification, then read the whole mapping at
    # offset+len == mapping.len with an FNV-1a checksum assertion, and
    # read the final byte at offset == len-1.
    def test_data_plane_exact_fit_and_partial_offsets(self):
        daemon = self._start_active_object()
        half = DATA_MAP_LEN // 2
        expected = _fnv1a64(_pattern(11, half) + _pattern(77, half))
        config = self._write_session(
            "exact-fit.conf",
            self._connect,
            [
                "acquire key=obj-1 idempotency_key=ef-acq-1",
                "map key=obj-1",
                f"write key=obj-1 offset=0 len={half} seed=11",
                f"write key=obj-1 offset={half} len={half} seed=77",
                f"read key=obj-1 offset={half} len={half} seed=77",
                f"read key=obj-1 offset=0 len={DATA_MAP_LEN} "
                f"expect_checksum=0x{expected:x}",
                f"read key=obj-1 offset={DATA_MAP_LEN - 1} len=1",
                "unmap key=obj-1",
                "release key=obj-1 idempotency_key=ef-rel-1",
            ],
            session_id="exact-fit",
            header_extra="provider=session-loopback",
        )
        try:
            result = self._run_session(config)
            self.assertEqual(result.returncode, 0,
                             result.stderr + result.stdout)
            for op_index, action in ((3, "write"), (4, "write"), (5, "read"),
                                     (6, "read"), (7, "read")):
                self.assertIn(f"session=exact-fit op={op_index} action={action} "
                              f"key=obj-1 status=ok", result.stdout)
            self.assertIn("session=exact-fit result=ok ops=9", result.stdout)
        finally:
            self._stop_server(daemon)

    # Unmappable publish addresses fail closed deterministically: an
    # unaligned address can never satisfy the strict same-VA contract
    # and is rejected at map time with map_failed, while a zero-length
    # region is rejected earlier, at publish, by the daemon. The
    # control-plane object is untouched by the data-plane failure: it
    # stays active and the holder can still release cleanly.
    def test_data_plane_unmappable_publish_address_rejected(self):
        daemon = self._start_home_daemon()
        try:
            self._register_home()
            setup = self._write_session(
                "um-unaligned-setup.conf",
                self._connect,
                [
                    "allocate key=obj-1 idempotency_key=um-unaligned-alloc "
                    "size_bytes=4096 capabilities=map",
                    f"publish key=obj-1 node_id={HOME_NODE} "
                    f"incarnation={HOME_INCARNATION} generation=1 "
                    f"descriptor_hex=deadbeef address={DATA_MAP_ADDRESS + 512} "
                    f"address_len={DATA_MAP_LEN}",
                ],
                session_id="um-unaligned-setup",
            )
            setup_r = self._run_session(setup)
            self.assertEqual(setup_r.returncode, 0,
                             setup_r.stderr + setup_r.stdout)

            mapping = self._write_session(
                "um-unaligned.conf",
                self._connect,
                [
                    "acquire key=obj-1 idempotency_key=um-acq-1",
                    "map key=obj-1",
                ],
                session_id="um-unaligned",
                header_extra="provider=session-loopback",
            )
            result = self._run_session(mapping)
            self.assertEqual(result.returncode, 1,
                             result.stderr + result.stdout)
            self.assertIn("action=map key=obj-1 status=internal "
                          "note=map_failed", result.stdout)
            self.assertIn("session=um-unaligned result=failed op=2",
                          result.stdout)

            cleanup = self._write_session(
                "um-unaligned-cleanup.conf",
                self._connect,
                [
                    "inspect key=obj-1 expect_state=active "
                    "expect_generation=1 expect_holder_count=1",
                    "release key=obj-1 idempotency_key=um-rel-1 "
                    "expected_generation=1",
                ],
                session_id="um-unaligned",
            )
            cleanup_r = self._run_session(cleanup)
            self.assertEqual(cleanup_r.returncode, 0,
                             cleanup_r.stderr + cleanup_r.stdout)

            # A zero-length region never reaches the data plane: the
            # daemon rejects the publish itself, and the object stays
            # in allocating (deterministic rollback-free rejection).
            zero = self._write_session(
                "um-zero-len.conf",
                self._connect,
                [
                    "allocate key=obj-2 idempotency_key=um-zero-alloc "
                    "size_bytes=4096 capabilities=map",
                    f"publish key=obj-2 node_id={HOME_NODE} "
                    f"incarnation={HOME_INCARNATION} generation=1 "
                    f"descriptor_hex=deadbeef address={DATA_MAP_ADDRESS_DEC} "
                    "address_len=0 expect_status=invalid_session",
                    "inspect key=obj-2 expect_state=allocating "
                    "expect_generation=2 expect_holder_count=0",
                ],
                session_id="um-zero-len",
            )
            zero_r = self._run_session(zero)
            self.assertEqual(zero_r.returncode, 0,
                             zero_r.stderr + zero_r.stdout)
            self.assertIn("op=2 action=publish key=obj-2 status=invalid_session",
                          zero_r.stdout)
            self.assertIn("op=3 action=inspect key=obj-2 status=ok",
                          zero_r.stdout)
            self.assertIn("session=um-zero-len result=ok ops=3",
                          zero_r.stdout)
        finally:
            self._stop_server(daemon)

    # Idempotency store exhaustion (64 records, no eviction): the 65th
    # mutating op with a fresh idempotency key is rejected with
    # capacity_exceeded no matter the operation, while a replay of an
    # already-recorded key still succeeds (it needs no new slot) and
    # non-mutating ops are unaffected. The idempotency store is the
    # tighter bound: the 128-slot managed table can never be filled
    # from the wire because every mutating op requires an idempotency
    # key.
    def test_idempotency_store_exhaustion(self):
        daemon = self._start_home_daemon()
        try:
            self._register_home()
            # A session config caps at 64 ops, so the 64 allocations
            # are split across two sessions sharing one daemon.
            for chunk in range(2):
                fill_ops = [
                    f"allocate key=fill-{n} idempotency_key=fill-{n}-alloc "
                    "size_bytes=4096 capabilities=map"
                    for n in range(chunk * 32, chunk * 32 + 32)
                ]
                fill = self._write_session(
                    f"fill-{chunk}.conf", self._connect, fill_ops,
                    session_id=f"fill-{chunk}")
                fill_r = self._run_session(fill)
                self.assertEqual(fill_r.returncode, 0,
                                 f"chunk {chunk}: "
                                 f"{fill_r.stderr}{fill_r.stdout}")

            overflow = self._write_session(
                "overflow.conf",
                self._connect,
                [
                    "allocate key=overflow idempotency_key=ov-alloc "
                    "size_bytes=4096 capabilities=map "
                    "expect_status=capacity_exceeded",
                    "retire key=fill-0 idempotency_key=ov-ret "
                    "expected_generation=1 expect_status=capacity_exceeded",
                    "inspect key=fill-0 expect_state=allocating "
                    "expect_generation=1 expect_holder_count=0",
                    "stats",
                ],
                session_id="overflow",
            )
            overflow_r = self._run_session(overflow)
            self.assertEqual(overflow_r.returncode, 0,
                             overflow_r.stderr + overflow_r.stdout)
            self.assertIn("op=1 action=allocate key=overflow "
                          "status=capacity_exceeded", overflow_r.stdout)
            self.assertIn("op=2 action=retire key=fill-0 "
                          "status=capacity_exceeded", overflow_r.stdout)
            self.assertIn("op=3 action=inspect key=fill-0 status=ok",
                          overflow_r.stdout)
            self.assertIn("op=4 action=stats status=ok live_objects=64 ",
                          overflow_r.stdout)
            self.assertIn("in_flight=64 ", overflow_r.stdout)

            # Identical payload (same session_id, same fields) replays
            # the recorded response instead of needing a fresh record.
            replay = self._write_session(
                "replay.conf",
                self._connect,
                [
                    "allocate key=fill-0 idempotency_key=fill-0-alloc "
                    "size_bytes=4096 capabilities=map",
                    "stats",
                ],
                session_id="fill-0",
            )
            replay_r = self._run_session(replay)
            self.assertEqual(replay_r.returncode, 0,
                             replay_r.stderr + replay_r.stdout)
            self.assertIn("op=1 action=allocate key=fill-0 status=ok "
                          "state=allocating", replay_r.stdout)
            self.assertIn("op=2 action=stats status=ok live_objects=64 ",
                          replay_r.stdout)
        finally:
            self._stop_server(daemon)

    # Holder exhaustion (8 holders per object): the 9th acquire is
    # rejected with capacity_exceeded, and after one holder releases a
    # retry succeeds. Acquire-only sessions fail their own leak check
    # (live_holder) while the daemon keeps the holder registered: a
    # disconnected client is never silently released.
    def test_holder_exhaustion(self):
        daemon = self._start_active_object()
        try:
            for n in range(1, 9):
                held = self._write_session(
                    f"h{n}.conf",
                    self._connect,
                    [f"acquire key=obj-1 idempotency_key=h{n}-acq"],
                    session_id=f"h{n}",
                )
                held_r = self._run_session(held)
                self.assertEqual(held_r.returncode, 1,
                                 f"h{n}: {held_r.stderr}{held_r.stdout}")
                self.assertIn("action=acquire key=obj-1 status=ok",
                              held_r.stdout, f"h{n}: {held_r.stdout}")
                self.assertIn(f"session=h{n} result=failed reason=live_holder",
                              held_r.stdout, f"h{n}: {held_r.stdout}")

            ninth = self._write_session(
                "h9.conf",
                self._connect,
                [
                    "acquire key=obj-1 idempotency_key=h9-acq "
                    "expect_status=capacity_exceeded",
                    "inspect key=obj-1 expect_state=active "
                    "expect_generation=1 expect_holder_count=8",
                ],
                session_id="h9",
            )
            ninth_r = self._run_session(ninth)
            self.assertEqual(ninth_r.returncode, 0,
                             ninth_r.stderr + ninth_r.stdout)
            self.assertIn("op=1 action=acquire key=obj-1 "
                          "status=capacity_exceeded", ninth_r.stdout)
            self.assertIn("op=2 action=inspect key=obj-1 status=ok",
                          ninth_r.stdout)

            release = self._write_session(
                "h1-release.conf",
                self._connect,
                ["release key=obj-1 idempotency_key=h1-rel "
                 "expected_generation=1"],
                session_id="h1",
            )
            release_r = self._run_session(release)
            self.assertEqual(release_r.returncode, 0,
                             release_r.stderr + release_r.stdout)

            retry = self._write_session(
                "h9b.conf",
                self._connect,
                [
                    "acquire key=obj-1 idempotency_key=h9b-acq",
                    "inspect key=obj-1 expect_state=active "
                    "expect_generation=1 expect_holder_count=8",
                ],
                session_id="h9b",
            )
            retry_r = self._run_session(retry)
            self.assertEqual(retry_r.returncode, 1,
                             retry_r.stderr + retry_r.stdout)
            self.assertIn("op=1 action=acquire key=obj-1 status=ok",
                          retry_r.stdout)
            self.assertIn("op=2 action=inspect key=obj-1 status=ok",
                          retry_r.stdout)
            self.assertIn("session=h9b result=failed reason=live_holder",
                          retry_r.stdout)
        finally:
            self._stop_server(daemon)

    # Concurrent allocation of the same key is deterministic: with the
    # same idempotency key both clients observe the same single object;
    # with different keys exactly one wins and the loser is rejected
    # with version_conflict.
    def test_concurrent_allocate_same_key(self):
        daemon = self._start_home_daemon()
        try:
            self._register_home()
            # Same idempotency key + identical request payload (same
            # session_id) from two processes: one executes, the other
            # replays the recorded response.
            same = [
                self._write_session(
                    f"race-a{n}.conf",
                    self._connect,
                    ["allocate key=race-a idempotency_key=race-a-shared "
                     "size_bytes=4096 capabilities=map"],
                    session_id="race-a",
                )
                for n in (1, 2)
            ]
            same_r = self._run_sessions_concurrent(same)
            for result in same_r:
                self.assertEqual(result.returncode, 0,
                                 result.stderr + result.stdout)
                self.assertIn("action=allocate key=race-a status=ok",
                              result.stdout)

            diff = [
                self._write_session(
                    f"race-b{n}.conf",
                    self._connect,
                    [f"allocate key=race-b idempotency_key=race-b-k{n} "
                     "size_bytes=4096 capabilities=map"],
                    session_id=f"race-b{n}",
                )
                for n in (1, 2)
            ]
            diff_r = self._run_sessions_concurrent(diff)
            oks = [r for r in diff_r if r.returncode == 0]
            conflicts = [r for r in diff_r if r.returncode == 1]
            self.assertEqual(len(oks), 1,
                             "\n".join(r.stdout + r.stderr for r in diff_r))
            self.assertEqual(len(conflicts), 1,
                             "\n".join(r.stdout + r.stderr for r in diff_r))
            self.assertIn("action=allocate key=race-b status=ok",
                          oks[0].stdout)
            self.assertIn("action=allocate key=race-b status=version_conflict",
                          conflicts[0].stdout)

            stats = self._write_session("race-stats.conf", self._connect,
                                        ["stats"], session_id="race-stats")
            stats_r = self._run_session(stats)
            self.assertEqual(stats_r.returncode, 0,
                             stats_r.stderr + stats_r.stdout)
            self.assertIn("action=stats status=ok live_objects=2 ",
                          stats_r.stdout)
        finally:
            self._stop_server(daemon)

    # Wire idempotency for holder ops (plan M1.4): a retry carrying the
    # same operation/idempotency identity and a byte-identical payload
    # replays the recorded outcome without re-executing the mutation;
    # the same key with any field changed (expected_generation,
    # session_id) or attached to a different operation is a
    # version_conflict, never a silent second change. The managed layer
    # is also naturally idempotent: re-acquire by an existing holder
    # with a fresh key succeeds without adding a holder, while a
    # re-executed release with a fresh key is a deterministic
    # not_found. Daemon metrics idempotency_replay_count /
    # idempotency_conflict_count and the managed ok/rejected counters
    # separate ledger replay from re-execution.
    def test_acquire_release_idempotent_replay_and_version_conflict(self):
        daemon = self._start_active_object()
        try:
            session = self._write_session(
                "idem-holder.conf",
                self._connect,
                [
                    "acquire key=obj-1 idempotency_key=ih-acq-1 expected_generation=1",
                    "acquire key=obj-1 idempotency_key=ih-acq-1 expected_generation=1",
                    "inspect key=obj-1 expect_state=active expect_generation=1 "
                    "expect_holder_count=1",
                    "acquire key=obj-1 idempotency_key=ih-acq-2 expected_generation=1",
                    "inspect key=obj-1 expect_state=active expect_generation=1 "
                    "expect_holder_count=1",
                    "acquire key=obj-1 idempotency_key=ih-acq-1 expected_generation=2 "
                    "expect_status=version_conflict",
                    "acquire key=obj-1 idempotency_key=ih-acq-1 session_id=stranger "
                    "expected_generation=1 expect_status=version_conflict",
                    "release key=obj-1 idempotency_key=ih-acq-1 expected_generation=1 "
                    "expect_status=version_conflict",
                    "release key=obj-1 idempotency_key=ih-rel-1 expected_generation=1",
                    "release key=obj-1 idempotency_key=ih-rel-1 expected_generation=1",
                    "release key=obj-1 idempotency_key=ih-rel-2 expected_generation=1 "
                    "expect_status=not_found",
                    "inspect key=obj-1 expect_state=active expect_generation=1 "
                    "expect_holder_count=0",
                    "stats",
                ],
                session_id="idem-holder",
            )
            result = self._run_session(session)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("op=1 action=acquire key=obj-1 status=ok "
                          "state=active generation=1", result.stdout)
            # op=2 is the ledger replay of op=1: identical outcome.
            self.assertIn("op=2 action=acquire key=obj-1 status=ok "
                          "state=active generation=1", result.stdout)
            # op=4 is a natural re-acquire by the existing holder.
            self.assertIn("op=4 action=acquire key=obj-1 status=ok "
                          "state=active generation=1", result.stdout)
            self.assertIn("op=6 action=acquire key=obj-1 "
                          "status=version_conflict", result.stdout)
            self.assertIn("op=7 action=acquire key=obj-1 "
                          "status=version_conflict", result.stdout)
            self.assertIn("op=8 action=release key=obj-1 "
                          "status=version_conflict", result.stdout)
            self.assertIn("op=9 action=release key=obj-1 status=ok "
                          "state=active generation=1", result.stdout)
            # op=10 replays the recorded release; a re-executed release
            # would be not_found because the holder is already gone.
            self.assertIn("op=10 action=release key=obj-1 status=ok "
                          "state=active generation=1", result.stdout)
            self.assertIn("op=11 action=release key=obj-1 status=not_found",
                          result.stdout)
            self.assertIn("op=13 action=stats status=ok live_objects=1 ",
                          result.stdout)
            self.assertIn("live_refs=0 ", result.stdout)
            self.assertIn("session=idem-holder result=ok ops=13", result.stdout)

            stats = self._allocation_stats()
            self.assertEqual(stats["acquire_ok_count"], "2")
            self.assertEqual(stats["acquire_rejected_count"], "0")
            self.assertEqual(stats["release_ok_count"], "1")
            self.assertEqual(stats["release_rejected_count"], "1")
            self.assertEqual(stats["live_refs"], "0")
            self.assertEqual(stats["live_objects"], "1")

            metrics = self._daemon_metrics()
            self.assertEqual(metrics["idempotency_replay_count"], 2)
            self.assertEqual(metrics["idempotency_conflict_count"], 3)
        finally:
            self._stop_server(daemon)

    # Retire/reclaim idempotency: a holder-free provider-backed object
    # parks in RETIRING until the home provider confirms reclaim. A
    # byte-identical retire replay is served from the ledger; a retire
    # with a fresh key re-executes harmlessly on RETIRING. Reclaim is
    # deliberately not a ledger operation: a repeated confirmed reclaim
    # against the terminal RETIRED state is naturally idempotent and
    # returns ok/retired without touching the replay counters, while a
    # retire against RETIRED is a deterministic stale_ref rejection.
    def test_retire_reclaim_idempotent_replay_and_terminal_rejection(self):
        daemon = self._start_active_object()
        try:
            retire = self._write_session(
                "retire.conf",
                self._connect,
                [
                    "retire key=obj-1 idempotency_key=rt-1 expected_generation=1",
                    "retire key=obj-1 idempotency_key=rt-1 expected_generation=1",
                    "retire key=obj-1 idempotency_key=rt-1 expected_generation=2 "
                    "expect_status=version_conflict",
                    "retire key=obj-1 idempotency_key=rt-2 expected_generation=1",
                    "inspect key=obj-1 expect_state=retiring expect_generation=1",
                    "stats",
                ],
                session_id="retire",
            )
            retire_r = self._run_session(retire)
            self.assertEqual(retire_r.returncode, 0,
                             retire_r.stderr + retire_r.stdout)
            self.assertIn("op=1 action=retire key=obj-1 status=ok "
                          "state=retiring generation=1", retire_r.stdout)
            self.assertIn("op=2 action=retire key=obj-1 status=ok "
                          "state=retiring generation=1", retire_r.stdout)
            self.assertIn("op=3 action=retire key=obj-1 "
                          "status=version_conflict", retire_r.stdout)
            self.assertIn("op=4 action=retire key=obj-1 status=ok "
                          "state=retiring generation=1", retire_r.stdout)
            self.assertIn("op=6 action=stats status=ok live_objects=1 ",
                          retire_r.stdout)

            reclaim = self._write_session(
                "reclaim.conf",
                self._connect,
                [
                    f"reclaim key=obj-1 node_id={HOME_NODE} "
                    f"incarnation={HOME_INCARNATION} generation=1 confirmed=1",
                    f"reclaim key=obj-1 node_id={HOME_NODE} "
                    f"incarnation={HOME_INCARNATION} generation=1 confirmed=1",
                    "retire key=obj-1 idempotency_key=rt-3 expected_generation=1 "
                    "expect_status=stale_ref",
                    "inspect key=obj-1 expect_state=retired expect_generation=1",
                    "stats",
                ],
                session_id="reclaim",
            )
            reclaim_r = self._run_session(reclaim)
            self.assertEqual(reclaim_r.returncode, 0,
                             reclaim_r.stderr + reclaim_r.stdout)
            self.assertIn("op=1 action=reclaim key=obj-1 status=ok "
                          "state=retired generation=1", reclaim_r.stdout)
            self.assertIn("op=2 action=reclaim key=obj-1 status=ok "
                          "state=retired generation=1", reclaim_r.stdout)
            self.assertIn("op=3 action=retire key=obj-1 status=stale_ref",
                          reclaim_r.stdout)
            self.assertIn("op=5 action=stats status=ok live_objects=0 ",
                          reclaim_r.stdout)

            stats = self._allocation_stats()
            self.assertEqual(stats["retire_ok_count"], "2")
            self.assertEqual(stats["retire_rejected_count"], "1")
            self.assertEqual(stats["reclaim_ok_count"], "2")
            self.assertEqual(stats["reclaim_rejected_count"], "0")
            self.assertEqual(stats["live_objects"], "0")
            self.assertEqual(stats["quarantined_objects"], "0")

            metrics = self._daemon_metrics()
            self.assertEqual(metrics["idempotency_replay_count"], 1)
            self.assertEqual(metrics["idempotency_conflict_count"], 1)
        finally:
            self._stop_server(daemon)

    # Rejected outcomes are ledger outcomes too: a stale-generation
    # acquire is recorded with its stale_ref response, and an identical
    # retry replays that rejection instead of re-executing it (the
    # managed rejected counter moves exactly once). The object itself
    # is untouched: no holder appears and the state stays active.
    def test_rejected_outcome_idempotent_replay(self):
        daemon = self._start_active_object()
        try:
            session = self._write_session(
                "idem-reject.conf",
                self._connect,
                [
                    "acquire key=obj-1 idempotency_key=rj-acq-1 "
                    "expected_generation=99 expect_status=stale_ref",
                    "acquire key=obj-1 idempotency_key=rj-acq-1 "
                    "expected_generation=99 expect_status=stale_ref",
                    "inspect key=obj-1 expect_state=active expect_generation=1 "
                    "expect_holder_count=0",
                    "stats",
                ],
                session_id="idem-reject",
            )
            result = self._run_session(session)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("op=1 action=acquire key=obj-1 status=stale_ref",
                          result.stdout)
            self.assertIn("op=2 action=acquire key=obj-1 status=stale_ref",
                          result.stdout)
            self.assertIn("op=4 action=stats status=ok live_objects=1 ",
                          result.stdout)
            self.assertIn("session=idem-reject result=ok ops=4", result.stdout)

            stats = self._allocation_stats()
            self.assertEqual(stats["acquire_ok_count"], "0")
            self.assertEqual(stats["acquire_rejected_count"], "1")
            self.assertEqual(stats["live_refs"], "0")

            metrics = self._daemon_metrics()
            self.assertEqual(metrics["idempotency_replay_count"], 1)
            self.assertEqual(metrics["idempotency_conflict_count"], 0)
        finally:
            self._stop_server(daemon)

    # Failed publishes apply nothing (plan M1.4 failure determinism):
    # a provider-mismatch publish (wrong incarnation) is stopped by the
    # daemon's provider caller check and a stale-generation publish is
    # rejected by the managed table, both before any field is written,
    # so the object stays ALLOCATING with its original generation and
    # zero reserved resources, and a later correct publish still
    # completes the allocation.
    def test_failed_publish_leaves_allocating_state_deterministic(self):
        daemon = self._start_home_daemon()
        try:
            self._register_home()
            session = self._write_session(
                "pub-fail.conf",
                self._connect,
                [
                    "allocate key=obj-p idempotency_key=pf-alloc "
                    "size_bytes=4096 capabilities=map",
                    f"publish key=obj-p node_id={HOME_NODE} incarnation=99 "
                    "generation=1 descriptor_hex=deadbeef address=4096 "
                    "address_len=8192 expect_status=version_conflict",
                    f"publish key=obj-p node_id={HOME_NODE} "
                    f"incarnation={HOME_INCARNATION} generation=99 "
                    "descriptor_hex=deadbeef address=4096 "
                    "address_len=8192 expect_status=stale_ref",
                    "inspect key=obj-p expect_state=allocating "
                    "expect_generation=1 expect_holder_count=0",
                    "stats",
                    f"publish key=obj-p node_id={HOME_NODE} "
                    f"incarnation={HOME_INCARNATION} generation=1 "
                    "descriptor_hex=deadbeef address=4096 address_len=8192",
                    "inspect key=obj-p expect_state=active expect_generation=1",
                    "stats",
                ],
                session_id="pub-fail",
            )
            result = self._run_session(session)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("op=2 action=publish key=obj-p "
                          "status=version_conflict", result.stdout)
            self.assertIn("op=3 action=publish key=obj-p status=stale_ref",
                          result.stdout)
            self.assertIn("op=4 action=inspect key=obj-p status=ok "
                          "state=allocating generation=1", result.stdout)
            self.assertIn("op=5 action=stats status=ok live_objects=1 "
                          "backing_allocated_bytes=0 address_reserved_bytes=0 "
                          "live_refs=0 in_flight=1 quarantined_objects=0 "
                          "quarantined_bytes=0", result.stdout)
            self.assertIn("op=6 action=publish key=obj-p status=ok "
                          "state=active generation=1", result.stdout)
            self.assertIn("op=8 action=stats status=ok live_objects=1 "
                          "backing_allocated_bytes=4096 "
                          "address_reserved_bytes=8192 live_refs=0 in_flight=0 "
                          "quarantined_objects=0 quarantined_bytes=0",
                          result.stdout)

            # The daemon caller check rejects the wrong-incarnation
            # publish before it reaches the managed table, so only the
            # stale-generation publish is counted as a managed-layer
            # rejection; neither rejection wrote any object state.
            stats = self._allocation_stats()
            self.assertEqual(stats["publish_ok_count"], "1")
            self.assertEqual(stats["publish_rejected_count"], "1")
        finally:
            self._stop_server(daemon)

    # in_flight is a transient process value, not a leak: it counts
    # exactly the ALLOCATING and RETIRING phases and returns to zero
    # at ACTIVE and at RETIRED, while backing/address counters only
    # exist between publish and reclaim.
    def test_in_flight_transient_across_lifecycle(self):
        daemon = self._start_home_daemon()
        try:
            self._register_home()
            session = self._write_session(
                "inflight.conf",
                self._connect,
                [
                    "allocate key=obj-x idempotency_key=if-alloc "
                    "size_bytes=4096 capabilities=map",
                    "stats",
                    f"publish key=obj-x node_id={HOME_NODE} "
                    f"incarnation={HOME_INCARNATION} generation=1 "
                    "descriptor_hex=deadbeef address=4096 address_len=8192",
                    "stats",
                    "acquire key=obj-x idempotency_key=if-acq "
                    "expected_generation=1",
                    "stats",
                    "release key=obj-x idempotency_key=if-rel "
                    "expected_generation=1",
                    "stats",
                    "retire key=obj-x idempotency_key=if-ret "
                    "expected_generation=1",
                    "stats",
                    f"reclaim key=obj-x node_id={HOME_NODE} "
                    f"incarnation={HOME_INCARNATION} generation=1 confirmed=1",
                    "stats",
                ],
                session_id="inflight",
            )
            result = self._run_session(session)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("op=2 action=stats status=ok live_objects=1 "
                          "backing_allocated_bytes=0 address_reserved_bytes=0 "
                          "live_refs=0 in_flight=1 quarantined_objects=0 "
                          "quarantined_bytes=0", result.stdout)
            self.assertIn("op=4 action=stats status=ok live_objects=1 "
                          "backing_allocated_bytes=4096 "
                          "address_reserved_bytes=8192 live_refs=0 in_flight=0 "
                          "quarantined_objects=0 quarantined_bytes=0",
                          result.stdout)
            self.assertIn("op=6 action=stats status=ok live_objects=1 "
                          "backing_allocated_bytes=4096 "
                          "address_reserved_bytes=8192 live_refs=1 in_flight=0 "
                          "quarantined_objects=0 quarantined_bytes=0",
                          result.stdout)
            self.assertIn("op=8 action=stats status=ok live_objects=1 "
                          "backing_allocated_bytes=4096 "
                          "address_reserved_bytes=8192 live_refs=0 in_flight=0 "
                          "quarantined_objects=0 quarantined_bytes=0",
                          result.stdout)
            self.assertIn("op=10 action=stats status=ok live_objects=1 "
                          "backing_allocated_bytes=4096 "
                          "address_reserved_bytes=8192 live_refs=0 in_flight=1 "
                          "quarantined_objects=0 quarantined_bytes=0",
                          result.stdout)
            self.assertIn("op=12 action=stats status=ok live_objects=0 "
                          "backing_allocated_bytes=0 address_reserved_bytes=0 "
                          "live_refs=0 in_flight=0 quarantined_objects=0 "
                          "quarantined_bytes=0", result.stdout)
        finally:
            self._stop_server(daemon)

    # Quarantine is the deterministic terminal state for an
    # unconfirmable release (plan M1.4 isolation stats): a provider
    # reclaim with confirmed=0 moves the object to QUARANTINED, its
    # bytes are reported only in the quarantined counters and never
    # mixed back into live/backing/address accounting, and every later
    # mutation against the quarantined identity fails closed (acquire,
    # retire) except the naturally idempotent terminal reclaim.
    # Re-allocating the same key is a version_conflict: a quarantined
    # identity is never silently reused.
    def test_quarantine_isolation_stats_and_terminal_behavior(self):
        daemon = self._start_active_object()
        try:
            session = self._write_session(
                "quarantine.conf",
                self._connect,
                [
                    "stats",
                    "retire key=obj-1 idempotency_key=q-ret-1 "
                    "expected_generation=1",
                    "stats",
                    f"reclaim key=obj-1 node_id={HOME_NODE} "
                    f"incarnation={HOME_INCARNATION} generation=1 confirmed=0",
                    "inspect key=obj-1 expect_state=quarantined "
                    "expect_generation=1",
                    "stats",
                    "acquire key=obj-1 idempotency_key=q-acq-1 "
                    "expected_generation=1 expect_status=stale_ref",
                    "retire key=obj-1 idempotency_key=q-ret-2 "
                    "expected_generation=1 expect_status=stale_ref",
                    f"reclaim key=obj-1 node_id={HOME_NODE} "
                    f"incarnation={HOME_INCARNATION} generation=1 confirmed=1",
                    "allocate key=obj-1 idempotency_key=q-alloc-2 "
                    "size_bytes=4096 capabilities=map "
                    "expect_status=version_conflict",
                    "stats",
                ],
                session_id="quarantine",
            )
            result = self._run_session(session)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("op=1 action=stats status=ok live_objects=1 "
                          "backing_allocated_bytes=4096 "
                          "address_reserved_bytes=16384 live_refs=0 "
                          "in_flight=0 quarantined_objects=0 "
                          "quarantined_bytes=0", result.stdout)
            self.assertIn("op=4 action=reclaim key=obj-1 status=ok "
                          "state=quarantined generation=1", result.stdout)
            self.assertIn("op=6 action=stats status=ok live_objects=0 "
                          "backing_allocated_bytes=0 address_reserved_bytes=0 "
                          "live_refs=0 in_flight=0 quarantined_objects=1 "
                          "quarantined_bytes=4096", result.stdout)
            self.assertIn("op=7 action=acquire key=obj-1 status=stale_ref",
                          result.stdout)
            self.assertIn("op=8 action=retire key=obj-1 status=stale_ref",
                          result.stdout)
            self.assertIn("op=9 action=reclaim key=obj-1 status=ok "
                          "state=quarantined generation=1", result.stdout)
            self.assertIn("op=10 action=allocate key=obj-1 "
                          "status=version_conflict", result.stdout)
            self.assertIn("op=11 action=stats status=ok live_objects=0 "
                          "backing_allocated_bytes=0 address_reserved_bytes=0 "
                          "live_refs=0 in_flight=0 quarantined_objects=1 "
                          "quarantined_bytes=4096", result.stdout)

            stats = self._allocation_stats()
            self.assertEqual(stats["quarantine_events"], "1")
            self.assertEqual(stats["quarantined_objects"], "1")
            self.assertEqual(stats["quarantined_bytes"], "4096")
            self.assertEqual(stats["reclaim_ok_count"], "2")
            self.assertEqual(stats["acquire_rejected_count"], "1")
            self.assertEqual(stats["retire_rejected_count"], "1")
            self.assertEqual(stats["allocate_rejected_count"], "1")
        finally:
            self._stop_server(daemon)

    # A killed client never implies release (plan M1.4: a disconnected
    # holder is not evidence of release): after the client process is
    # SIGKILLed mid-session the daemon keeps the holder registered
    # indefinitely and every reclaim, confirmed or not, is rejected
    # while the holder record exists. With the owning session gone no
    # explicit release can ever arrive, so the object stays in RETIRING
    # permanently, still counted in live_refs and in_flight.
    def test_killed_client_holder_is_never_silently_reclaimed(self):
        daemon = self._start_active_object()
        victim = None
        try:
            victim_config = self._write_session(
                "killed.conf",
                self._connect,
                [
                    "acquire key=obj-1 idempotency_key=k-acq-1 "
                    "expected_generation=1",
                    "wait_state key=obj-1 state=retired timeout_ms=30000 "
                    "poll_ms=100",
                ],
                session_id="killed",
            )
            victim = subprocess.Popen(
                [str(self.binary), "object-session", "--config",
                 str(victim_config)],
                cwd=REPO_ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            deadline = time.time() + 10.0
            while time.time() < deadline:
                if victim.poll() is not None:
                    stdout, stderr = victim.communicate(timeout=1)
                    self.fail(f"victim session exited early rc="
                              f"{victim.returncode}\n{stdout}{stderr}")
                if self._allocation_stats().get("live_refs") == "1":
                    break
                time.sleep(0.05)
            else:
                self.fail("holder was never registered by victim session")

            victim.kill()
            victim.communicate(timeout=5)
            victim = None

            # The RPC disconnect changes nothing: the holder survives
            # client death across repeated observations.
            first = self._allocation_stats()
            time.sleep(0.2)
            second = self._allocation_stats()
            self.assertEqual(first["live_refs"], "1")
            self.assertEqual(second["live_refs"], "1")
            self.assertEqual(first["live_objects"], "1")

            checker = self._write_session(
                "checker.conf",
                self._connect,
                [
                    "retire key=obj-1 idempotency_key=k-ret-1 "
                    "expected_generation=1",
                    "inspect key=obj-1 expect_state=retiring "
                    "expect_generation=1 expect_holder_count=1",
                    f"reclaim key=obj-1 node_id={HOME_NODE} "
                    f"incarnation={HOME_INCARNATION} generation=1 "
                    "confirmed=1 expect_status=stale_ref",
                    f"reclaim key=obj-1 node_id={HOME_NODE} "
                    f"incarnation={HOME_INCARNATION} generation=1 "
                    "confirmed=0 expect_status=stale_ref",
                    "inspect key=obj-1 expect_state=retiring "
                    "expect_generation=1 expect_holder_count=1",
                    "stats",
                ],
                session_id="checker",
            )
            checker_r = self._run_session(checker)
            self.assertEqual(checker_r.returncode, 0,
                             checker_r.stderr + checker_r.stdout)
            self.assertIn("op=3 action=reclaim key=obj-1 status=stale_ref",
                          checker_r.stdout)
            self.assertIn("op=4 action=reclaim key=obj-1 status=stale_ref",
                          checker_r.stdout)
            self.assertIn("op=5 action=inspect key=obj-1 status=ok "
                          "state=retiring generation=1", checker_r.stdout)
            self.assertIn("op=6 action=stats status=ok live_objects=1 "
                          "backing_allocated_bytes=4096 "
                          "address_reserved_bytes=16384 live_refs=1 "
                          "in_flight=1 quarantined_objects=0 "
                          "quarantined_bytes=0",
                          checker_r.stdout)

            stats = self._allocation_stats()
            self.assertEqual(stats["reclaim_rejected_count"], "2")
            self.assertEqual(stats["reclaim_ok_count"], "0")
            self.assertEqual(stats["quarantine_events"], "0")
        finally:
            if victim is not None:
                victim.kill()
                victim.communicate(timeout=5)
            self._stop_server(daemon)


if __name__ == "__main__":
    unittest.main()
