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
    # identity. Every step is a separate real CLI process.
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
                    "release key=obj-1 idempotency_key=c-rel-1 expected_generation=1",
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
            self.assertIn("session=producer op=3 action=retire key=obj-1 "
                          "status=ok state=retiring generation=1",
                          producer_r.stdout)
            self.assertIn("session=producer op=5 action=stats status=ok "
                          "live_objects=0", producer_r.stdout)
            self.assertIn("session=producer result=ok ops=5", producer_r.stdout)

            self.assertIn("session=provider op=2 action=publish key=obj-1 "
                          "status=ok state=active generation=1",
                          provider_r.stdout)
            self.assertIn("session=provider op=4 action=reclaim key=obj-1 "
                          "status=ok state=retired generation=1",
                          provider_r.stdout)
            self.assertIn("session=provider result=ok ops=4", provider_r.stdout)

            self.assertIn("session=consumer op=2 action=acquire key=obj-1 "
                          "status=ok state=active generation=1",
                          consumer_r.stdout)
            self.assertIn("session=consumer op=3 action=inspect key=obj-1 "
                          "status=ok state=active generation=1",
                          consumer_r.stdout)
            self.assertIn("session=consumer result=ok ops=4", consumer_r.stdout)

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


if __name__ == "__main__":
    unittest.main()
