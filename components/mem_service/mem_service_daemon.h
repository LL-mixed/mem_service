#ifndef MEM_SERVICE_DAEMON_H
#define MEM_SERVICE_DAEMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mem_service_remote_transport_probe_result {
    bool payload_block_round_trip;
    bool payload_checksum_validation;
    bool payload_corruption_fail_closed;
    uint64_t payload_len;
    uint64_t payload_checksum;
};

struct mem_service_daemon_limits {
    uint64_t max_records;
    uint64_t max_payload_bytes;
    uint64_t max_audit_events;
    uint64_t max_checkpoint_records;
    uint64_t max_retained_records;
    uint64_t max_retained_record_age_ms;
    uint32_t max_retained_record_kind;
    bool max_retained_record_tenant_enabled;
    uint32_t max_retained_record_tenant;
};

#define MEM_SERVICE_NETWORK_NODE_ID_LEN 64U
#define MEM_SERVICE_NETWORK_IPV4_LEN 16U
#define MEM_SERVICE_NETWORK_MAX_PEERS 32U
#define MEM_SERVICE_NETWORK_DEFAULT_IO_TIMEOUT_MS 3000U

/*
 * Trusted-guest-network access policy for serving the wire protocol over
 * TCP. The allowlist maps stable node IDs to exact guest IPv4 addresses
 * one-to-one; wildcards, CIDR ranges and duplicate identities are rejected
 * by the configuration layer. This mode has no TLS or cryptographic peer
 * authentication: it is only valid on an isolated guest network where all
 * listed nodes form one trust domain.
 */
struct mem_service_network_peer {
    char node_id[MEM_SERVICE_NETWORK_NODE_ID_LEN];
    char ipv4[MEM_SERVICE_NETWORK_IPV4_LEN];
};

struct mem_service_network_access {
    bool enabled;
    char node_id[MEM_SERVICE_NETWORK_NODE_ID_LEN];
    struct mem_service_network_peer peers[MEM_SERVICE_NETWORK_MAX_PEERS];
    size_t peer_count;
    uint64_t io_timeout_ms;
};

struct mem_service_provider_registry;
struct mem_service_provider_directory_config;

struct mem_service_daemon_runtime {
    const struct mem_service_daemon_limits *limits;
    const struct mem_service_provider_registry *providers;
    const struct mem_service_network_access *network;
    /*
     * Optional required-provider set and lease for the control-plane
     * provider directory. When present, managed data operations stay
     * fail-closed until every required provider node holds a fresh
     * registration; when absent the directory keeps legacy behavior
     * (data operations allowed, readiness not gated).
     */
    const struct mem_service_provider_directory_config *provider_directory;
    /*
     * Optional single home provider node (the M1 address-allocation
     * owner). When set, managed allocate binds the allocation to this
     * node's active registration and waits in ALLOCATING for the
     * provider's publish; when NULL the managed table keeps its legacy
     * behavior (in-process backing or fail-closed).
     */
    const char *allocation_home_provider;
};

int mem_service_run_unix_daemon(const char *listen_spec);
int mem_service_run_unix_daemon_with_store(const char *listen_spec, const char *store_path);
int mem_service_run_unix_daemon_with_store_and_metrics(const char *listen_spec,
                                                       const char *store_path,
                                                       const char *metrics_listen_spec);
int mem_service_run_unix_daemon_with_store_metrics_and_catalog(
    const char *listen_spec,
    const char *store_path,
    const char *metrics_listen_spec,
    const char *storage_root);
int mem_service_run_unix_daemon_with_store_metrics_catalog_and_limits(
    const char *listen_spec,
    const char *store_path,
    const char *metrics_listen_spec,
    const char *storage_root,
    const struct mem_service_daemon_limits *limits);
int mem_service_run_unix_daemon_with_runtime(
    const char *listen_spec,
    const char *store_path,
    const char *metrics_listen_spec,
    const char *storage_root,
    const struct mem_service_daemon_runtime *runtime);
/*
 * Transport-neutral serve entry. A "tcp:<ipv4>:<port>" listen spec requires
 * runtime->network to describe an enabled trusted-guest-network access
 * policy; without one the daemon refuses to expose the wire protocol on a
 * network endpoint. Unix listen specs keep the existing local behavior and
 * ignore the network policy.
 */
int mem_service_run_daemon_with_runtime(
    const char *listen_spec,
    const char *store_path,
    const char *metrics_listen_spec,
    const char *storage_root,
    const struct mem_service_daemon_runtime *runtime);
int mem_service_run_wire_fixture_check(void);
int mem_service_run_store_fixture_check(void);
int mem_service_run_journal_fixture_check(void);
int mem_service_run_journal_torn_recovery_fixture_check(void);
int mem_service_run_journal_compaction_fixture_check(void);
int mem_service_run_durable_catalog_fixture_check(void);
int mem_service_run_chunked_block_fixture_check(void);
int mem_service_run_transport_block_fixture_check(void);
int mem_service_run_network_transport_block_fixture_check(void);
int mem_service_run_tcp_payload_fixture_source(const char *listen_spec,
                                               uint64_t payload_len);
int mem_service_run_runtime_quota_fixture_check(void);
int mem_service_run_retention_fixture_check(void);
int mem_service_run_checkpoint_retention_fixture_check(void);
int mem_service_run_payload_gc_fixture_check(void);
int mem_service_run_record_retention_fixture_check(void);
int mem_service_probe_transport_tcp_payload_block(
    const char *storage_root,
    const char *payload_source,
    struct mem_service_remote_transport_probe_result *result);
int mem_service_run_serving_fail_closed_fixture_check(void);
int mem_service_run_pretraining_fail_closed_fixture_check(void);
int mem_service_run_typed_payload_fixture_check(void);
int mem_service_run_allocation_fixture_check(void);
int mem_service_run_provider_directory_fixture_check(void);
int mem_service_run_restore_policy_fixture_check(void);
int mem_service_run_upgrade_rollback_runtime_fixture_check(void);
int mem_service_run_compat_runtime_fixture_check(void);
int mem_service_run_compat_old_server_runtime_fixture_check(void);

#endif
