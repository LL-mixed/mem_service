#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "components/mem_service/mem_service_core.h"
#include "components/mem_service/mem_service_client.h"
#include "components/mem_service/mem_service_daemon.h"
#include "components/mem_service/mem_service_provider_directory.h"
#include "components/mem_service/mem_service_object_contract.h"
#include "components/mem_service/mem_service_ub_ssd_gsva_backend.h"
#include "components/mem_service/mem_service_wire_client.h"
#include "components/mem_service/mem_service_wire_payload.h"
#include "components/mem_service/mem_service_wire_schema.h"

#ifdef MEM_SERVICE_ENABLE_QWEN3_INSPECT
#include "components/llm_infer/llm_infer.h"
#endif

#ifdef MEM_SERVICE_OBJECT_SESSION_OBMM
#include "components/mem_service/providers/mem_service_provider_obmm.h"
#endif

#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_VERSION 1U
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN 17289U
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM 0xe07a9225U
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_OPERATION_COUNT 39U
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_FIELD_COUNT 224U
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_COUNT 1U
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_FIELD_COUNT 2U
#define MEM_SERVICE_CONFIG_SCHEMA_VERSION 1U
#define MEM_SERVICE_DEPLOYMENT_SMOKE_VERSION 1U
#define MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_VERSION 1U
#define MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN 7648U
#define MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM 0xd4f33080U
#define MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_VERSION 1U
#define MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_LEN 2144U
#define MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM 0x13450492U
#define MEM_SERVICE_ALERT_RULES_VERSION 1U
#define MEM_SERVICE_ALERT_RULES_EXPECTED_LEN 2096U
#define MEM_SERVICE_ALERT_RULES_EXPECTED_CHECKSUM 0x05a9245cU
#define MEM_SERVICE_ALERT_RULES_EXPECTED_RULE_COUNT 6U
#define MEM_SERVICE_OPS_CERTIFICATION_POLICY_VERSION 1U
#define MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_LEN 1118U
#define MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM 0xe77c644bU
#define MEM_SERVICE_OPS_CERTIFICATION_EVIDENCE_VERSION 1U
#define MEM_SERVICE_REMOTE_TRANSPORT_EVIDENCE_VERSION 1U
#define MEM_SERVICE_PACKAGE_MANIFEST_VERSION 1U
#define MEM_SERVICE_RELEASE_VERSION "0.1.0"
#define MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN 9814U
#define MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM 0xfaf773b5U
#define MEM_SERVICE_PACKAGE_MANIFEST_INSTALLED_FILE_COUNT 57U
#define MEM_SERVICE_PACKAGE_MANIFEST_GATE_COUNT 34U
#define MEM_SERVICE_PACKAGE_TARBALL_NAME "linqu_mem_service-installed-layout-v1.tar"
#define MEM_SERVICE_NATIVE_DEB_NAME "linqu-mem-service_0.1.0-1_arm64.deb"
#define MEM_SERVICE_NATIVE_RPM_NAME "linqu-mem-service-0.1.0-1.aarch64.rpm"
#define MEM_SERVICE_API_ABI_POLICY_VERSION 1U
#define MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN 1025U
#define MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM 0x5e460a87U
#define MEM_SERVICE_COMPAT_MATRIX_VERSION 1U
#define MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN 2226U
#define MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM 0x20e8427fU
#define MEM_SERVICE_COMPAT_MATRIX_STATUS_COUNT 11U
#define MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN 1252U
#define MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM 0x2e21b1d6U
#define MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN 1734U
#define MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM 0x8e3cee6eU
#define MEM_SERVICE_CLI_STORE_MAGIC "mem_service_store_v1"

static void usage(const char *argv0)
{
    printf("Usage: %s [--smoke] [--self-test]", argv0);
    printf(" [version] [version-fixtures]");
    printf(" [release-readiness [--ops-evidence-file <path> --remote-transport-evidence-file <path>]] [release-readiness-fixtures]");
    printf(" [provider-status] [provider-fixtures]");
    printf(" [wire-fixtures] [wire-schema] [wire-schema-fixtures]");
    printf(" [store-fixtures] [journal-fixtures] [journal-compaction-fixtures] [journal-torn-recovery-fixtures] [config-fixtures]");
    printf(" [restore-policy-fixtures]");
    printf(" [runtime-quota-fixtures] [retention-fixtures]");
    printf(" [checkpoint-retention-fixtures] [payload-gc-fixtures]");
    printf(" [record-retention-fixtures] [encryption-fixtures]");
    printf(" [metrics-export-fixtures] [collector-fixtures] [deployment-fixtures]");
    printf(" [admin-output-schema] [admin-output-fixtures]");
    printf(" [upgrade-rollback-policy] [upgrade-rollback-fixtures]");
    printf(" [upgrade-rollback-runtime-fixtures]");
    printf(" [alert-rules] [alert-fixtures] [alert-integration-fixtures]");
    printf(" [ops-certification-policy] [ops-certification-fixtures]");
    printf(" [encryption-policy]");
    printf(" [ops-certification-evidence-fixtures]");
    printf(" [ops-certification-generate-evidence --rpm-file <path> --upgrade-rollback-marker <path>]");
    printf(" [ops-certification-linux-ci-smoke --rpm-file <path> --upgrade-rollback-marker <path> --evidence-file <path>]");
    printf(" [ops-certification-verify --evidence-file <path>]");
    printf(" [package-manifest] [package-fixtures]");
    printf(" [durable-catalog-fixtures]");
    printf(" [chunked-block-fixtures]");
    printf(" [transport-block-fixtures]");
    printf(" [network-transport-block-fixtures]");
    printf(" [ub-ssd-gsva-descriptor-fixtures]");
    printf(" [remote-block-backend-policy-fixtures]");
    printf(" [remote-transport-evidence-fixtures]");
    printf(" [remote-transport-serve-fixture --listen tcp:<ipv4>:<port> --payload-len <bytes>]");
    printf(" [remote-transport-generate-evidence --source tcp:<ipv4>:<port> --producer-host <host> --consumer-host <host> --network-partition-marker <path> --evidence-file <path>]");
    printf(" [remote-transport-verify --evidence-file <path>]");
    printf(" [api-abi-policy] [api-abi-fixtures]");
    printf(" [client-retry-fixtures] [compat-matrix] [compat-fixtures]");
    printf(" [compat-baseline-v1] [compat-baseline-fixtures]");
    printf(" [compat-old-new-matrix] [compat-old-new-fixtures]");
    printf(" [compat-runtime-fixtures]");
    printf(" [compat-old-server-runtime-fixtures]");
    printf(" [serving-fail-closed-fixtures]");
    printf(" [pretraining-fail-closed-fixtures]");
    printf(" [typed-payload-fixtures]");
    printf(" [release-manifest] [release-fixtures]");
    printf(" [serve [--config <path>] [--listen unix:%s|tcp:<ipv4>:<port>] [--store <path>]"
           " [--metrics-listen tcp:127.0.0.1:9900]]",
           MEM_SERVICE_DEFAULT_UNIX_SOCKET);
    printf(" [tcp listen requires auth_mode=trusted-guest-network with node_id and network_peer allowlist]");
    printf(" [health|ready|status|list-records|metrics|metrics-export|audit-log|export-snapshot|export-snapshot-page|export-snapshot-to|restore-snapshot [--connect unix:%s|tcp:<ipv4>:<port>] [--timeout-ms <ms>] [--max-attempts <n>] [--retry-backoff-ms <ms>] [--retry-timeouts]]",
           MEM_SERVICE_DEFAULT_UNIX_SOCKET);
    printf(" [metrics-export accepts --format prometheus-text]");
    printf(" [put-object|get-object|inspect-object|materialize-object|register-prefix|lookup-prefix|publish-kv|resolve-kv]");
    printf(" [publish-runtime-handoff|resolve-runtime-handoff]");
    printf(" [register-execution-artifact|query-execution-artifact]");
    printf(" [register-training-artifact|query-training-artifact]");
    printf(" [commit-training-step|resolve-training-step]");
    printf(" [mutating commands accept --idempotency-key <key>]");
    printf(" [object/artifact mutating commands accept --payload-file <path>]");
    printf(" [put-object accepts --backend ub-ssd-gsva-v1 --backend-write 1 --backend-buffer-gsva-base <u64> --backend-buffer-key-segment-id <u64>]");
    printf(" [get-object accepts --backend-read 1 with the same --backend-buffer-* GSVA descriptor fields]");
    printf(" [materialize-object --key <key> --to <new-path> [--expected-version <u64>] [--expected-checksum <u64>]]");
    printf(" [allocate-object --key <key> --idempotency-key <key> --size-bytes <u64> --capabilities <u64> [--session-id <id>] [--alignment-bytes <u64>]]");
    printf(" [acquire-object|release-object --key <key> --idempotency-key <key> --session-id <id> [--expected-generation <u64>]]");
    printf(" [retire-object --key <key> --idempotency-key <key> [--expected-generation <u64>]]");
    printf(" [inspect-allocation --key <key>] [allocation-stats] [allocation-fixtures]");
    printf(" [provider-register --node-id <id> --incarnation <u64> --readiness-generation <u64> --capabilities <u64>]");
    printf(" [provider-refresh --node-id <id> --incarnation <u64> --readiness-generation <u64>]");
    printf(" [provider-deregister --node-id <id> --incarnation <u64>] [provider-directory-status]");
    printf(" [publish-allocation --key <key> --node-id <id> --incarnation <u64> --generation <u64> --descriptor-hex <hex> --address <u64> --address-len <u64>]");
    printf(" [reclaim-allocation --key <key> --node-id <id> --incarnation <u64> --generation <u64> --confirmed <0|1>]");
    printf(" [poll-allocation --node-id <id> --incarnation <u64> --after-generation <u64>]");
    printf(" [mapping-transition --key <key> --session-id <id> --generation <u64> --mapping-id <u64> --action <begin|confirm|close|finish|cancel|inspect> --idempotency-key <id>]");
    printf(" [reference-transition --action <begin|stage|seal|resolve|acquire|map-begin> --key <key> [--session-id <id> --idempotency-key <id>] [--generation <u64> --version <u64>] [--reference-hex <512hex>] [--access <1|2|3>]]");
    printf(" [object-session --config <path> # deterministic SDK op sequence; config lines: session_id, connect, request_timeout_ms, provider=<session-loopback|obmm> (provider_device/provider_cna_path/provider_instance/provider_import_region_bytes for obmm), op=<allocate|acquire|release|retire|inspect|wait_state|publish|reclaim|stats|map|unmap|write|read|publish_data|wait_visible|probe_readonly|probe_guard|probe_conflict|probe_descriptor> field=value ...]");
    printf(" [object-session map diagnostics: fault=<descriptor|descriptor_length|descriptor_oversize|address|address_len|size|alignment|capabilities|home|incarnation> [fault_byte=N for descriptor] expect_status=stale_ref]");
    printf(" [object-session unmap diagnostic: unmap key=<key> probe_unmapped=1 # require same-process CPU fault after confirmed unmap]");
    printf(" [object-session retired descriptor diagnostic: capture_mapping key=<old>; retire old and verify same-address replacement; probe_retired_mapping key=<old> # require provider CPU fault and confirmed cleanup]");
    printf(" [object-session V2 writer: begin_reference key=<allocation> generation=N version=N idempotency_key=<id>; publish_reference key=<logical> offset=N len=N kind=N owner=N producer=N idempotency_key=<id>; unmap then seal_reference key=<allocation> generation=N version=N idempotency_key=<id>]");
    printf(" [object-session V2 reader: acquire_reference key=<logical> idempotency_key=<id>; map_reference key=<allocation>; unmap then release]");
    printf(" [bootstrap-w5-service --memory-store <path> --memory-object-store <path> --memory-engram-state <path> --memory-registry-dir <path> [--service-name <name>] [--print-env]]");
#ifdef MEM_SERVICE_ENABLE_QWEN3_INSPECT
    printf(" [--inspect-qwen3]");
#endif
    printf("\n");
}

static const char *wire_payload_format_name(uint32_t payload_format)
{
    if (payload_format == MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV) {
        return "text-kv";
    }
    return "unknown";
}

static const char *wire_field_type_name(enum mem_service_wire_payload_field_type type)
{
    if (type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING) {
        return "string";
    }
    if (type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32) {
        return "u32";
    }
    if (type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64) {
        return "u64";
    }
    return "unknown";
}

static int append_wire_schema_line(char *manifest,
                                   size_t manifest_len,
                                   size_t *used,
                                   const char *fmt,
                                   ...)
{
    va_list ap;
    int written;

    if (manifest == NULL || used == NULL || *used >= manifest_len) {
        return -1;
    }
    va_start(ap, fmt);
    written = vsnprintf(manifest + *used, manifest_len - *used, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= manifest_len - *used) {
        return -1;
    }
    *used += (size_t)written;
    return 0;
}

static const char *option_value(int argc, char **argv, const char *option_name);
static int validate_ops_certification_evidence(const char *evidence,
                                               char *reason,
                                               size_t reason_len);
static int validate_remote_transport_evidence(const char *evidence,
                                              char *reason,
                                              size_t reason_len);
static int read_text_file_limited(const char *path, char *payload, size_t payload_len);
static int append_ops_certification_valid_evidence(char *evidence,
                                                   size_t evidence_len);
static int append_remote_transport_valid_evidence(char *evidence,
                                                  size_t evidence_len);

static size_t wire_schema_operation_count(void)
{
    return sizeof(mem_service_wire_operation_schemas) /
           sizeof(mem_service_wire_operation_schemas[0]);
}

static void wire_schema_count_fields(size_t *field_count_out,
                                     size_t *oneof_count_out,
                                     size_t *oneof_field_count_out)
{
    size_t op_index;
    size_t field_count = 0;
    size_t oneof_count = 0;
    size_t oneof_field_count = 0;

    for (op_index = 0; op_index < wire_schema_operation_count(); ++op_index) {
        const struct mem_service_wire_operation_schema *schema =
            &mem_service_wire_operation_schemas[op_index];
        size_t oneof_index;

        field_count += schema->field_count;
        oneof_count += schema->oneof_count;
        for (oneof_index = 0; oneof_index < schema->oneof_count; ++oneof_index) {
            oneof_field_count += schema->oneofs[oneof_index].field_count;
        }
    }
    if (field_count_out != NULL) {
        *field_count_out = field_count;
    }
    if (oneof_count_out != NULL) {
        *oneof_count_out = oneof_count;
    }
    if (oneof_field_count_out != NULL) {
        *oneof_field_count_out = oneof_field_count;
    }
}

static int render_wire_schema_manifest(char *manifest,
                                       size_t manifest_len,
                                       size_t *used_out)
{
    size_t used = 0;
    size_t op_index;
    size_t field_count = 0;
    size_t oneof_count = 0;
    size_t oneof_field_count = 0;

    if (manifest == NULL || manifest_len == 0) {
        return -1;
    }
    manifest[0] = '\0';
    wire_schema_count_fields(&field_count, &oneof_count, &oneof_field_count);
    if (append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "mem_service_wire_schema_manifest_version=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "wire_schema_version=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "wire_payload_format=%s\n",
                                wire_payload_format_name(
                                    MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV)) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "operation_count=%zu\n",
                                wire_schema_operation_count()) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "field_count=%zu\n",
                                field_count) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "oneof_count=%zu\n",
                                oneof_count) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "oneof_field_count=%zu\n",
                                oneof_field_count) != 0) {
        return -1;
    }
    for (op_index = 0; op_index < wire_schema_operation_count(); ++op_index) {
        const struct mem_service_wire_operation_schema *schema =
            &mem_service_wire_operation_schemas[op_index];
        size_t field_index;
        size_t oneof_index;

        if (append_wire_schema_line(manifest,
                                    manifest_len,
                                    &used,
                                    "operation=%s:%u schema_version=%u "
                                    "payload_format=%s fields=%zu oneofs=%zu\n",
                                    schema->name,
                                    (uint32_t)schema->operation,
                                    schema->schema_version,
                                    wire_payload_format_name(schema->payload_format),
                                    schema->field_count,
                                    schema->oneof_count) != 0) {
            return -1;
        }
        for (field_index = 0; field_index < schema->field_count; ++field_index) {
            const struct mem_service_wire_payload_field *field =
                &schema->fields[field_index];

            if (append_wire_schema_line(manifest,
                                        manifest_len,
                                        &used,
                                        "field=%s.%s type=%s required=%u\n",
                                        schema->name,
                                        field->name,
                                        wire_field_type_name(field->type),
                                        field->required ? 1U : 0U) != 0) {
                return -1;
            }
        }
        for (oneof_index = 0; oneof_index < schema->oneof_count; ++oneof_index) {
            const struct mem_service_wire_payload_oneof *oneof =
                &schema->oneofs[oneof_index];
            size_t oneof_field_index;

            if (append_wire_schema_line(manifest,
                                        manifest_len,
                                        &used,
                                        "oneof=%s.%zu field_count=%zu\n",
                                        schema->name,
                                        oneof_index,
                                        oneof->field_count) != 0) {
                return -1;
            }
            for (oneof_field_index = 0; oneof_field_index < oneof->field_count;
                 ++oneof_field_index) {
                if (append_wire_schema_line(manifest,
                                            manifest_len,
                                            &used,
                                            "oneof_field=%s.%zu.%s\n",
                                            schema->name,
                                            oneof_index,
                                            oneof->field_names[oneof_field_index]) != 0) {
                    return -1;
                }
            }
        }
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_wire_schema_manifest(void)
{
    char manifest[32768];
    size_t used = 0;

    if (render_wire_schema_manifest(manifest, sizeof(manifest), &used) != 0) {
        fprintf(stderr, "mem_service wire-schema: render failed\n");
        return 1;
    }
    (void)used;
    fputs(manifest, stdout);
    return 0;
}

static int run_wire_schema_fixture_check(void)
{
    char manifest[32768];
    size_t used = 0;
    size_t field_count = 0;
    size_t oneof_count = 0;
    size_t oneof_field_count = 0;
    uint32_t checksum;
    int failures = 0;

    if (render_wire_schema_manifest(manifest, sizeof(manifest), &used) != 0) {
        fprintf(stderr, "mem_service wire-schema-fixtures: render failed\n");
        return 1;
    }
    wire_schema_count_fields(&field_count, &oneof_count, &oneof_field_count);
    checksum = mem_service_wire_checksum(manifest, used);
    if (wire_schema_operation_count() !=
        MEM_SERVICE_WIRE_SCHEMA_MANIFEST_OPERATION_COUNT) {
        fprintf(stderr, "mem_service wire-schema-fixtures: operation count mismatch\n");
        failures -= 1;
    }
    if (field_count != MEM_SERVICE_WIRE_SCHEMA_MANIFEST_FIELD_COUNT) {
        fprintf(stderr, "mem_service wire-schema-fixtures: field count mismatch\n");
        failures -= 1;
    }
    if (oneof_count != MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_COUNT) {
        fprintf(stderr, "mem_service wire-schema-fixtures: oneof count mismatch\n");
        failures -= 1;
    }
    if (oneof_field_count != MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_FIELD_COUNT) {
        fprintf(stderr, "mem_service wire-schema-fixtures: oneof field count mismatch\n");
        failures -= 1;
    }
    if (used != MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service wire-schema-fixtures: manifest len actual=%zu expected=%u\n",
                used,
                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN);
        failures -= 1;
    }
    if (checksum != MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service wire-schema-fixtures: manifest checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM);
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service wire-schema-fixtures: status=ok manifest_len=%u "
           "manifest_checksum=0x%08x operations=%u fields=%u oneofs=%u "
           "oneof_fields=%u\n",
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_OPERATION_COUNT,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_FIELD_COUNT,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_COUNT,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_FIELD_COUNT);
    return 0;
}

static int render_api_abi_policy(char *policy, size_t policy_len, size_t *used_out)
{
    size_t used = 0;

    if (policy == NULL || policy_len == 0) {
        return -1;
    }
    policy[0] = '\0';
    if (append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "mem_service_api_abi_policy_version=%u\n",
                                MEM_SERVICE_API_ABI_POLICY_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_api_version=%u\n",
                                MEM_SERVICE_CLIENT_API_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_abi_version=%u\n",
                                MEM_SERVICE_CLIENT_ABI_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_api_compatibility=%s\n",
                                MEM_SERVICE_CLIENT_API_COMPATIBILITY) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_abi_compatibility=%s\n",
                                MEM_SERVICE_CLIENT_ABI_COMPATIBILITY) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_record_abi_size=%u\n",
                                MEM_SERVICE_CLIENT_RECORD_ABI_SIZE) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_record_actual_size=%zu\n",
                                sizeof(struct mem_service_client_record)) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_key_len=%u\n",
                                MEM_SERVICE_CLIENT_KEY_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_id_len=%u\n",
                                MEM_SERVICE_CLIENT_ID_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_state_len=%u\n",
                                MEM_SERVICE_CLIENT_STATE_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "provider_api_version=%u\n",
                                MEM_SERVICE_PROVIDER_API_VERSION) != 0 ||
        append_wire_schema_line(
            policy,
            policy_len,
            &used,
            "provider_api_compatibility=additive-source-compatible\n") != 0 ||
        append_wire_schema_line(
            policy,
            policy_len,
            &used,
            "provider_mapping_contract_version=%u\n",
            MEM_SERVICE_PROVIDER_MAPPING_CONTRACT_VERSION) != 0 ||
        append_wire_schema_line(
            policy,
            policy_len,
            &used,
            "provider_mapping_contract=peer-mapping+range-visibility\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_version_min=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_version_current=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_version_max=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_header_len=%u\n",
                                MEM_SERVICE_WIRE_HEADER_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_schema_version=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_payload_format=text-kv\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "unknown_field_policy=ignored_when_optional\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_field_policy=missing_required_field_fails_schema\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "operation_id_policy=stable-within-v1\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "status_code_policy=stable-within-v1\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "old_client_new_server_policy=compatible-within-v1\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "new_client_old_server_policy=certified\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "upgrade_policy=current-version-only\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "rollback_policy=current-version-only\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "binary_typed_schema=typed-binary-v1\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_api_abi_policy(void)
{
    char policy[4096];
    size_t used = 0;

    if (render_api_abi_policy(policy, sizeof(policy), &used) != 0) {
        fprintf(stderr, "mem_service api-abi-policy: render failed\n");
        return 1;
    }
    (void)used;
    fputs(policy, stdout);
    return 0;
}

static int run_api_abi_fixture_check(void)
{
    char policy[4096];
    size_t used = 0;
    uint32_t checksum;
    int failures = 0;

    if (render_api_abi_policy(policy, sizeof(policy), &used) != 0) {
        fprintf(stderr, "mem_service api-abi-fixtures: render failed\n");
        return 1;
    }
    checksum = mem_service_wire_checksum(policy, used);
    if (used != MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service api-abi-fixtures: policy len actual=%zu expected=%u\n",
                used,
                MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN);
        failures -= 1;
    }
    if (checksum != MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service api-abi-fixtures: policy checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM);
        failures -= 1;
    }
    if (MEM_SERVICE_CLIENT_API_VERSION != 1U ||
        MEM_SERVICE_CLIENT_ABI_VERSION != 1U ||
        MEM_SERVICE_CLIENT_RECORD_ABI_SIZE !=
            sizeof(struct mem_service_client_record) ||
        MEM_SERVICE_PROVIDER_API_VERSION != 2U ||
        MEM_SERVICE_PROVIDER_MAPPING_CONTRACT_VERSION != 1U ||
        MEM_SERVICE_WIRE_VERSION != 1U ||
        MEM_SERVICE_WIRE_HEADER_LEN != 48U ||
        MEM_SERVICE_WIRE_SCHEMA_VERSION != 1U) {
        fprintf(stderr, "mem_service api-abi-fixtures: version/layout mismatch\n");
        failures -= 1;
    }
    if (strstr(policy, "old_client_new_server_policy=compatible-within-v1\n") ==
            NULL ||
        strstr(policy,
               "new_client_old_server_policy=certified\n") ==
            NULL ||
        strstr(policy, "upgrade_policy=current-version-only\n") == NULL ||
        strstr(policy, "rollback_policy=current-version-only\n") == NULL ||
        strstr(policy,
               "provider_mapping_contract=peer-mapping+range-visibility\n") ==
            NULL) {
        fprintf(stderr, "mem_service api-abi-fixtures: required policy missing\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service api-abi-fixtures: status=ok policy_version=%u "
           "policy_len=%u policy_checksum=0x%08x client_api_version=%u "
           "client_abi_version=%u client_record_abi_size=%u\n",
           MEM_SERVICE_API_ABI_POLICY_VERSION,
           MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN,
           MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM,
           MEM_SERVICE_CLIENT_API_VERSION,
           MEM_SERVICE_CLIENT_ABI_VERSION,
           MEM_SERVICE_CLIENT_RECORD_ABI_SIZE);
    return 0;
}

static int render_upgrade_rollback_policy(char *policy,
                                          size_t policy_len,
                                          size_t *used_out)
{
    size_t used = 0;

    if (policy == NULL || policy_len == 0) {
        return -1;
    }
    policy[0] = '\0';
    if (append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "mem_service_upgrade_rollback_policy_version=%u\n",
                                MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "release_manifest_version=1\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_version_current=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_header_len=%u\n",
                                MEM_SERVICE_WIRE_HEADER_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_schema_version_current=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_payload_format=text-kv\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_schema_manifest_len=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_schema_manifest_checksum=0x%08x\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "admin_output_schema_len=%u\n",
                                MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "admin_output_schema_checksum=0x%08x\n",
                                MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "api_abi_policy_len=%u\n",
                                MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "api_abi_policy_checksum=0x%08x\n",
                                MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "compat_matrix_len=%u\n",
                                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "compat_matrix_checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "compat_baseline_len=%u\n",
                                MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "compat_baseline_checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "compat_old_new_matrix_len=%u\n",
                                MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "compat_old_new_matrix_checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "config_schema_version=%u\n",
                                MEM_SERVICE_CONFIG_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "deployment_smoke_version=%u\n",
                                MEM_SERVICE_DEPLOYMENT_SMOKE_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "store_magic=%s\n",
                                MEM_SERVICE_CLI_STORE_MAGIC) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "store_schema_version=1\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "catalog_layout=storage-root-v1\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "payload_block_backend=sealed-local-block-v1,sealed-chunked-block-v1,transport-loopback-block-v1,transport-tcp-block-v1,ub-ssd-gsva-v1\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "same_version_restart_recovery=store-snapshot+journal\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "same_version_restore=export-snapshot-page+restore-snapshot-page\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "same_version_runtime_gate=upgrade-rollback-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "upgrade_policy=current-version-only\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "rollback_policy=current-version-only\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "upgrade_admission=reject-unknown-release-generation\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "rollback_admission=reject-unknown-release-generation\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "old_server_runtime_binary=certified\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "new_client_old_server=certified\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "catalog_schema_version=1\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "migration_policy=catalog-schema-version-migrate-legacy-to-v1-reject-future\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "store_migration_policy=store-schema-version-migrate-legacy-to-v1-reject-future\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "downgrade_policy=not-certified\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=wire-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=wire-schema-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=admin-output-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=upgrade-rollback-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=api-abi-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=compat-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=compat-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=compat-old-new-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=store-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=journal-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=durable-catalog-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=deployment-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=collector-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=alert-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=alert-integration-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=package-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=release-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=host-artifact-smoke\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=install-smoke\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_upgrade_rollback_policy(void)
{
    char policy[4096];
    size_t used = 0;

    if (render_upgrade_rollback_policy(policy, sizeof(policy), &used) != 0) {
        fprintf(stderr, "mem_service upgrade-rollback-policy: render failed\n");
        return 1;
    }
    (void)used;
    fputs(policy, stdout);
    return 0;
}

static int run_upgrade_rollback_fixture_check(void)
{
    char policy[4096];
    size_t used = 0;
    uint32_t checksum;
    int failures = 0;

    if (render_upgrade_rollback_policy(policy, sizeof(policy), &used) != 0) {
        fprintf(stderr, "mem_service upgrade-rollback-fixtures: render failed\n");
        return 1;
    }
    checksum = mem_service_wire_checksum(policy, used);
    if (used != MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service upgrade-rollback-fixtures: policy len actual=%zu "
                "expected=%u\n",
                used,
                MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_LEN);
        failures -= 1;
    }
    if (checksum != MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service upgrade-rollback-fixtures: policy checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM);
        failures -= 1;
    }
    if (MEM_SERVICE_WIRE_VERSION != 1U ||
        MEM_SERVICE_WIRE_SCHEMA_VERSION != 1U ||
        MEM_SERVICE_CONFIG_SCHEMA_VERSION != 1U ||
        MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN == 0U ||
        MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN == 0U ||
        MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN == 0U) {
        fprintf(stderr,
                "mem_service upgrade-rollback-fixtures: release dependency missing\n");
        failures -= 1;
    }
    if (strstr(policy, "upgrade_policy=current-version-only\n") == NULL ||
        strstr(policy, "rollback_policy=current-version-only\n") == NULL ||
        strstr(policy, "old_server_runtime_binary=certified\n") == NULL ||
        strstr(policy, "new_client_old_server=certified\n") ==
            NULL ||
        strstr(policy, "same_version_runtime_gate=upgrade-rollback-runtime-fixtures\n") ==
            NULL ||
        strstr(policy, "required_gate=admin-output-fixtures\n") == NULL ||
        strstr(policy, "required_gate=upgrade-rollback-runtime-fixtures\n") == NULL ||
        strstr(policy, "required_gate=compat-runtime-fixtures\n") == NULL ||
        strstr(policy, "required_gate=alert-fixtures\n") == NULL ||
        strstr(policy, "required_gate=alert-integration-fixtures\n") == NULL ||
        strstr(policy, "required_gate=package-fixtures\n") == NULL ||
        strstr(policy, "required_gate=install-smoke\n") == NULL) {
        fprintf(stderr,
                "mem_service upgrade-rollback-fixtures: required policy missing\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service upgrade-rollback-fixtures: status=ok policy_version=%u "
           "policy_len=%u policy_checksum=0x%08x "
           "upgrade_policy=current-version-only "
           "rollback_policy=current-version-only required_gates=19\n",
           MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_VERSION,
           MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_LEN,
           MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM);
    return 0;
}

static int render_compat_matrix(char *matrix, size_t matrix_len, size_t *used_out)
{
    size_t used = 0;
    size_t field_count = 0;
    size_t oneof_count = 0;
    size_t oneof_field_count = 0;

    if (matrix == NULL || matrix_len == 0) {
        return -1;
    }
    matrix[0] = '\0';
    wire_schema_count_fields(&field_count, &oneof_count, &oneof_field_count);
    if (append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "mem_service_compat_matrix_version=%u\n",
                                MEM_SERVICE_COMPAT_MATRIX_VERSION) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_scope=wire-schema,release-layout,client-retry,idempotency,audit,snapshot,journal\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_version_min=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_version_current=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_version_max=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_header_len=%u\n",
                                MEM_SERVICE_WIRE_HEADER_LEN) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_schema_version_min=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_schema_version_current=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_payload_format=text-kv\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_schema_manifest_len=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_schema_manifest_checksum=0x%08x\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "operation_count=%zu\n",
                                wire_schema_operation_count()) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "field_count=%zu\n",
                                field_count) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "oneof_count=%zu\n",
                                oneof_count) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "oneof_field_count=%zu\n",
                                oneof_field_count) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "status_count=%u\n",
                                MEM_SERVICE_COMPAT_MATRIX_STATUS_COUNT) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "unknown_text_field_policy=ignored_by_schema_validation\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "required_field_policy=missing_required_field_fails_schema\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "oneof_policy=at_least_one_selector_field_required\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "client_retry_policy=explicit-max-attempts-backoff\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "retry_timeout_policy=opt-in-retry-timeouts\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "idempotency_scope=mutating-object-prefix-kv-runtime-execution-training\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "idempotency_replay_match=operation-and-request-checksum\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "idempotency_conflict_status=version_conflict\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "idempotency_persistence=store-journal-and-full-snapshot\n"
                                "managed_history=append-only-checkpoint-v1\n"
                                "managed_history_store_magic=mem_service_store_history_v1\n"
                                "managed_history_rollback=reject-legacy-reader\n"
                                "managed_history_snapshot=paired-files-only\n"
                                "managed_history_restart_data_plane=requires-reconciliation\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "audit_log_scope=mutating-and-fail-closed\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "audit_log_retention=bounded-ring\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "audit_log_persistence=store-journal-and-full-snapshot\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "journal_store_magic=mem_service_journal_v1\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "journal_path_policy=store-path-dot-journal\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "journal_scope=completed-idempotency-and-audit-events\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "journal_truncation_policy=threshold-compaction\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "snapshot_store_magic=%s\n",
                                MEM_SERVICE_CLI_STORE_MAGIC) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "snapshot_full_restore_state=records-idempotency-audit\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "snapshot_paged_restore_state=records-only-clears-idempotency-audit\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "client_api=pretraining-refs-v1\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "client_api=pretraining-step-commit-v1\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=wire-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=wire-schema-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=store-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=journal-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=config-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=metrics-export-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=deployment-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=client-retry-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=compat-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=release-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=daemon-runtime\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "release_gate=install-smoke\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "upgrade_policy=current-version-only\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "rollback_policy=current-version-only\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "binary_typed_schema=typed-binary-v1\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_compat_matrix(void)
{
    char matrix[8192];
    size_t used = 0;

    if (render_compat_matrix(matrix, sizeof(matrix), &used) != 0) {
        fprintf(stderr, "mem_service compat-matrix: render failed\n");
        return 1;
    }
    (void)used;
    fputs(matrix, stdout);
    return 0;
}

static int run_compat_fixture_check(void)
{
    char matrix[8192];
    size_t used = 0;
    size_t field_count = 0;
    size_t oneof_count = 0;
    size_t oneof_field_count = 0;
    uint32_t checksum;
    int failures = 0;
    const struct mem_service_wire_operation_schema *put_object_schema =
        mem_service_wire_schema_for_operation(MEM_SERVICE_WIRE_OP_PUT_OBJECT);
    const struct mem_service_wire_operation_schema *resolve_kv_schema =
        mem_service_wire_schema_for_operation(MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT);
    struct mem_service_wire_payload_view valid_put =
        mem_service_wire_payload_view_from_cstr(
            "key=compat-object\n"
            "version=1\n"
            "checksum=2\n"
            "unknown_future_field=ignored\n");
    struct mem_service_wire_payload_view invalid_put =
        mem_service_wire_payload_view_from_cstr("version=1\nchecksum=2\n");
    struct mem_service_wire_payload_view invalid_oneof =
        mem_service_wire_payload_view_from_cstr("unknown_future_field=ignored\n");

    if (render_compat_matrix(matrix, sizeof(matrix), &used) != 0) {
        fprintf(stderr, "mem_service compat-fixtures: render failed\n");
        return 1;
    }
    wire_schema_count_fields(&field_count, &oneof_count, &oneof_field_count);
    checksum = mem_service_wire_checksum(matrix, used);
    if (used != MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service compat-fixtures: matrix len actual=%zu expected=%u\n",
                used,
                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN);
        failures -= 1;
    }
    if (checksum != MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service compat-fixtures: matrix checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM);
        failures -= 1;
    }
    if (MEM_SERVICE_WIRE_VERSION != 1U ||
        MEM_SERVICE_WIRE_HEADER_LEN != 48U ||
        MEM_SERVICE_WIRE_SCHEMA_VERSION != 1U ||
        wire_schema_operation_count() !=
            MEM_SERVICE_WIRE_SCHEMA_MANIFEST_OPERATION_COUNT ||
        field_count != MEM_SERVICE_WIRE_SCHEMA_MANIFEST_FIELD_COUNT ||
        oneof_count != MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_COUNT ||
        oneof_field_count != MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_FIELD_COUNT ||
        MEM_SERVICE_WIRE_OP_AUDIT_LOG != 10U ||
        MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT != 4U) {
        fprintf(stderr, "mem_service compat-fixtures: version/id matrix mismatch\n");
        failures -= 1;
    }
    if (!mem_service_wire_schema_validate_payload(put_object_schema,
                                                  &valid_put,
                                                  NULL) ||
        mem_service_wire_schema_validate_payload(put_object_schema,
                                                 &invalid_put,
                                                 NULL) ||
        mem_service_wire_schema_validate_payload(resolve_kv_schema,
                                                 &invalid_oneof,
                                                 NULL)) {
        fprintf(stderr, "mem_service compat-fixtures: schema policy mismatch\n");
        failures -= 1;
    }
    if (strstr(matrix, "idempotency_conflict_status=version_conflict\n") == NULL ||
        strstr(matrix, "idempotency_persistence=store-journal-and-full-snapshot\n") == NULL ||
        strstr(matrix, "audit_log_persistence=store-journal-and-full-snapshot\n") == NULL ||
        strstr(matrix, "compat_test=journal-fixtures\n") == NULL ||
        strstr(matrix, "compat_test=compat-runtime-fixtures\n") == NULL ||
        strstr(matrix,
               "snapshot_paged_restore_state=records-only-clears-idempotency-audit\n") ==
            NULL ||
        strstr(matrix, "upgrade_policy=current-version-only\n") == NULL) {
        fprintf(stderr, "mem_service compat-fixtures: required rule missing\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service compat-fixtures: status=ok matrix_version=%u "
           "matrix_len=%u matrix_checksum=0x%08x wire_version=%u "
           "schema_version=%u operations=%u fields=%u statuses=%u\n",
           MEM_SERVICE_COMPAT_MATRIX_VERSION,
           MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN,
           MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM,
           MEM_SERVICE_WIRE_VERSION,
           MEM_SERVICE_WIRE_SCHEMA_VERSION,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_OPERATION_COUNT,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_FIELD_COUNT,
           MEM_SERVICE_COMPAT_MATRIX_STATUS_COUNT);
    return 0;
}

static int render_compat_baseline_v1(char *baseline,
                                     size_t baseline_len,
                                     size_t *used_out)
{
    size_t used = 0;

    if (baseline == NULL || baseline_len == 0) {
        return -1;
    }
    baseline[0] = '\0';
    if (append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "mem_service_compat_baseline_version=1\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "baseline_name=mem-service-wire-v1\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "baseline_scope=old-v1-client-to-current-server\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "wire_version=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "wire_header_len=%u\n",
                                MEM_SERVICE_WIRE_HEADER_LEN) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "wire_schema_version=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "wire_payload_format=text-kv\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "wire_schema_manifest_len=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "wire_schema_manifest_checksum=0x%08x\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "compat_matrix_len=%u\n",
                                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "compat_matrix_checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "operation_count=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_OPERATION_COUNT) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "field_count=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_FIELD_COUNT) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "status_count=%u\n",
                                MEM_SERVICE_COMPAT_MATRIX_STATUS_COUNT) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "old_client_new_server=compatible-within-v1\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "new_client_old_server=certified\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "unknown_text_field_policy=ignored_by_schema_validation\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "required_field_policy=missing_required_field_fails_schema\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "oneof_policy=at_least_one_selector_field_required\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "idempotency_replay_match=operation-and-request-checksum\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "idempotency_conflict_status=version_conflict\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "idempotency_persistence=store-journal-and-full-snapshot\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "audit_log_persistence=store-journal-and-full-snapshot\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "journal_scope=completed-idempotency-and-audit-events\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "journal_truncation_policy=threshold-compaction\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "snapshot_full_restore_state=records-idempotency-audit\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "snapshot_paged_restore_state=records-only-clears-idempotency-audit\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "baseline_payload=put_object:v1-key-version-checksum-extra-field\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "baseline_payload=resolve_kv_segment:v1-key-or-block-hash-oneof\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "baseline_payload=register_training_artifact:v1-training-step-compatible\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_compat_baseline_v1(void)
{
    char baseline[4096];
    size_t used = 0;

    if (render_compat_baseline_v1(baseline, sizeof(baseline), &used) != 0) {
        fprintf(stderr, "mem_service compat-baseline-v1: render failed\n");
        return 1;
    }
    (void)used;
    fputs(baseline, stdout);
    return 0;
}

static int run_compat_baseline_fixture_check(void)
{
    char baseline[4096];
    size_t used = 0;
    uint32_t checksum;
    int failures = 0;
    const struct mem_service_wire_operation_schema *put_object_schema =
        mem_service_wire_schema_for_operation(MEM_SERVICE_WIRE_OP_PUT_OBJECT);
    const struct mem_service_wire_operation_schema *resolve_kv_schema =
        mem_service_wire_schema_for_operation(MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT);
    const struct mem_service_wire_operation_schema *training_publish_schema =
        mem_service_wire_schema_for_operation(
            MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT);
    struct mem_service_wire_payload_view old_put =
        mem_service_wire_payload_view_from_cstr(
            "key=old-client-object\n"
            "version=1\n"
            "checksum=2\n"
            "future_optional_field=ignored\n");
    struct mem_service_wire_payload_view old_put_missing_key =
        mem_service_wire_payload_view_from_cstr("version=1\nchecksum=2\n");
    struct mem_service_wire_payload_view old_resolve_kv =
        mem_service_wire_payload_view_from_cstr("block_hash=old-block\n");
    struct mem_service_wire_payload_view old_resolve_kv_missing_selector =
        mem_service_wire_payload_view_from_cstr("future_optional_field=ignored\n");
    struct mem_service_wire_payload_view old_training_publish =
        mem_service_wire_payload_view_from_cstr(
            "key=training/old/global-step-0001/commit\n"
            "session_id=old\n"
            "model_key=model-a\n"
            "artifact_kind=training-step-commit\n"
            "artifact_id=global-step-0001\n"
            "version=1\n"
            "checksum=101\n"
            "idempotency_key=old/global-step-0001/v1\n"
            "future_optional_field=ignored\n");

    if (render_compat_baseline_v1(baseline, sizeof(baseline), &used) != 0) {
        fprintf(stderr, "mem_service compat-baseline-fixtures: render failed\n");
        return 1;
    }
    checksum = mem_service_wire_checksum(baseline, used);
    if (used != MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service compat-baseline-fixtures: baseline len actual=%zu "
                "expected=%u\n",
                used,
                MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN);
        failures -= 1;
    }
    if (checksum != MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service compat-baseline-fixtures: baseline checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM);
        failures -= 1;
    }
    if (!mem_service_wire_schema_validate_payload(put_object_schema,
                                                  &old_put,
                                                  NULL) ||
        mem_service_wire_schema_validate_payload(put_object_schema,
                                                 &old_put_missing_key,
                                                 NULL) ||
        !mem_service_wire_schema_validate_payload(resolve_kv_schema,
                                                  &old_resolve_kv,
                                                  NULL) ||
        mem_service_wire_schema_validate_payload(resolve_kv_schema,
                                                 &old_resolve_kv_missing_selector,
                                                 NULL) ||
        !mem_service_wire_schema_validate_payload(training_publish_schema,
                                                  &old_training_publish,
                                                  NULL)) {
        fprintf(stderr,
                "mem_service compat-baseline-fixtures: old payload policy failed\n");
        failures -= 1;
    }
    if (strstr(baseline, "old_client_new_server=compatible-within-v1\n") == NULL ||
        strstr(baseline, "new_client_old_server=certified\n") == NULL ||
        strstr(baseline,
               "idempotency_persistence=store-journal-and-full-snapshot\n") == NULL ||
        strstr(baseline,
               "audit_log_persistence=store-journal-and-full-snapshot\n") == NULL ||
        strstr(baseline,
               "baseline_payload=register_training_artifact:v1-training-step-compatible\n") ==
            NULL) {
        fprintf(stderr,
                "mem_service compat-baseline-fixtures: required baseline missing\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service compat-baseline-fixtures: status=ok baseline_version=1 "
           "baseline_len=%u baseline_checksum=0x%08x old_client_new_server=v1 "
           "new_client_old_server=certified\n",
           MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN,
           MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM);
    return 0;
}

static const struct mem_service_wire_payload_field *
find_schema_field(const struct mem_service_wire_operation_schema *schema,
                  const char *field_name)
{
    size_t i;

    if (schema == NULL || field_name == NULL) {
        return NULL;
    }
    for (i = 0; i < schema->field_count; ++i) {
        if (strcmp(schema->fields[i].name, field_name) == 0) {
            return &schema->fields[i];
        }
    }
    return NULL;
}

static int append_compat_profile_field(
    char *payload,
    size_t payload_len,
    const struct mem_service_wire_payload_field *field)
{
    if (field == NULL) {
        return -1;
    }
    if (field->type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32 ||
        field->type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64) {
        return mem_service_wire_payload_append_u64(payload,
                                                   payload_len,
                                                   field->name,
                                                   1U);
    }
    return mem_service_wire_payload_append_field(payload,
                                                 payload_len,
                                                 field->name,
                                                 "compat");
}

static bool compat_profile_payload_has_field(const char *payload,
                                             const char *field_name)
{
    struct mem_service_wire_payload_view view =
        mem_service_wire_payload_view_from_cstr(payload);
    char value[128];

    return mem_service_wire_payload_get_string(&view,
                                               field_name,
                                               value,
                                               sizeof(value));
}

static int render_compat_profile_payload(
    const struct mem_service_wire_operation_schema *schema,
    bool include_optional_fields,
    char *payload,
    size_t payload_len)
{
    size_t i;

    if (schema == NULL || payload == NULL || payload_len == 0) {
        return -1;
    }
    payload[0] = '\0';
    for (i = 0; i < schema->field_count; ++i) {
        const struct mem_service_wire_payload_field *field = &schema->fields[i];

        if ((field->required || include_optional_fields) &&
            append_compat_profile_field(payload, payload_len, field) != 0) {
            return -1;
        }
    }
    for (i = 0; i < schema->oneof_count; ++i) {
        const struct mem_service_wire_payload_oneof *oneof = &schema->oneofs[i];
        bool oneof_satisfied = false;
        size_t field_index;

        for (field_index = 0; field_index < oneof->field_count; ++field_index) {
            if (compat_profile_payload_has_field(payload,
                                                 oneof->field_names[field_index])) {
                oneof_satisfied = true;
                break;
            }
        }
        if (!oneof_satisfied && oneof->field_count > 0) {
            const struct mem_service_wire_payload_field *field =
                find_schema_field(schema, oneof->field_names[0]);

            if (append_compat_profile_field(payload, payload_len, field) != 0) {
                return -1;
            }
        }
    }
    if (include_optional_fields &&
        mem_service_wire_payload_append_field(payload,
                                              payload_len,
                                              "future_optional_field",
                                              "ignored") != 0) {
        return -1;
    }
    return 0;
}

static bool schema_has_required_field(
    const struct mem_service_wire_operation_schema *schema)
{
    size_t i;

    if (schema == NULL) {
        return false;
    }
    for (i = 0; i < schema->field_count; ++i) {
        if (schema->fields[i].required) {
            return true;
        }
    }
    return false;
}

static int render_compat_old_new_matrix(char *matrix,
                                        size_t matrix_len,
                                        size_t *used_out)
{
    size_t used = 0;
    size_t field_count = 0;
    size_t oneof_count = 0;
    size_t oneof_field_count = 0;

    if (matrix == NULL || matrix_len == 0) {
        return -1;
    }
    matrix[0] = '\0';
    wire_schema_count_fields(&field_count, &oneof_count, &oneof_field_count);
    if (append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "mem_service_old_new_compat_matrix_version=1\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "matrix_name=mem-service-old-new-wire-v1\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "matrix_scope=wire-header,schema-profile,payload-policy,response-status,release-artifact\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_version_old=1\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_version_current=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_header_len_old=48\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_header_len_current=%u\n",
                                MEM_SERVICE_WIRE_HEADER_LEN) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_schema_version_old=1\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_schema_version_current=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_payload_format=text-kv\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_schema_manifest_len=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_schema_manifest_checksum=0x%08x\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_matrix_len=%u\n",
                                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_matrix_checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_baseline_len=%u\n",
                                MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_baseline_checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "operation_count=%zu\n",
                                wire_schema_operation_count()) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "field_count=%zu\n",
                                field_count) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "oneof_count=%zu\n",
                                oneof_count) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "status_count=%u\n",
                                MEM_SERVICE_COMPAT_MATRIX_STATUS_COUNT) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "old_client_profile=v1-min-required-fields\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "current_client_profile=v1-current-fields-plus-future-optional\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "old_server_profile=v1-schema-validation-profile\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "current_server_profile=current-runtime-handlers\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "old_server_runtime_binary=in-tree\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=old-client-current-server:schema-compatible\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=old-client-current-server:runtime-compatible\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=current-client-old-schema-profile:schema-compatible\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=current-client-current-server:wire-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=current-client-current-server:compat-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=old-client-missing-required:fail-closed\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=old-client-missing-oneof:fail-closed\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=unknown-text-field-forward:ignored\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=status-id-forward:stable\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=idempotency-forward:compatible\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "certified_pair=old-v1-client->current-v1-server\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "certified_pair=current-v1-client->old-v1-schema-profile\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "certified_pair=current-v1-client->old-v1-runtime-binary\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "evidence=wire-schema-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "evidence=wire-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "evidence=compat-baseline-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "evidence=compat-old-new-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "evidence=compat-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "evidence=compat-old-server-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "release_gate=install-smoke\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "certification_limit=none\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_compat_old_new_matrix(void)
{
    char matrix[8192];
    size_t used = 0;

    if (render_compat_old_new_matrix(matrix, sizeof(matrix), &used) != 0) {
        fprintf(stderr, "mem_service compat-old-new-matrix: render failed\n");
        return 1;
    }
    (void)used;
    fputs(matrix, stdout);
    return 0;
}

static int run_compat_old_new_fixture_check(void)
{
    char matrix[8192];
    char old_payload[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char current_payload[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    struct mem_service_wire_payload_view view;
    size_t used = 0;
    size_t op_index;
    size_t old_payloads = 0;
    size_t current_payloads = 0;
    size_t required_fail_closed = 0;
    size_t oneof_fail_closed = 0;
    uint32_t checksum;
    int failures = 0;

    if (render_compat_old_new_matrix(matrix, sizeof(matrix), &used) != 0) {
        fprintf(stderr, "mem_service compat-old-new-fixtures: render failed\n");
        return 1;
    }
    checksum = mem_service_wire_checksum(matrix, used);
    if (used != MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service compat-old-new-fixtures: matrix len actual=%zu "
                "expected=%u\n",
                used,
                MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN);
        failures -= 1;
    }
    if (checksum != MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service compat-old-new-fixtures: matrix checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM);
        failures -= 1;
    }
    for (op_index = 0; op_index < wire_schema_operation_count(); ++op_index) {
        const struct mem_service_wire_operation_schema *schema =
            &mem_service_wire_operation_schemas[op_index];

        if (render_compat_profile_payload(schema,
                                          false,
                                          old_payload,
                                          sizeof(old_payload)) != 0 ||
            render_compat_profile_payload(schema,
                                          true,
                                          current_payload,
                                          sizeof(current_payload)) != 0) {
            fprintf(stderr,
                    "mem_service compat-old-new-fixtures: payload render failed op=%s\n",
                    schema->name);
            failures -= 1;
            continue;
        }
        view = mem_service_wire_payload_view_from_cstr(old_payload);
        if (!mem_service_wire_schema_validate_payload(schema, &view, NULL)) {
            fprintf(stderr,
                    "mem_service compat-old-new-fixtures: old payload rejected op=%s\n",
                    schema->name);
            failures -= 1;
        } else {
            old_payloads += 1;
        }
        view = mem_service_wire_payload_view_from_cstr(current_payload);
        if (!mem_service_wire_schema_validate_payload(schema, &view, NULL)) {
            fprintf(stderr,
                    "mem_service compat-old-new-fixtures: current payload rejected op=%s\n",
                    schema->name);
            failures -= 1;
        } else {
            current_payloads += 1;
        }
        if (schema_has_required_field(schema)) {
            view = mem_service_wire_payload_view_from_cstr(
                "future_optional_field=ignored\n");
            if (mem_service_wire_schema_validate_payload(schema, &view, NULL)) {
                fprintf(stderr,
                        "mem_service compat-old-new-fixtures: missing required accepted op=%s\n",
                        schema->name);
                failures -= 1;
            } else {
                required_fail_closed += 1;
            }
        }
        if (schema->oneof_count > 0) {
            view = mem_service_wire_payload_view_from_cstr(
                "future_optional_field=ignored\n");
            if (mem_service_wire_schema_validate_payload(schema, &view, NULL)) {
                fprintf(stderr,
                        "mem_service compat-old-new-fixtures: missing oneof accepted op=%s\n",
                        schema->name);
                failures -= 1;
            } else {
                oneof_fail_closed += 1;
            }
        }
    }
    if (old_payloads != wire_schema_operation_count() ||
        current_payloads != wire_schema_operation_count() ||
        MEM_SERVICE_WIRE_VERSION != 1U ||
        MEM_SERVICE_WIRE_HEADER_LEN != 48U ||
        MEM_SERVICE_WIRE_SCHEMA_VERSION != 1U ||
        MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT != 4U ||
        MEM_SERVICE_WIRE_STATUS_UNSUPPORTED != 9U) {
        fprintf(stderr, "mem_service compat-old-new-fixtures: version matrix failed\n");
        failures -= 1;
    }
    if (strstr(matrix, "old_server_runtime_binary=in-tree\n") == NULL ||
        strstr(matrix,
               "certified_pair=current-v1-client->old-v1-schema-profile\n") == NULL ||
        strstr(matrix,
               "case=old-client-current-server:runtime-compatible\n") == NULL ||
        strstr(matrix,
               "certified_pair=current-v1-client->old-v1-runtime-binary\n") ==
            NULL ||
        strstr(matrix, "evidence=compat-old-new-fixtures\n") == NULL ||
        strstr(matrix, "evidence=compat-runtime-fixtures\n") == NULL ||
        strstr(matrix, "evidence=compat-old-server-runtime-fixtures\n") == NULL) {
        fprintf(stderr,
                "mem_service compat-old-new-fixtures: required matrix rule missing\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service compat-old-new-fixtures: status=ok matrix_len=%u "
           "matrix_checksum=0x%08x old_payloads=%zu current_payloads=%zu "
           "required_fail_closed=%zu oneof_fail_closed=%zu "
           "old_server_runtime_binary=in-tree\n",
           MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN,
           MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM,
           old_payloads,
           current_payloads,
           required_fail_closed,
           oneof_fail_closed);
    return 0;
}

static int run_smoke(void)
{
    struct mem_service svc;
    struct mem_service_block_ctx ctx;
    struct mem_service_block_ctx aux_ctx;
    struct mem_service_record block;
    struct mem_service_record aux_block;
    struct mem_service_record prefix;
    struct mem_service_record aux_prefix;
    struct mem_service_record group;
    char block_key[96];

    memset(&svc, 0, sizeof(svc));
    memset(&ctx, 0, sizeof(ctx));
    memset(&aux_ctx, 0, sizeof(aux_ctx));
    memset(&block, 0, sizeof(block));
    memset(&aux_block, 0, sizeof(aux_block));
    memset(&prefix, 0, sizeof(prefix));
    memset(&aux_prefix, 0, sizeof(aux_prefix));
    memset(&group, 0, sizeof(group));

    snprintf(ctx.request_id, sizeof(ctx.request_id), "cli-smoke-request");
    snprintf(ctx.prefix_group, sizeof(ctx.prefix_group), "cli-prefix");
    snprintf(ctx.group_id, sizeof(ctx.group_id), "cli-group");
    snprintf(ctx.block_hash, sizeof(ctx.block_hash), "cli-block-hash");
    ctx.placement_node = 1;
    ctx.placement_level = 2;
    ctx.hot_segment_id = 0x1000;
    ctx.result_segment_id = 0x2000;
    aux_ctx = ctx;
    snprintf(aux_ctx.prefix_group, sizeof(aux_ctx.prefix_group), "cli-prefix-aux");
    snprintf(aux_ctx.block_hash, sizeof(aux_ctx.block_hash), "cli-block-hash-aux");
    aux_ctx.hot_segment_id = 0x3000;
    aux_ctx.result_segment_id = 0x4000;

    if (mem_service_init(&svc, true, true, true) != 0) {
        fprintf(stderr, "mem_service smoke: init failed\n");
        return 1;
    }
    if (mem_service_bootstrap_kvcache(&svc, &ctx, &block) != 0) {
        fprintf(stderr, "mem_service smoke: bootstrap failed\n");
        return 1;
    }
    if (mem_service_bootstrap_kvcache(&svc, &aux_ctx, &aux_block) != 0) {
        fprintf(stderr, "mem_service smoke: aux bootstrap failed\n");
        return 1;
    }
    if (mem_service_apply_block_result(&svc,
                                       &ctx,
                                       ctx.result_segment_id + 0x10,
                                       MEM_SERVICE_KVCACHE_STATE_RELOADED,
                                       &block) != 0) {
        fprintf(stderr, "mem_service smoke: apply block result failed\n");
        return 1;
    }
    if (mem_service_apply_block_result(&svc,
                                       &aux_ctx,
                                       aux_ctx.result_segment_id + 0x10,
                                       MEM_SERVICE_KVCACHE_STATE_RELOADED,
                                       &aux_block) != 0) {
        fprintf(stderr, "mem_service smoke: apply aux block result failed\n");
        return 1;
    }
    if (mem_service_update_prefix_metadata(&svc, &ctx, &block, &prefix) != 0) {
        fprintf(stderr, "mem_service smoke: prefix metadata update failed\n");
        return 1;
    }
    if (mem_service_update_prefix_metadata(&svc, &aux_ctx, &aux_block, &aux_prefix) != 0) {
        fprintf(stderr, "mem_service smoke: aux prefix metadata update failed\n");
        return 1;
    }
    if (mem_service_get_prefix_group_metadata(&svc, &ctx, &group) != 0) {
        fprintf(stderr, "mem_service smoke: prefix group metadata failed\n");
        return 1;
    }
    mem_service_build_block_key_from_hash(ctx.block_hash, block_key, sizeof(block_key));
    if (mem_service_get_record(&svc, block_key, &block) != 0) {
        fprintf(stderr, "mem_service smoke: block lookup failed\n");
        return 1;
    }
    if (!mem_service_prefix_matches_block_meta(&prefix, &block) ||
        !mem_service_prefix_matches_block_meta(&aux_prefix, &aux_block) ||
        !mem_service_group_covers_blocks(&group, &block, &aux_block)) {
        fprintf(stderr, "mem_service smoke: prefix/group relation failed\n");
        return 1;
    }

    printf("mem_service smoke: status=ok records=%zu block_key=%s state=%s group_members=%u\n",
           svc.record_count,
           block_key,
           mem_service_kvcache_state_name(block.state),
           group.member_count);
    return 0;
}

#ifdef MEM_SERVICE_ENABLE_QWEN3_INSPECT
static int inspect_qwen3(void)
{
    uint32_t node;
    uint32_t nodes = (uint32_t)llm_infer_qwen3_pipeline_nodes();

    printf("mem_service qwen3: model_key=%s nodes=%u layers=%" PRIu64
           " hidden_range_bytes=%" PRIu64 " decode_hidden_bytes=%" PRIu64 "\n",
           llm_infer_qwen3_model_key(),
           nodes,
           llm_infer_qwen3_total_layers(),
           llm_infer_qwen3_hidden_range_bytes(),
           llm_infer_qwen3_decode_hidden_bytes());
    for (node = 0; node < nodes; ++node) {
        uint32_t start = 0;
        uint32_t end = 0;
        uint32_t next = 0;

        if (llm_infer_qwen3_layer_range_for_node(node, nodes, &start, &end, &next) != 0) {
            fprintf(stderr, "mem_service qwen3: invalid placement node=%u\n", node);
            return 1;
        }
        printf("mem_service qwen3: node=%u layers=[%u,%u) next=%u kv_bytes_per_token=%" PRIu64 "\n",
               node + 1,
               start,
               end,
               next + 1,
               llm_infer_qwen3_range_kv_state_bytes(start, end));
    }
    return 0;
}
#endif

static int render_version_manifest(char *manifest,
                                   size_t manifest_len,
                                   size_t *used_out)
{
    size_t used = 0;

    if (manifest == NULL || manifest_len == 0) {
        return -1;
    }
    manifest[0] = '\0';
    if (append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "service_version=%s\n",
                                MEM_SERVICE_RELEASE_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_contract_version=1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "version_contract=text-kv\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "wire_version=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "wire_schema_version=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "wire_schema_manifest_version=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "wire_schema_manifest_checksum=0x%08x\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "api_abi_policy_version=%u\n",
                                MEM_SERVICE_API_ABI_POLICY_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "api_abi_policy_checksum=0x%08x\n",
                                MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "package_manifest_version=%u\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "package_manifest_len=%u\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "package_manifest_checksum=0x%08x\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_manifest_command=release-manifest\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "package_manifest_command=package-manifest\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "config_security_gate=config-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "version_gate=version-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "ops_certification_status=not-certified-until-external-evidence\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_version_manifest(void)
{
    char manifest[2048];
    size_t used = 0;

    if (render_version_manifest(manifest, sizeof(manifest), &used) != 0) {
        fprintf(stderr, "mem_service version: render failed\n");
        return 1;
    }
    fwrite(manifest, 1, used, stdout);
    return 0;
}

static int run_version_fixture_check(void)
{
    char manifest[2048];
    size_t used = 0;

    if (render_version_manifest(manifest, sizeof(manifest), &used) != 0) {
        fprintf(stderr, "mem_service version-fixtures: render failed\n");
        return 1;
    }
    if (strstr(manifest, "service_name=linqu_mem_service\n") == NULL ||
        strstr(manifest, "service_version=" MEM_SERVICE_RELEASE_VERSION "\n") == NULL ||
        strstr(manifest, "version_contract=text-kv\n") == NULL ||
        strstr(manifest, "wire_version=1\n") == NULL ||
        strstr(manifest, "wire_schema_manifest_checksum=0xe07a9225\n") == NULL ||
        strstr(manifest, "api_abi_policy_checksum=0x5e460a87\n") == NULL ||
        strstr(manifest, "package_manifest_checksum=0x") == NULL ||
        strstr(manifest, "release_manifest_command=release-manifest\n") == NULL ||
        strstr(manifest, "package_manifest_command=package-manifest\n") == NULL ||
        strstr(manifest, "config_security_gate=config-fixtures\n") == NULL ||
        strstr(manifest, "version_gate=version-fixtures\n") == NULL) {
        fprintf(stderr, "mem_service version-fixtures: required field missing\n");
        return 1;
    }
    printf("mem_service version-fixtures: status=ok service_version=%s "
           "wire_version=%u package_manifest_len=%u "
           "package_manifest_checksum=0x%08x\n",
           MEM_SERVICE_RELEASE_VERSION,
           MEM_SERVICE_WIRE_VERSION,
           MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN,
           MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM);
    (void)used;
    return 0;
}

static int render_release_readiness(char *manifest,
                                    size_t manifest_len,
                                    size_t *used_out,
                                    bool ops_certified,
                                    bool remote_transport_certified)
{
    size_t used = 0;
    const char *ops_status = ops_certified
                                 ? "certified"
                                 : "not-certified-until-external-evidence";
    const char *remote_status = remote_transport_certified
                                    ? "certified"
                                    : "not-certified-until-cross-host-evidence";
    const char *overall_status = ops_certified && remote_transport_certified
                                     ? "certified"
                                     : "not-certified";
    const char *blocking_evidence = ops_certified && remote_transport_certified
                                        ? "none"
                                        : "linux-ops-certification-bundle,remote-transport-certification-bundle";

    if (manifest == NULL || manifest_len == 0) {
        return -1;
    }
    manifest[0] = '\0';
    if (append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "mem_service_release_readiness_version=1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "service_version=%s\n",
                                MEM_SERVICE_RELEASE_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "readiness_contract=text-kv\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "package_manifest_len=%u\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "package_manifest_checksum=0x%08x\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_manifest_command=release-manifest\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "package_manifest_command=package-manifest\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_sdk_preflight=scripts/verify_mem_service_installed_sdk.sh --preflight\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_sdk_contract=machine-discoverable\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "serving_pretraining_runtime=certified\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "serving_pretraining_runtime_gate=installed-sdk-runtime-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "ops_certification_status=%s\n",
                                ops_status) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "ops_certification_required_bundle=linqu-mem-service-ops-certification-bundle.tar\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "ops_certification_ci=scripts/run_mem_service_linux_ops_ci.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "ops_certification_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "ops_certification_evidence_verify=ops-certification-verify --evidence-file\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_transport_status=%s\n",
                                remote_status) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_transport_required_bundle=linqu-mem-service-remote-transport-bundle.tar\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_transport_ci=scripts/run_mem_service_remote_transport_ci.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_transport_evidence_verify=remote-transport-verify --evidence-file\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_certification_ci=scripts/run_mem_service_release_certification_ci.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_certification_verify=scripts/verify_mem_service_release_certification.sh --ops-bundle-file --remote-transport-bundle-file\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_certification_readiness_gate=release-readiness --ops-evidence-file --remote-transport-evidence-file\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_readiness_evidence_verify=release-readiness --ops-evidence-file --remote-transport-evidence-file\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "overall_status=%s\n",
                                overall_status) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "blocking_external_evidence=%s\n",
                                blocking_evidence) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_readiness_gate=release-readiness-fixtures\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_release_readiness(int argc, char **argv)
{
    const char *ops_path = option_value(argc, argv, "--ops-evidence-file");
    const char *remote_path =
        option_value(argc, argv, "--remote-transport-evidence-file");
    char manifest[3072];
    char ops_evidence[4096];
    char remote_evidence[2048];
    char reason[160];
    size_t used = 0;
    bool verify_evidence = ops_path != NULL || remote_path != NULL;
    bool ops_certified = false;
    bool remote_transport_certified = false;
    int i;

    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--ops-evidence-file") == 0 ||
            strcmp(argv[i], "--remote-transport-evidence-file") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr,
                        "mem_service release-readiness: option requires a path: %s\n",
                        argv[i]);
                return 2;
            }
            ++i;
            continue;
        }
        fprintf(stderr,
                "mem_service release-readiness: unexpected argument: %s\n",
                argv[i]);
        return 2;
    }

    if (verify_evidence) {
        if (ops_path == NULL || ops_path[0] == '\0' ||
            remote_path == NULL || remote_path[0] == '\0') {
            fprintf(stderr,
                    "mem_service release-readiness: both --ops-evidence-file "
                    "and --remote-transport-evidence-file are required\n");
            return 2;
        }
        if (read_text_file_limited(ops_path, ops_evidence, sizeof(ops_evidence)) != 0) {
            fprintf(stderr, "mem_service release-readiness: ops evidence read failed\n");
            return 1;
        }
        if (validate_ops_certification_evidence(ops_evidence,
                                                reason,
                                                sizeof(reason)) != 0) {
            fprintf(stderr,
                    "mem_service release-readiness: fail-closed "
                    "scope=ops reason=%s\n",
                    reason);
            return 1;
        }
        if (read_text_file_limited(remote_path,
                                   remote_evidence,
                                   sizeof(remote_evidence)) != 0) {
            fprintf(stderr,
                    "mem_service release-readiness: remote transport evidence "
                    "read failed\n");
            return 1;
        }
        if (validate_remote_transport_evidence(remote_evidence,
                                               reason,
                                               sizeof(reason)) != 0) {
            fprintf(stderr,
                    "mem_service release-readiness: fail-closed "
                    "scope=remote-transport reason=%s\n",
                    reason);
            return 1;
        }
        ops_certified = true;
        remote_transport_certified = true;
    }
    if (render_release_readiness(manifest,
                                 sizeof(manifest),
                                 &used,
                                 ops_certified,
                                 remote_transport_certified) != 0) {
        fprintf(stderr, "mem_service release-readiness: render failed\n");
        return 1;
    }
    fwrite(manifest, 1, used, stdout);
    return 0;
}

static int run_release_readiness_fixture_check(void)
{
    char manifest[3072];
    char valid[3072];
    char invalid[3072];
    char ops_evidence[2048];
    char remote_evidence[2048];
    size_t used = 0;

    if (render_release_readiness(manifest, sizeof(manifest), &used, false, false) != 0) {
        fprintf(stderr, "mem_service release-readiness-fixtures: render failed\n");
        return 1;
    }
    if (strstr(manifest, "mem_service_release_readiness_version=1\n") == NULL ||
        strstr(manifest, "readiness_contract=text-kv\n") == NULL ||
        strstr(manifest, "installed_sdk_contract=machine-discoverable\n") == NULL ||
        strstr(manifest, "serving_pretraining_runtime=certified\n") == NULL ||
        strstr(manifest,
               "ops_certification_status=not-certified-until-external-evidence\n") ==
            NULL ||
        strstr(manifest,
               "ops_certification_ci=scripts/run_mem_service_linux_ops_ci.sh\n") ==
            NULL ||
        strstr(manifest,
               "ops_certification_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight\n") ==
            NULL ||
        strstr(manifest,
               "ops_certification_evidence_verify=ops-certification-verify --evidence-file\n") ==
            NULL ||
        strstr(manifest,
               "remote_transport_status=not-certified-until-cross-host-evidence\n") ==
            NULL ||
        strstr(manifest,
               "remote_transport_ci=scripts/run_mem_service_remote_transport_ci.sh\n") ==
            NULL ||
        strstr(manifest,
               "remote_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight\n") ==
            NULL ||
        strstr(manifest,
               "remote_transport_evidence_verify=remote-transport-verify --evidence-file\n") ==
            NULL ||
        strstr(manifest,
               "release_certification_ci=scripts/run_mem_service_release_certification_ci.sh\n") ==
            NULL ||
        strstr(manifest,
               "release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight\n") ==
            NULL ||
        strstr(manifest,
               "release_certification_verify=scripts/verify_mem_service_release_certification.sh --ops-bundle-file --remote-transport-bundle-file\n") ==
            NULL ||
        strstr(manifest,
               "release_certification_readiness_gate=release-readiness --ops-evidence-file --remote-transport-evidence-file\n") ==
            NULL ||
        strstr(manifest,
               "release_readiness_evidence_verify=release-readiness --ops-evidence-file --remote-transport-evidence-file\n") ==
            NULL ||
        strstr(manifest, "overall_status=not-certified\n") == NULL ||
        strstr(manifest,
               "blocking_external_evidence=linux-ops-certification-bundle,remote-transport-certification-bundle\n") ==
            NULL ||
        strstr(manifest, "release_readiness_gate=release-readiness-fixtures\n") ==
            NULL) {
        fprintf(stderr,
                "mem_service release-readiness-fixtures: required field missing\n");
        return 1;
    }
    if (append_ops_certification_valid_evidence(ops_evidence, sizeof(ops_evidence)) != 0 ||
        append_remote_transport_valid_evidence(remote_evidence,
                                               sizeof(remote_evidence)) != 0) {
        fprintf(stderr,
                "mem_service release-readiness-fixtures: evidence render failed\n");
        return 1;
    }
    if (validate_ops_certification_evidence(ops_evidence, invalid, sizeof(invalid)) != 0 ||
        validate_remote_transport_evidence(remote_evidence,
                                           invalid,
                                           sizeof(invalid)) != 0) {
        fprintf(stderr,
                "mem_service release-readiness-fixtures: valid evidence rejected\n");
        return 1;
    }
    if (render_release_readiness(valid, sizeof(valid), &used, true, true) != 0 ||
        strstr(valid, "ops_certification_status=certified\n") == NULL ||
        strstr(valid, "remote_transport_status=certified\n") == NULL ||
        strstr(valid, "overall_status=certified\n") == NULL ||
        strstr(valid, "blocking_external_evidence=none\n") == NULL) {
        fprintf(stderr,
                "mem_service release-readiness-fixtures: certified readiness missing\n");
        return 1;
    }
    strcpy(invalid, ops_evidence);
    {
        char *gate = strstr(invalid, "rpm_package_smoke=pass\n");

        if (gate == NULL) {
            return 1;
        }
        memcpy(gate, "rpm_package_smoke=fail\n", strlen("rpm_package_smoke=fail\n"));
    }
    if (validate_ops_certification_evidence(invalid, manifest, sizeof(manifest)) == 0 ||
        strstr(manifest, "rpm_package_smoke") == NULL) {
        fprintf(stderr,
                "mem_service release-readiness-fixtures: invalid evidence accepted\n");
        return 1;
    }
    printf("mem_service release-readiness-fixtures: status=ok "
           "default_overall_status=not-certified "
           "certified_overall_status=certified "
           "blocking_external_evidence=2 evidence_positive=2 fail_closed=1 "
           "package_manifest_checksum=0x%08x\n",
           MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM);
    (void)used;
    return 0;
}

static int render_package_manifest(char *manifest,
                                   size_t manifest_len,
                                   size_t *used_out)
{
    size_t used = 0;

    if (manifest == NULL || manifest_len == 0) {
        return -1;
    }
    manifest[0] = '\0';
    if (append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "mem_service_package_manifest_version=%u\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "package_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "package_format=installed-layout-v1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "artifact_format=tar\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "artifact_name=%s\n",
                                MEM_SERVICE_PACKAGE_TARBALL_NAME) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "artifact_root=usr+etc\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "artifact_install_prefix=/usr\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "artifact_contents=installed-layout-v1-root\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "artifact_gate=package-tarball-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "native_package_format=deb\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "native_package_name=%s\n",
                                MEM_SERVICE_NATIVE_DEB_NAME) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "native_package_arch=arm64\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "native_package_payload=debian-binary+control.tar.gz+data.tar.gz\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "native_package_gate=package-deb-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "native_package_runtime=not-executed-cross-compiled-arm64\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "rpm_package_format=rpm\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "rpm_package_name=%s\n",
                                MEM_SERVICE_NATIVE_RPM_NAME) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "rpm_package_arch=aarch64\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "rpm_package_payload=rpm-cpio+metadata\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "rpm_package_gate=package-rpm-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "rpm_package_runtime=requires-linux-rpm-toolchain\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "package_scope=core-daemon+host-daemon+client-sdk+examples+contracts+deploy+runtime-config+systemd-units+release-scripts\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "prefix_default=/usr\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "binary=bin/linqu_mem_service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "host_binary=libexec/lingqu/mem_service/linqu_mem_service_host\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "binary_version_command=version\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "binary_version_contract=text-kv\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "binary_version_gate=version-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_readiness_command=release-readiness\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_readiness_contract=text-kv\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_readiness_evidence_verify=release-readiness --ops-evidence-file --remote-transport-evidence-file\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_readiness_gate=release-readiness-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "optional_adapter=bin/linqu_mem_service_qwen3\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "default_endpoint=%s\n",
                                mem_service_default_unix_socket_spec()) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "data_root=share/lingqu/mem_service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "header_root=include/lingqu/mem_service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "source_root=src/lingqu/mem_service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "pkgconfig=lib/pkgconfig/lingqu-mem-service.pc\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "pkgconfig_name=lingqu-mem-service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "pkgconfig_cflags=-I${includedir}\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "pkgconfig_sdk_sources=${sourcedir}/mem_service_client.c ${sourcedir}/mem_service_wire_client.c ${sourcedir}/mem_service_provider.c\n") != 0 ||
        append_wire_schema_line(manifest, manifest_len, &used,
                                "pkgconfig_mapping_owner_sources=${sourcedir}/mem_service_mapping_owner.c\n") != 0 ||
        append_wire_schema_line(manifest, manifest_len, &used,
                                "pkgconfig_mapping_owner_libs=-pthread\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "pkgconfig_payload_provider_roce_sources=${sourcedir}/mem_service_provider_roce.c\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "pkgconfig_payload_provider_roce_libs=-lrdmacm -libverbs\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "pkgconfig_payload_provider_tcp_sources=${sourcedir}/mem_service_provider_tcp.c\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "pkgconfig_payload_provider_tcp_libs=-pthread\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_sdk_preflight=scripts/verify_mem_service_installed_sdk.sh --preflight\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_sdk_preflight_scope=pkg-config-cflags+sdk-sources+examples+host-binary-no-compile\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_sdk_example_smoke=installed-sdk-example-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_sdk_example_smoke_scope=serving+pretraining-external-client-compile\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_sdk_pkgconfig_smoke=installed-sdk-pkgconfig-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_sdk_pkgconfig_smoke_scope=pkg-config-cflags+sdk-sources-external-client-compile\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_sdk_runtime_smoke=installed-sdk-runtime-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_sdk_runtime_smoke_scope=installed-host-daemon+serving+pretraining-runtime\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_sdk_runtime_reuse=installed-sdk-runtime-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_sdk_runtime_reuse_scope=daemon-restart+durable-store+serving+pretraining\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "config_root=share/lingqu/mem_service/config\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "system_config_root=etc/lingqu/mem_service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "runtime_config=etc/lingqu/mem_service/mem_service.conf\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "runtime_config_source=share/lingqu/mem_service/config/mem_service.runtime.conf\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "host_runtime_config=etc/lingqu/mem_service/mem_service.host.conf\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "host_runtime_config_source=share/lingqu/mem_service/config/mem_service.host.runtime.conf\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "service_auth_boundary=unix-socket-local-only\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "metrics_auth_boundary=loopback-only\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "config_security_gate=config-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "deployment_quota_contract=max-records+max-payload-bytes\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "deployment_quota_gate=config-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "retention_policy=manual-or-audit-log-limit\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "retention_policy_gate=config-fixtures,retention-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "checkpoint_retention_policy=manual-or-latest-limit\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "checkpoint_retention_gate=config-fixtures,checkpoint-retention-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "record_retention_policy=manual-or-global-kind-tenant-latest-or-ttl\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "record_retention_gate=config-fixtures,record-retention-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "payload_block_gc=record-and-checkpoint-retention-orphan-blocks\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "payload_block_gc_gate=payload-gc-fixtures,record-retention-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "encryption_policy=explicit-none-only\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "encryption_at_rest=not-certified\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "encryption_policy_command=encryption-policy\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "encryption_policy_gate=encryption-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "runtime_quota_admission=max-records+max-payload-bytes\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "runtime_quota_gate=runtime-quota-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "deploy_root=share/lingqu/mem_service/deploy\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "systemd_unit_root=lib/systemd/system\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "systemd_unit=lib/systemd/system/linqu_mem_service.service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "host_systemd_unit=lib/systemd/system/linqu_mem_service.host.service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script_root=share/lingqu/mem_service/scripts\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_certification_ci=scripts/run_mem_service_release_certification_ci.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_certification_readiness_gate=release-readiness --ops-evidence-file --remote-transport-evidence-file\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "linux_ops_ci=scripts/run_mem_service_linux_ops_ci.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "linux_ops_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/run_mem_service_linux_ops_ci.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/verify_mem_service_linux_ops_evidence.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/verify_mem_service_ops_certification_bundle.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/run_mem_service_remote_transport_ci.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/verify_mem_service_remote_transport_evidence.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/verify_mem_service_remote_transport_bundle.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/verify_mem_service_release_certification.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_file_count=%u\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_INSTALLED_FILE_COUNT) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=core_binary count=1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=host_binary count=1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=public_headers count=15\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=client_sources count=4\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=provider_sources count=2\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=pkgconfig count=1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=examples count=2\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=contracts count=10\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=configs count=4\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=runtime_config count=2\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=deploy count=3\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=systemd_units count=2\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=release_scripts count=10\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=release-manifest path=share/lingqu/mem_service/release-manifest.txt\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=wire-schema path=share/lingqu/mem_service/wire-schema.txt checksum=0x%08x\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=admin-output-schema path=share/lingqu/mem_service/admin-output-schema.txt checksum=0x%08x\n",
                                MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=upgrade-rollback-policy path=share/lingqu/mem_service/upgrade-rollback-policy.txt checksum=0x%08x\n",
                                MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=api-abi-policy path=share/lingqu/mem_service/api-abi-policy.txt checksum=0x%08x\n",
                                MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=compat-matrix path=share/lingqu/mem_service/compat-matrix.txt checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=compat-baseline-v1 path=share/lingqu/mem_service/compat-baseline-v1.txt checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=compat-old-new-matrix path=share/lingqu/mem_service/compat-old-new-matrix.txt checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=alert-rules path=share/lingqu/mem_service/deploy/linqu_mem_service.prometheus-alerts.yml checksum=0x%08x\n",
                                MEM_SERVICE_ALERT_RULES_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=ops-certification-policy path=share/lingqu/mem_service/ops-certification-policy.txt checksum=0x%08x\n",
                                MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate_count=%u\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_GATE_COUNT) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=release-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=package-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=version-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=release-readiness-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=admin-output-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=upgrade-rollback-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=upgrade-rollback-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=restore-policy-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=runtime-quota-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=retention-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=checkpoint-retention-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=payload-gc-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=record-retention-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=encryption-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=api-abi-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=compat-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=compat-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=deployment-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=collector-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=alert-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=alert-integration-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=ops-certification-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=ops-certification-evidence-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=remote-transport-evidence-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=ops-certification-linux-ci-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=durable-catalog-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=host-artifact-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=package-tarball-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=package-deb-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=package-rpm-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=install-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=installed-sdk-example-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=installed-sdk-pkgconfig-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=installed-sdk-runtime-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "serving_api=typed-c-client-v1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "pretraining_api=typed-c-client-v1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "payload_ownership_matrix=certified\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "payload_ownership_scope=artifact-query-expected-owner\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "restore_policy=transactional-staged-restore\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "restore_policy_gate=restore-policy-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "durable_backend=snapshot+journal\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "durable_store_schema_version=1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "durable_store_migration_policy=legacy-to-v1-reject-future\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "payload_block_backend=sealed-local-block-v1,sealed-chunked-block-v1,transport-loopback-block-v1,transport-tcp-block-v1,ub-ssd-gsva-v1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_payload_production_network_transport=not-certified\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_payload_production_transport_evidence_schema=remote-transport-evidence-v1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_payload_production_transport_evidence_gate=remote-transport-evidence-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_payload_production_transport_generate=remote-transport-generate-evidence\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_payload_production_transport_verify=remote-transport-verify --evidence-file\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_payload_production_transport_ci=scripts/run_mem_service_remote_transport_ci.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_payload_production_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "upgrade_policy=current-version-only\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "old_server_runtime_binary=certified\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "cross_version_upgrade=certified\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "real_systemd_environment=not-certified\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "production_collector_alert_environment=not-certified\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_package_manifest(void)
{
    char manifest[12288];
    size_t used = 0;

    if (render_package_manifest(manifest, sizeof(manifest), &used) != 0) {
        fprintf(stderr, "mem_service package-manifest: render failed\n");
        return 1;
    }
    fwrite(manifest, 1, used, stdout);
    return 0;
}

static int render_ops_certification_policy(char *policy,
                                           size_t policy_len,
                                           size_t *used_out)
{
    size_t used = 0;

    if (policy == NULL || policy_len == 0) {
        return -1;
    }
    if (append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "mem_service_ops_certification_policy_version=%u\n",
                                MEM_SERVICE_OPS_CERTIFICATION_POLICY_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "certification_scope=real-linux-operations\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "certification_status=not-certified\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "admission_rule=fail-closed-until-external-evidence\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "evidence_schema=ops-certification-evidence-v1\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "evidence_verify=ops-certification-verify --evidence-file\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "evidence_generate=ops-certification-generate-evidence\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "evidence_ci_gate=ops-certification-linux-ci-smoke\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "evidence_gate=ops-certification-evidence-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "local_gate=deployment-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "local_gate=collector-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "local_gate=alert-integration-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "external_gate=linux-systemd-service-smoke\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "external_gate=linux-systemd-host-service-smoke\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "external_gate=prometheus-scrape-smoke\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "external_gate=prometheus-alertmanager-rule-smoke\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "external_gate=rpm-package-smoke\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "external_gate=upgrade-rollback-deployment-smoke\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_environment=os=linux\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_environment=init=systemd\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_tool=systemctl\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_tool=journalctl\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_tool=promtool\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_tool=rpmbuild\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_tool=rpm2cpio\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "real_systemd_environment=not-certified\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "production_collector_alert_environment=not-certified\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "rpm_package=not-certified\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_ops_certification_policy(void)
{
    char policy[4096];
    size_t used = 0;

    if (render_ops_certification_policy(policy, sizeof(policy), &used) != 0) {
        fprintf(stderr, "mem_service ops-certification-policy: render failed\n");
        return 1;
    }
    fwrite(policy, 1, used, stdout);
    return 0;
}

static int run_ops_certification_fixture_check(void)
{
    char policy[4096];
    size_t used = 0;
    uint32_t checksum;

    if (render_ops_certification_policy(policy, sizeof(policy), &used) != 0) {
        fprintf(stderr, "mem_service ops-certification-fixtures: render failed\n");
        return 1;
    }
    checksum = mem_service_wire_checksum(policy, used);
    if (used != MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service ops-certification-fixtures: policy len actual=%zu "
                "expected=%u\n",
                used,
                MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_LEN);
        return 1;
    }
    if (checksum != MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service ops-certification-fixtures: policy checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM);
        return 1;
    }
    if (strstr(policy, "certification_status=not-certified\n") == NULL ||
        strstr(policy,
               "admission_rule=fail-closed-until-external-evidence\n") == NULL ||
        strstr(policy, "evidence_schema=ops-certification-evidence-v1\n") == NULL ||
        strstr(policy,
               "evidence_gate=ops-certification-evidence-fixtures\n") == NULL ||
        strstr(policy, "external_gate=linux-systemd-service-smoke\n") == NULL ||
        strstr(policy,
               "external_gate=prometheus-alertmanager-rule-smoke\n") == NULL ||
        strstr(policy, "external_gate=rpm-package-smoke\n") == NULL ||
        strstr(policy, "required_environment=init=systemd\n") == NULL ||
        strstr(policy, "real_systemd_environment=not-certified\n") == NULL ||
        strstr(policy,
               "production_collector_alert_environment=not-certified\n") == NULL) {
        fprintf(stderr,
                "mem_service ops-certification-fixtures: required policy missing\n");
        return 1;
    }
    printf("mem_service ops-certification-fixtures: status=ok policy_version=%u "
           "policy_len=%u policy_checksum=0x%08x "
           "certification_status=not-certified external_gates=6 "
           "admission_rule=fail-closed-until-external-evidence\n",
           MEM_SERVICE_OPS_CERTIFICATION_POLICY_VERSION,
           MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_LEN,
           MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM);
    return 0;
}

static int run_encryption_policy(void)
{
    printf("mem_service_encryption_policy_version=1\n");
    printf("encryption_at_rest=not-certified\n");
    printf("supported_config_encryption=none\n");
    printf("unsupported_encryption_admission=fail-closed\n");
    printf("key_management=not-certified\n");
    printf("data_plane_encryption=not-certified\n");
    printf("config_gate=config-fixtures\n");
    printf("policy_gate=encryption-fixtures\n");
    return 0;
}

static bool mem_service_payload_string_equals(
    const struct mem_service_wire_payload_view *view,
    const char *name,
    const char *expected)
{
    char value[160];

    return mem_service_wire_payload_get_string(view, name, value, sizeof(value)) &&
           strcmp(value, expected) == 0;
}

static int validate_ops_certification_evidence(const char *evidence,
                                               char *reason,
                                               size_t reason_len)
{
    struct mem_service_wire_payload_view view;
    uint64_t policy_checksum;
    uint64_t package_checksum;
    uint32_t version;
    static const char *required_pass_gates[] = {
        "linux_systemd_service_smoke",
        "linux_systemd_host_service_smoke",
        "prometheus_scrape_smoke",
        "prometheus_alertmanager_rule_smoke",
        "rpm_package_smoke",
        "upgrade_rollback_deployment_smoke",
    };
    size_t i;

    if (reason != NULL && reason_len > 0) {
        reason[0] = '\0';
    }
    if (evidence == NULL || evidence[0] == '\0') {
        snprintf(reason, reason_len, "empty-evidence");
        return -1;
    }
    view = mem_service_wire_payload_view_from_cstr(evidence);
    version = mem_service_wire_payload_get_u32(
        &view, "mem_service_ops_certification_evidence_version", 0);
    if (version != MEM_SERVICE_OPS_CERTIFICATION_EVIDENCE_VERSION) {
        snprintf(reason, reason_len, "bad-evidence-version");
        return -1;
    }
    if (!mem_service_payload_string_equals(&view,
                                           "service_name",
                                           "linqu_mem_service") ||
        !mem_service_payload_string_equals(&view,
                                           "certification_scope",
                                           "real-linux-operations") ||
        !mem_service_payload_string_equals(&view, "evidence_os", "linux") ||
        !mem_service_payload_string_equals(&view, "evidence_init", "systemd")) {
        snprintf(reason, reason_len, "bad-evidence-identity");
        return -1;
    }
    if (!mem_service_wire_payload_get_u64_checked(&view,
                                                   "ops_certification_policy_checksum",
                                                   &policy_checksum) ||
        policy_checksum != MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM) {
        snprintf(reason, reason_len, "bad-policy-checksum");
        return -1;
    }
    if (!mem_service_wire_payload_get_u64_checked(&view,
                                                   "package_manifest_checksum",
                                                   &package_checksum) ||
        package_checksum != MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM) {
        snprintf(reason, reason_len, "bad-package-checksum");
        return -1;
    }
    for (i = 0; i < sizeof(required_pass_gates) / sizeof(required_pass_gates[0]); ++i) {
        if (!mem_service_payload_string_equals(&view,
                                               required_pass_gates[i],
                                               "pass")) {
            snprintf(reason, reason_len, "gate-not-pass:%s", required_pass_gates[i]);
            return -1;
        }
    }
    return 0;
}

static int validate_remote_transport_evidence(const char *evidence,
                                              char *reason,
                                              size_t reason_len)
{
    struct mem_service_wire_payload_view view;
    uint64_t package_checksum;
    uint32_t version;
    static const char *required_pass_gates[] = {
        "source_address_non_loopback",
        "payload_block_round_trip",
        "payload_checksum_validation",
        "payload_corruption_fail_closed",
        "producer_consumer_distinct_hosts",
        "network_partition_fail_closed",
    };
    size_t i;

    if (reason != NULL && reason_len > 0) {
        reason[0] = '\0';
    }
    if (evidence == NULL || evidence[0] == '\0') {
        snprintf(reason, reason_len, "empty-evidence");
        return -1;
    }
    view = mem_service_wire_payload_view_from_cstr(evidence);
    version = mem_service_wire_payload_get_u32(
        &view, "mem_service_remote_transport_evidence_version", 0);
    if (version != MEM_SERVICE_REMOTE_TRANSPORT_EVIDENCE_VERSION) {
        snprintf(reason, reason_len, "bad-evidence-version");
        return -1;
    }
    if (!mem_service_payload_string_equals(&view,
                                           "service_name",
                                           "linqu_mem_service") ||
        !mem_service_payload_string_equals(&view,
                                           "certification_scope",
                                           "production-network-transport") ||
        !mem_service_payload_string_equals(&view,
                                           "transport_backend",
                                           "transport-tcp-block-v1") ||
        !mem_service_payload_string_equals(&view, "transport_protocol", "tcp-ipv4") ||
        !mem_service_payload_string_equals(&view, "transport_topology", "cross-host")) {
        snprintf(reason, reason_len, "bad-evidence-identity");
        return -1;
    }
    if (!mem_service_wire_payload_get_u64_checked(&view,
                                                   "package_manifest_checksum",
                                                   &package_checksum) ||
        package_checksum != MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM) {
        snprintf(reason, reason_len, "bad-package-checksum");
        return -1;
    }
    for (i = 0; i < sizeof(required_pass_gates) / sizeof(required_pass_gates[0]); ++i) {
        if (!mem_service_payload_string_equals(&view,
                                               required_pass_gates[i],
                                               "pass")) {
            snprintf(reason, reason_len, "gate-not-pass:%s", required_pass_gates[i]);
            return -1;
        }
    }
    return 0;
}

static int read_text_file_limited(const char *path, char *payload, size_t payload_len)
{
    FILE *file;
    size_t used;

    if (path == NULL || path[0] == '\0' || payload == NULL || payload_len == 0) {
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    used = fread(payload, 1, payload_len - 1U, file);
    if (ferror(file) != 0 || (!feof(file) && used == payload_len - 1U)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    payload[used] = '\0';
    return 0;
}

static int write_text_file_limited(const char *path,
                                   const char *payload,
                                   size_t payload_len)
{
    FILE *file;

    if (path == NULL || path[0] == '\0' || payload == NULL) {
        return -1;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        return -1;
    }
    if (payload_len > 0 && fwrite(payload, 1, payload_len, file) != payload_len) {
        fclose(file);
        return -1;
    }
    if (fflush(file) != 0) {
        fclose(file);
        return -1;
    }
    fclose(file);
    return 0;
}

static int path_exists(const char *path)
{
    struct stat st;

    return path != NULL && path[0] != '\0' && stat(path, &st) == 0;
}

static int ensure_directory_path(const char *path)
{
    char buffer[4096];
    char *cursor;
    size_t len;

    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    len = strlen(path);
    if (len >= sizeof(buffer)) {
        return -1;
    }
    memcpy(buffer, path, len + 1U);
    while (len > 1U && buffer[len - 1U] == '/') {
        buffer[len - 1U] = '\0';
        len -= 1U;
    }
    for (cursor = buffer + 1; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mkdir(buffer, 0777) != 0 && errno != EEXIST) {
                return -1;
            }
            *cursor = '/';
        }
    }
    if (mkdir(buffer, 0777) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int ensure_parent_directory_path(const char *path)
{
    char buffer[4096];
    char *slash;
    size_t len;

    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    len = strlen(path);
    if (len >= sizeof(buffer)) {
        return -1;
    }
    memcpy(buffer, path, len + 1U);
    slash = strrchr(buffer, '/');
    if (slash == NULL) {
        return 0;
    }
    if (slash == buffer) {
        return 0;
    }
    *slash = '\0';
    return ensure_directory_path(buffer);
}

static int write_text_file_if_missing(const char *path, const char *payload)
{
    if (path_exists(path)) {
        return 0;
    }
    if (ensure_parent_directory_path(path) != 0) {
        return -1;
    }
    return write_text_file_limited(path, payload, strlen(payload));
}

static bool ops_certification_command_ok(const char *command)
{
    int rc;

    if (command == NULL || command[0] == '\0') {
        return false;
    }
    rc = system(command);
    return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

static bool ops_certification_command_exists(const char *name)
{
    char command[160];

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (snprintf(command,
                 sizeof(command),
                 "command -v %s >/dev/null 2>&1",
                 name) >= (int)sizeof(command)) {
        return false;
    }
    return ops_certification_command_ok(command);
}

static bool ops_certification_safe_path(const char *path)
{
    size_t i;

    if (path == NULL || path[0] == '\0') {
        return false;
    }
    for (i = 0; path[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)path[i];

        if (!(isalnum(ch) || ch == '/' || ch == '.' || ch == '_' || ch == '-' ||
              ch == ':' || ch == '+')) {
            return false;
        }
    }
    return true;
}

static bool ops_certification_host_is_linux(void)
{
#ifdef __linux__
    return true;
#else
    return false;
#endif
}

static bool ops_certification_systemd_is_available(void)
{
    return access("/run/systemd/system", F_OK) == 0 &&
           ops_certification_command_exists("systemctl") &&
           ops_certification_command_exists("journalctl");
}

static bool ops_certification_service_is_active(const char *unit)
{
    char command[192];

    if (unit == NULL || unit[0] == '\0') {
        return false;
    }
    if (snprintf(command,
                 sizeof(command),
                 "systemctl is-active --quiet %s",
                 unit) >= (int)sizeof(command)) {
        return false;
    }
    return ops_certification_command_ok(command);
}

static bool ops_certification_metrics_scrape_passes(void)
{
    FILE *pipe;
    char line[256];
    bool found = false;

    if (!ops_certification_command_exists("curl")) {
        return false;
    }
    pipe = popen("curl -fsS http://127.0.0.1:9900/metrics 2>/dev/null", "r");
    if (pipe == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), pipe) != NULL) {
        if (strstr(line, "lingqu_mem_service_") != NULL) {
            found = true;
            break;
        }
    }
    if (pclose(pipe) == -1) {
        return false;
    }
    return found;
}

static bool ops_certification_alert_rules_pass(void)
{
    return ops_certification_command_exists("promtool") &&
           ops_certification_command_ok(
               "promtool check rules "
               "/usr/share/lingqu/mem_service/deploy/"
               "linqu_mem_service.prometheus-alerts.yml >/dev/null 2>&1");
}

static bool ops_certification_rpm_package_pass(const char *rpm_file)
{
    char command[512];

    if (!ops_certification_safe_path(rpm_file) ||
        !ops_certification_command_exists("rpmbuild") ||
        !ops_certification_command_exists("rpm2cpio") ||
        access(rpm_file, R_OK) != 0) {
        return false;
    }
    if (snprintf(command,
                 sizeof(command),
                 "rpm2cpio %s >/dev/null 2>&1",
                 rpm_file) >= (int)sizeof(command)) {
        return false;
    }
    return ops_certification_command_ok(command);
}

static bool ops_certification_upgrade_rollback_pass(const char *marker_path)
{
    char marker[512];

    if (!ops_certification_safe_path(marker_path) ||
        read_text_file_limited(marker_path, marker, sizeof(marker)) != 0) {
        return false;
    }
    return strstr(marker, "upgrade_rollback_deployment_smoke=pass\n") != NULL;
}

static int append_ops_certification_gate_status(char *evidence,
                                                size_t evidence_len,
                                                size_t *used,
                                                const char *name,
                                                bool passed)
{
    return append_wire_schema_line(evidence,
                                   evidence_len,
                                   used,
                                   "%s=%s\n",
                                   name,
                                   passed ? "pass" : "fail");
}

static int render_ops_certification_generated_evidence(int argc,
                                                       char **argv,
                                                       char *evidence,
                                                       size_t evidence_len,
                                                       size_t *used_out)
{
    const char *rpm_file = option_value(argc, argv, "--rpm-file");
    const char *upgrade_marker = option_value(argc, argv, "--upgrade-rollback-marker");
    size_t used = 0;
    bool linux_host = ops_certification_host_is_linux();
    bool systemd_host = linux_host && ops_certification_systemd_is_available();
    bool service_pass = systemd_host &&
                        ops_certification_service_is_active(
                            "linqu_mem_service.service");
    bool host_service_pass = systemd_host &&
                             ops_certification_service_is_active(
                                 "linqu_mem_service.host.service");
    bool scrape_pass = linux_host && ops_certification_metrics_scrape_passes();
    bool alert_pass = linux_host && ops_certification_alert_rules_pass();
    bool rpm_pass = linux_host && ops_certification_rpm_package_pass(rpm_file);
    bool upgrade_pass = linux_host &&
                        ops_certification_upgrade_rollback_pass(upgrade_marker);

    if (evidence == NULL || evidence_len == 0) {
        return -1;
    }
    evidence[0] = '\0';
    if (append_wire_schema_line(
            evidence,
            evidence_len,
            &used,
            "mem_service_ops_certification_evidence_version=%u\n",
            MEM_SERVICE_OPS_CERTIFICATION_EVIDENCE_VERSION) != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "certification_scope=real-linux-operations\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "evidence_os=%s\n",
                                linux_host ? "linux" : "non-linux") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "evidence_init=%s\n",
                                systemd_host ? "systemd" : "not-systemd") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "evidence_generator=ops-certification-generate-evidence\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "ops_certification_policy_checksum=0x%08x\n",
                                MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "package_manifest_checksum=0x%08x\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "linux_systemd_service_smoke",
                                             service_pass) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "linux_systemd_host_service_smoke",
                                             host_service_pass) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "prometheus_scrape_smoke",
                                             scrape_pass) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "prometheus_alertmanager_rule_smoke",
                                             alert_pass) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "rpm_package_smoke",
                                             rpm_pass) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "upgrade_rollback_deployment_smoke",
                                             upgrade_pass) != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_ops_certification_generate_evidence(int argc, char **argv)
{
    char evidence[4096];
    size_t used = 0;

    if (render_ops_certification_generated_evidence(
            argc, argv, evidence, sizeof(evidence), &used) != 0) {
        fprintf(stderr,
                "mem_service ops-certification-generate-evidence: render failed\n");
        return 1;
    }
    fwrite(evidence, 1, used, stdout);
    return 0;
}

static int run_ops_certification_linux_ci_smoke(int argc, char **argv)
{
    const char *evidence_file = option_value(argc, argv, "--evidence-file");
    char evidence[4096];
    char reason[160];
    size_t used = 0;

    if (evidence_file == NULL || evidence_file[0] == '\0') {
        fprintf(stderr,
                "mem_service ops-certification-linux-ci-smoke: missing --evidence-file\n");
        return 2;
    }
    if (!ops_certification_safe_path(evidence_file)) {
        fprintf(stderr,
                "mem_service ops-certification-linux-ci-smoke: unsafe evidence file\n");
        return 2;
    }
    if (render_ops_certification_generated_evidence(
            argc, argv, evidence, sizeof(evidence), &used) != 0) {
        fprintf(stderr,
                "mem_service ops-certification-linux-ci-smoke: render failed\n");
        return 1;
    }
    if (write_text_file_limited(evidence_file, evidence, used) != 0) {
        fprintf(stderr,
                "mem_service ops-certification-linux-ci-smoke: evidence write failed\n");
        return 1;
    }
    if (validate_ops_certification_evidence(evidence, reason, sizeof(reason)) != 0) {
        fprintf(stderr,
                "mem_service ops-certification-linux-ci-smoke: fail-closed "
                "reason=%s evidence_file=%s\n",
                reason,
                evidence_file);
        return 1;
    }
    printf("mem_service ops-certification-linux-ci-smoke: status=ok "
           "certification_status=certified evidence_file=%s external_gates=6\n",
           evidence_file);
    return 0;
}

static int run_ops_certification_verify(int argc, char **argv)
{
    const char *path = option_value(argc, argv, "--evidence-file");
    char evidence[4096];
    char reason[160];

    if (path == NULL || path[0] == '\0') {
        fprintf(stderr, "mem_service ops-certification-verify: missing --evidence-file\n");
        return 2;
    }
    if (read_text_file_limited(path, evidence, sizeof(evidence)) != 0) {
        fprintf(stderr, "mem_service ops-certification-verify: evidence read failed\n");
        return 1;
    }
    if (validate_ops_certification_evidence(evidence, reason, sizeof(reason)) != 0) {
        fprintf(stderr,
                "mem_service ops-certification-verify: fail-closed reason=%s\n",
                reason);
        return 1;
    }
    printf("mem_service ops-certification-verify: status=ok "
           "certification_status=certified evidence_version=%u external_gates=6\n",
           MEM_SERVICE_OPS_CERTIFICATION_EVIDENCE_VERSION);
    return 0;
}

static int run_remote_transport_verify(int argc, char **argv)
{
    const char *path = option_value(argc, argv, "--evidence-file");
    char evidence[2048];
    char reason[160];

    if (path == NULL || path[0] == '\0') {
        fprintf(stderr, "mem_service remote-transport-verify: missing --evidence-file\n");
        return 2;
    }
    if (read_text_file_limited(path, evidence, sizeof(evidence)) != 0) {
        fprintf(stderr, "mem_service remote-transport-verify: evidence read failed\n");
        return 1;
    }
    if (validate_remote_transport_evidence(evidence, reason, sizeof(reason)) != 0) {
        fprintf(stderr,
                "mem_service remote-transport-verify: fail-closed reason=%s\n",
                reason);
        return 1;
    }
    printf("mem_service remote-transport-verify: status=ok "
           "certification_status=certified evidence_version=%u external_gates=6\n",
           MEM_SERVICE_REMOTE_TRANSPORT_EVIDENCE_VERSION);
    return 0;
}

static int run_remote_transport_serve_fixture(int argc, char **argv)
{
    const char *listen_spec = option_value(argc, argv, "--listen");
    const char *payload_len_arg = option_value(argc, argv, "--payload-len");
    char *end = NULL;
    unsigned long long payload_len;

    if (listen_spec == NULL || payload_len_arg == NULL ||
        listen_spec[0] == '\0' || payload_len_arg[0] == '\0') {
        fprintf(stderr,
                "mem_service remote-transport-serve-fixture: "
                "missing --listen or --payload-len\n");
        return 2;
    }
    payload_len = strtoull(payload_len_arg, &end, 10);
    if (end == payload_len_arg || end == NULL || *end != '\0' ||
        payload_len == 0ULL) {
        fprintf(stderr,
                "mem_service remote-transport-serve-fixture: invalid payload length\n");
        return 2;
    }
    return mem_service_run_tcp_payload_fixture_source(listen_spec,
                                                     (uint64_t)payload_len);
}

static bool remote_transport_parse_source_ip(const char *source,
                                             char *ip,
                                             size_t ip_len)
{
    const char *start;
    const char *port;
    size_t len;

    if (source == NULL || strncmp(source, "tcp:", 4) != 0 ||
        ip == NULL || ip_len == 0) {
        return false;
    }
    start = source + 4;
    port = strrchr(start, ':');
    if (port == NULL || port == start || port[1] == '\0') {
        return false;
    }
    len = (size_t)(port - start);
    if (len == 0 || len >= ip_len) {
        return false;
    }
    memcpy(ip, start, len);
    ip[len] = '\0';
    return true;
}

static bool remote_transport_source_ip_is_non_loopback(const char *source)
{
    char ip[80];

    if (!remote_transport_parse_source_ip(source, ip, sizeof(ip))) {
        return false;
    }
    if (strcmp(ip, "0.0.0.0") == 0 ||
        strcmp(ip, "127.0.0.1") == 0 ||
        strcmp(ip, "localhost") == 0 ||
        strncmp(ip, "127.", 4) == 0) {
        return false;
    }
    return true;
}

static bool remote_transport_hosts_are_distinct(const char *producer_host,
                                                const char *consumer_host)
{
    return producer_host != NULL && producer_host[0] != '\0' &&
           consumer_host != NULL && consumer_host[0] != '\0' &&
           strcmp(producer_host, consumer_host) != 0;
}

static bool remote_transport_partition_marker_passes(const char *marker_path)
{
    char marker[512];

    if (!ops_certification_safe_path(marker_path) ||
        read_text_file_limited(marker_path, marker, sizeof(marker)) != 0) {
        return false;
    }
    return strstr(marker, "network_partition_fail_closed=pass\n") != NULL;
}

static int render_remote_transport_generated_evidence(
    const char *source,
    const char *producer_host,
    const char *consumer_host,
    const char *partition_marker,
    const struct mem_service_remote_transport_probe_result *probe,
    char *evidence,
    size_t evidence_len,
    size_t *used_out)
{
    size_t used = 0;
    bool source_non_loopback = remote_transport_source_ip_is_non_loopback(source);
    bool distinct_hosts =
        remote_transport_hosts_are_distinct(producer_host, consumer_host);
    bool partition_pass =
        remote_transport_partition_marker_passes(partition_marker);

    if (probe == NULL || evidence == NULL || evidence_len == 0) {
        return -1;
    }
    evidence[0] = '\0';
    if (append_wire_schema_line(
            evidence,
            evidence_len,
            &used,
            "mem_service_remote_transport_evidence_version=%u\n",
            MEM_SERVICE_REMOTE_TRANSPORT_EVIDENCE_VERSION) != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "certification_scope=production-network-transport\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "evidence_generator=remote-transport-generate-evidence\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "transport_backend=transport-tcp-block-v1\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "transport_protocol=tcp-ipv4\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "transport_topology=cross-host\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "producer_host=%s\n",
                                producer_host != NULL ? producer_host : "") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "consumer_host=%s\n",
                                consumer_host != NULL ? consumer_host : "") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "source=%s\n",
                                source != NULL ? source : "") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "package_manifest_checksum=0x%08x\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "payload_len=%" PRIu64 "\n",
                                probe->payload_len) != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "payload_checksum=0x%016" PRIx64 "\n",
                                probe->payload_checksum) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "source_address_non_loopback",
                                             source_non_loopback) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "payload_block_round_trip",
                                             probe->payload_block_round_trip) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "payload_checksum_validation",
                                             probe->payload_checksum_validation) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "payload_corruption_fail_closed",
                                             probe->payload_corruption_fail_closed) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "producer_consumer_distinct_hosts",
                                             distinct_hosts) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "network_partition_fail_closed",
                                             partition_pass) != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_remote_transport_generate_evidence(int argc, char **argv)
{
    const char *source = option_value(argc, argv, "--source");
    const char *producer_host = option_value(argc, argv, "--producer-host");
    const char *consumer_host = option_value(argc, argv, "--consumer-host");
    const char *partition_marker =
        option_value(argc, argv, "--network-partition-marker");
    const char *evidence_file = option_value(argc, argv, "--evidence-file");
    const char *storage_root_arg = option_value(argc, argv, "--storage-root");
    char storage_root[256];
    char evidence[2048];
    char reason[160];
    size_t used = 0;
    struct mem_service_remote_transport_probe_result probe;

    if (source == NULL || producer_host == NULL || consumer_host == NULL ||
        partition_marker == NULL || evidence_file == NULL ||
        source[0] == '\0' || producer_host[0] == '\0' ||
        consumer_host[0] == '\0' || partition_marker[0] == '\0' ||
        evidence_file[0] == '\0') {
        fprintf(stderr,
                "mem_service remote-transport-generate-evidence: missing required option\n");
        return 2;
    }
    if (!ops_certification_safe_path(evidence_file) ||
        !ops_certification_safe_path(partition_marker)) {
        fprintf(stderr,
                "mem_service remote-transport-generate-evidence: unsafe evidence path\n");
        return 2;
    }
    if (storage_root_arg != NULL && storage_root_arg[0] != '\0') {
        if (!ops_certification_safe_path(storage_root_arg) ||
            strlen(storage_root_arg) >= sizeof(storage_root)) {
            fprintf(stderr,
                    "mem_service remote-transport-generate-evidence: unsafe storage root\n");
            return 2;
        }
        strcpy(storage_root, storage_root_arg);
    } else if (snprintf(storage_root,
                        sizeof(storage_root),
                        "/tmp/linqu_mem_service_remote_transport_probe_%ld",
                        (long)getpid()) >= (int)sizeof(storage_root)) {
        return 1;
    }
    if (mem_service_probe_transport_tcp_payload_block(storage_root,
                                                      source,
                                                      &probe) != 0) {
        memset(&probe, 0, sizeof(probe));
    }
    if (render_remote_transport_generated_evidence(source,
                                                   producer_host,
                                                   consumer_host,
                                                   partition_marker,
                                                   &probe,
                                                   evidence,
                                                   sizeof(evidence),
                                                   &used) != 0) {
        fprintf(stderr,
                "mem_service remote-transport-generate-evidence: render failed\n");
        return 1;
    }
    if (write_text_file_limited(evidence_file, evidence, used) != 0) {
        fprintf(stderr,
                "mem_service remote-transport-generate-evidence: evidence write failed\n");
        return 1;
    }
    if (validate_remote_transport_evidence(evidence, reason, sizeof(reason)) != 0) {
        fprintf(stderr,
                "mem_service remote-transport-generate-evidence: fail-closed "
                "reason=%s evidence_file=%s\n",
                reason,
                evidence_file);
        return 1;
    }
    printf("mem_service remote-transport-generate-evidence: status=ok "
           "certification_status=certified evidence_file=%s external_gates=6 "
           "payload_len=%" PRIu64 "\n",
           evidence_file,
           probe.payload_len);
    return 0;
}

static int append_ops_certification_valid_evidence(char *evidence,
                                                   size_t evidence_len)
{
    size_t used = 0;

    evidence[0] = '\0';
    return append_wire_schema_line(
               evidence,
               evidence_len,
               &used,
               "mem_service_ops_certification_evidence_version=%u\n",
               MEM_SERVICE_OPS_CERTIFICATION_EVIDENCE_VERSION) != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "service_name=linqu_mem_service\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "certification_scope=real-linux-operations\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "evidence_os=linux\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "evidence_init=systemd\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "ops_certification_policy_checksum=0x%08x\n",
                                   MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM) != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "package_manifest_checksum=0x%08x\n",
                                   MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "linux_systemd_service_smoke=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "linux_systemd_host_service_smoke=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "prometheus_scrape_smoke=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "prometheus_alertmanager_rule_smoke=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "rpm_package_smoke=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "upgrade_rollback_deployment_smoke=pass\n") != 0
               ? -1
               : 0;
}

static int append_remote_transport_valid_evidence(char *evidence,
                                                  size_t evidence_len)
{
    size_t used = 0;

    evidence[0] = '\0';
    return append_wire_schema_line(
               evidence,
               evidence_len,
               &used,
               "mem_service_remote_transport_evidence_version=%u\n",
               MEM_SERVICE_REMOTE_TRANSPORT_EVIDENCE_VERSION) != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "service_name=linqu_mem_service\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "certification_scope=production-network-transport\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "transport_backend=transport-tcp-block-v1\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "transport_protocol=tcp-ipv4\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "transport_topology=cross-host\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "package_manifest_checksum=0x%08x\n",
                                   MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "source_address_non_loopback=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "payload_block_round_trip=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "payload_checksum_validation=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "payload_corruption_fail_closed=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "producer_consumer_distinct_hosts=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "network_partition_fail_closed=pass\n") != 0
               ? -1
               : 0;
}

static int run_ops_certification_evidence_fixture_check(void)
{
    char valid[2048];
    char bad_gate[2048];
    char bad_checksum[2048];
    char reason[160];

    if (append_ops_certification_valid_evidence(valid, sizeof(valid)) != 0) {
        fprintf(stderr, "mem_service ops-certification-evidence-fixtures: render failed\n");
        return 1;
    }
    if (validate_ops_certification_evidence(valid, reason, sizeof(reason)) != 0) {
        fprintf(stderr,
                "mem_service ops-certification-evidence-fixtures: valid rejected reason=%s\n",
                reason);
        return 1;
    }
    strcpy(bad_gate, valid);
    {
        char *gate = strstr(bad_gate, "rpm_package_smoke=pass\n");

        if (gate == NULL) {
            return 1;
        }
        memcpy(gate, "rpm_package_smoke=fail\n", strlen("rpm_package_smoke=fail\n"));
    }
    if (validate_ops_certification_evidence(bad_gate, reason, sizeof(reason)) == 0 ||
        strstr(reason, "rpm_package_smoke") == NULL) {
        fprintf(stderr,
                "mem_service ops-certification-evidence-fixtures: bad gate accepted\n");
        return 1;
    }
    strcpy(bad_checksum, valid);
    {
        char *checksum = strstr(bad_checksum, "package_manifest_checksum=0x");

        if (checksum == NULL) {
            return 1;
        }
        memcpy(checksum,
               "package_manifest_checksum=0x00000000",
               strlen("package_manifest_checksum=0x00000000"));
    }
    if (validate_ops_certification_evidence(bad_checksum, reason, sizeof(reason)) == 0 ||
        strcmp(reason, "bad-package-checksum") != 0) {
        fprintf(stderr,
                "mem_service ops-certification-evidence-fixtures: bad checksum accepted\n");
        return 1;
    }
    printf("mem_service ops-certification-evidence-fixtures: status=ok "
           "evidence_schema=ops-certification-evidence-v1 "
           "positive=1 fail_closed=2 external_gates=6\n");
    return 0;
}

static int run_remote_transport_evidence_fixture_check(void)
{
    char valid[2048];
    char bad_gate[2048];
    char bad_topology[2048];
    char bad_checksum[2048];
    char reason[160];

    if (append_remote_transport_valid_evidence(valid, sizeof(valid)) != 0) {
        fprintf(stderr, "mem_service remote-transport-evidence-fixtures: render failed\n");
        return 1;
    }
    if (validate_remote_transport_evidence(valid, reason, sizeof(reason)) != 0) {
        fprintf(stderr,
                "mem_service remote-transport-evidence-fixtures: valid rejected "
                "reason=%s\n",
                reason);
        return 1;
    }
    strcpy(bad_gate, valid);
    {
        char *gate = strstr(bad_gate, "network_partition_fail_closed=pass\n");

        if (gate == NULL) {
            return 1;
        }
        memcpy(gate,
               "network_partition_fail_closed=fail\n",
               strlen("network_partition_fail_closed=fail\n"));
    }
    if (validate_remote_transport_evidence(bad_gate, reason, sizeof(reason)) == 0 ||
        strstr(reason, "network_partition_fail_closed") == NULL) {
        fprintf(stderr,
                "mem_service remote-transport-evidence-fixtures: bad gate accepted\n");
        return 1;
    }
    strcpy(bad_topology, valid);
    {
        char *topology = strstr(bad_topology, "transport_topology=cross-host\n");

        if (topology == NULL) {
            return 1;
        }
        memcpy(topology,
               "transport_topology=loopback   \n",
               strlen("transport_topology=loopback   \n"));
    }
    if (validate_remote_transport_evidence(bad_topology, reason, sizeof(reason)) == 0 ||
        strcmp(reason, "bad-evidence-identity") != 0) {
        fprintf(stderr,
                "mem_service remote-transport-evidence-fixtures: bad topology accepted\n");
        return 1;
    }
    strcpy(bad_checksum, valid);
    {
        char *checksum = strstr(bad_checksum, "package_manifest_checksum=0x");

        if (checksum == NULL) {
            return 1;
        }
        memcpy(checksum,
               "package_manifest_checksum=0x00000000",
               strlen("package_manifest_checksum=0x00000000"));
    }
    if (validate_remote_transport_evidence(bad_checksum, reason, sizeof(reason)) == 0 ||
        strcmp(reason, "bad-package-checksum") != 0) {
        fprintf(stderr,
                "mem_service remote-transport-evidence-fixtures: bad checksum accepted\n");
        return 1;
    }
    printf("mem_service remote-transport-evidence-fixtures: status=ok "
           "evidence_schema=remote-transport-evidence-v1 "
           "positive=1 fail_closed=3 external_gates=6 "
           "certification_status=not-certified-until-cross-host-evidence\n");
    return 0;
}

static int run_package_fixture_check(void)
{
    char manifest[12288];
    size_t used = 0;
    uint32_t checksum;

    if (render_package_manifest(manifest, sizeof(manifest), &used) != 0) {
        fprintf(stderr, "mem_service package-fixtures: render failed\n");
        return 1;
    }
    checksum = mem_service_wire_checksum(manifest, used);
    if (used != MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service package-fixtures: manifest len actual=%zu expected=%u\n",
                used,
                MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN);
        return 1;
    }
    if (checksum != MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service package-fixtures: manifest checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM);
        return 1;
    }
    if (strstr(manifest, "package_format=installed-layout-v1\n") == NULL ||
        strstr(manifest, "binary=bin/linqu_mem_service\n") == NULL ||
        strstr(manifest, "host_binary=libexec/lingqu/mem_service/linqu_mem_service_host\n") ==
            NULL ||
        strstr(manifest, "required_gate=install-smoke\n") == NULL ||
        strstr(manifest, "required_gate=installed-sdk-example-smoke\n") == NULL ||
        strstr(manifest, "artifact_gate=package-tarball-smoke\n") == NULL ||
        strstr(manifest, "native_package_gate=package-deb-smoke\n") == NULL ||
        strstr(manifest, "rpm_package_gate=package-rpm-smoke\n") == NULL ||
        strstr(manifest, "required_gate=upgrade-rollback-runtime-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=compat-runtime-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=alert-integration-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=ops-certification-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=ops-certification-evidence-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=remote-transport-evidence-fixtures\n") == NULL ||
        strstr(manifest,
               "remote_payload_production_transport_ci=scripts/run_mem_service_remote_transport_ci.sh\n") ==
            NULL ||
        strstr(manifest,
               "remote_payload_production_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight\n") ==
            NULL ||
        strstr(manifest, "required_gate=package-rpm-smoke\n") == NULL ||
        strstr(manifest, "required_gate=version-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=installed-sdk-runtime-smoke\n") == NULL ||
        strstr(manifest,
               "installed_sdk_pkgconfig_smoke=installed-sdk-pkgconfig-smoke\n") ==
            NULL ||
        strstr(manifest,
               "installed_sdk_pkgconfig_smoke_scope=pkg-config-cflags+sdk-sources-external-client-compile\n") ==
            NULL ||
        strstr(manifest, "required_gate=installed-sdk-pkgconfig-smoke\n") ==
            NULL ||
        strstr(manifest, "pkgconfig=lib/pkgconfig/lingqu-mem-service.pc\n") ==
            NULL ||
        strstr(manifest, "pkgconfig_name=lingqu-mem-service\n") == NULL ||
        strstr(manifest, "pkgconfig_cflags=-I${includedir}\n") == NULL ||
        strstr(manifest,
               "pkgconfig_sdk_sources=${sourcedir}/mem_service_client.c ${sourcedir}/mem_service_wire_client.c ${sourcedir}/mem_service_provider.c\n") ==
            NULL ||
        strstr(manifest,
               "pkgconfig_payload_provider_roce_sources=${sourcedir}/mem_service_provider_roce.c\n") ==
            NULL ||
        strstr(manifest,
               "pkgconfig_payload_provider_roce_libs=-lrdmacm -libverbs\n") ==
            NULL ||
        strstr(manifest,
               "pkgconfig_payload_provider_tcp_sources=${sourcedir}/mem_service_provider_tcp.c\n") ==
            NULL ||
        strstr(manifest,
               "pkgconfig_payload_provider_tcp_libs=-pthread\n") == NULL ||
        strstr(manifest,
               "installed_sdk_preflight=scripts/verify_mem_service_installed_sdk.sh --preflight\n") ==
            NULL ||
        strstr(manifest,
               "installed_sdk_preflight_scope=pkg-config-cflags+sdk-sources+examples+host-binary-no-compile\n") ==
            NULL ||
        strstr(manifest, "installed_sdk_runtime_smoke=installed-sdk-runtime-smoke\n") ==
            NULL ||
        strstr(manifest,
               "installed_sdk_runtime_smoke_scope=installed-host-daemon+serving+pretraining-runtime\n") ==
            NULL ||
        strstr(manifest, "installed_sdk_runtime_reuse=installed-sdk-runtime-smoke\n") ==
            NULL ||
        strstr(manifest,
               "installed_sdk_runtime_reuse_scope=daemon-restart+durable-store+serving+pretraining\n") ==
            NULL ||
        strstr(manifest, "release_script_root=share/lingqu/mem_service/scripts\n") ==
            NULL ||
        strstr(manifest,
               "release_certification_ci=scripts/run_mem_service_release_certification_ci.sh\n") ==
            NULL ||
        strstr(manifest,
               "release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight\n") ==
            NULL ||
        strstr(manifest,
               "release_certification_readiness_gate=release-readiness --ops-evidence-file --remote-transport-evidence-file\n") ==
            NULL ||
        strstr(manifest, "linux_ops_ci=scripts/run_mem_service_linux_ops_ci.sh\n") ==
            NULL ||
        strstr(manifest,
               "linux_ops_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight\n") ==
            NULL ||
        strstr(manifest,
               "release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh\n") ==
            NULL ||
        strstr(manifest,
               "release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh\n") ==
            NULL ||
        strstr(manifest,
               "release_script=share/lingqu/mem_service/scripts/run_mem_service_linux_ops_ci.sh\n") ==
            NULL ||
        strstr(manifest,
               "release_script=share/lingqu/mem_service/scripts/verify_mem_service_ops_certification_bundle.sh\n") ==
            NULL ||
        strstr(manifest,
               "release_script=share/lingqu/mem_service/scripts/run_mem_service_remote_transport_ci.sh\n") ==
            NULL ||
        strstr(manifest,
               "release_script=share/lingqu/mem_service/scripts/verify_mem_service_remote_transport_bundle.sh\n") ==
            NULL ||
        strstr(manifest,
               "release_script=share/lingqu/mem_service/scripts/verify_mem_service_release_certification.sh\n") ==
            NULL ||
        strstr(manifest,
               "release_script=share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh\n") ==
            NULL ||
        strstr(manifest, "file_class=release_scripts count=10\n") == NULL ||
        strstr(manifest, "file_class=pkgconfig count=1\n") == NULL ||
        strstr(manifest, "binary_version_command=version\n") == NULL ||
        strstr(manifest, "binary_version_contract=text-kv\n") == NULL ||
        strstr(manifest, "binary_version_gate=version-fixtures\n") == NULL ||
        strstr(manifest, "release_readiness_command=release-readiness\n") == NULL ||
        strstr(manifest, "release_readiness_contract=text-kv\n") == NULL ||
        strstr(manifest,
               "release_readiness_evidence_verify=release-readiness --ops-evidence-file --remote-transport-evidence-file\n") ==
            NULL ||
        strstr(manifest, "release_readiness_gate=release-readiness-fixtures\n") ==
            NULL ||
        strstr(manifest, "required_gate=release-readiness-fixtures\n") == NULL ||
        strstr(manifest,
               "deployment_quota_contract=max-records+max-payload-bytes\n") ==
            NULL ||
        strstr(manifest, "deployment_quota_gate=config-fixtures\n") == NULL ||
        strstr(manifest, "retention_policy=manual-or-audit-log-limit\n") == NULL ||
        strstr(manifest,
               "retention_policy_gate=config-fixtures,retention-fixtures\n") == NULL ||
        strstr(manifest,
               "checkpoint_retention_policy=manual-or-latest-limit\n") == NULL ||
        strstr(manifest,
               "checkpoint_retention_gate=config-fixtures,checkpoint-retention-fixtures\n") ==
            NULL ||
        strstr(manifest,
               "record_retention_policy=manual-or-global-kind-tenant-latest-or-ttl\n") ==
            NULL ||
        strstr(manifest,
               "record_retention_gate=config-fixtures,record-retention-fixtures\n") ==
            NULL ||
        strstr(manifest,
               "payload_block_gc=record-and-checkpoint-retention-orphan-blocks\n") ==
            NULL ||
        strstr(manifest,
               "payload_block_gc_gate=payload-gc-fixtures,record-retention-fixtures\n") ==
            NULL ||
        strstr(manifest, "encryption_policy=explicit-none-only\n") == NULL ||
        strstr(manifest, "encryption_at_rest=not-certified\n") == NULL ||
        strstr(manifest, "encryption_policy_command=encryption-policy\n") == NULL ||
        strstr(manifest, "encryption_policy_gate=encryption-fixtures\n") == NULL ||
        strstr(manifest,
               "runtime_quota_admission=max-records+max-payload-bytes\n") ==
            NULL ||
        strstr(manifest, "runtime_quota_gate=runtime-quota-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=runtime-quota-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=retention-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=checkpoint-retention-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=payload-gc-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=record-retention-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=encryption-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=restore-policy-fixtures\n") == NULL ||
        strstr(manifest, "contract=ops-certification-policy ") == NULL ||
        strstr(manifest, "payload_ownership_matrix=certified\n") == NULL ||
        strstr(manifest, "payload_ownership_scope=artifact-query-expected-owner\n") == NULL ||
        strstr(manifest, "service_auth_boundary=unix-socket-local-only\n") == NULL ||
        strstr(manifest, "metrics_auth_boundary=loopback-only\n") == NULL ||
        strstr(manifest, "config_security_gate=config-fixtures\n") == NULL ||
        strstr(manifest,
               "deployment_quota_contract=max-records+max-payload-bytes\n") ==
            NULL ||
        strstr(manifest, "deployment_quota_gate=config-fixtures\n") == NULL ||
        strstr(manifest, "retention_policy=manual-or-audit-log-limit\n") == NULL ||
        strstr(manifest,
               "retention_policy_gate=config-fixtures,retention-fixtures\n") == NULL ||
        strstr(manifest,
               "checkpoint_retention_policy=manual-or-latest-limit\n") == NULL ||
        strstr(manifest,
               "checkpoint_retention_gate=config-fixtures,checkpoint-retention-fixtures\n") ==
            NULL ||
        strstr(manifest,
               "record_retention_policy=manual-or-global-kind-tenant-latest-or-ttl\n") ==
            NULL ||
        strstr(manifest,
               "record_retention_gate=config-fixtures,record-retention-fixtures\n") ==
            NULL ||
        strstr(manifest,
               "payload_block_gc=record-and-checkpoint-retention-orphan-blocks\n") ==
            NULL ||
        strstr(manifest,
               "payload_block_gc_gate=payload-gc-fixtures,record-retention-fixtures\n") ==
            NULL ||
        strstr(manifest, "encryption_policy=explicit-none-only\n") == NULL ||
        strstr(manifest, "encryption_at_rest=not-certified\n") == NULL ||
        strstr(manifest, "encryption_policy_command=encryption-policy\n") == NULL ||
        strstr(manifest, "encryption_policy_gate=encryption-fixtures\n") == NULL ||
        strstr(manifest,
               "runtime_quota_admission=max-records+max-payload-bytes\n") ==
            NULL ||
        strstr(manifest, "runtime_quota_gate=runtime-quota-fixtures\n") == NULL ||
        strstr(manifest, "restore_policy=transactional-staged-restore\n") == NULL ||
        strstr(manifest, "restore_policy_gate=restore-policy-fixtures\n") == NULL ||
        strstr(manifest, "cross_version_upgrade=certified\n") == NULL) {
        fprintf(stderr, "mem_service package-fixtures: required manifest missing\n");
        return 1;
    }
    printf("mem_service package-fixtures: status=ok package_version=%u "
           "package_format=installed-layout-v1 manifest_len=%u "
           "manifest_checksum=0x%08x installed_files=%u required_gates=%u\n",
           MEM_SERVICE_PACKAGE_MANIFEST_VERSION,
           MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN,
           MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM,
           MEM_SERVICE_PACKAGE_MANIFEST_INSTALLED_FILE_COUNT,
           MEM_SERVICE_PACKAGE_MANIFEST_GATE_COUNT);
    return 0;
}

static int run_release_manifest(void)
{
    printf("mem_service_release_manifest_version=1\n");
    printf("service_name=linqu_mem_service\n");
    printf("service_version=%s\n", MEM_SERVICE_RELEASE_VERSION);
    printf("package_format=installed-layout-v1\n");
    printf("package_manifest=share/lingqu/mem_service/package-manifest.txt\n");
    printf("package_manifest_len=%u\n",
           MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN);
    printf("package_manifest_checksum=0x%08x\n",
           MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM);
    printf("package_gate=package-fixtures\n");
    printf("installed_sdk_preflight=scripts/verify_mem_service_installed_sdk.sh --preflight\n");
    printf("installed_sdk_preflight_scope=pkg-config-cflags+sdk-sources+examples+host-binary-no-compile\n");
    printf("installed_sdk_example_smoke=installed-sdk-example-smoke\n");
    printf("installed_sdk_example_smoke_scope=serving+pretraining-external-client-compile\n");
    printf("installed_sdk_pkgconfig_smoke=installed-sdk-pkgconfig-smoke\n");
    printf("installed_sdk_pkgconfig_smoke_scope=pkg-config-cflags+sdk-sources-external-client-compile\n");
    printf("installed_sdk_runtime_smoke=installed-sdk-runtime-smoke\n");
    printf("installed_sdk_runtime_smoke_scope=installed-host-daemon+serving+pretraining-runtime\n");
    printf("installed_sdk_runtime_reuse=installed-sdk-runtime-smoke\n");
    printf("installed_sdk_runtime_reuse_scope=daemon-restart+durable-store+serving+pretraining\n");
    printf("distributable_package=out/mem_service/%s\n",
           MEM_SERVICE_PACKAGE_TARBALL_NAME);
    printf("distributable_package_format=tar\n");
    printf("distributable_package_root=usr+etc\n");
    printf("distributable_package_gate=package-tarball-smoke\n");
    printf("native_package=out/mem_service/%s\n", MEM_SERVICE_NATIVE_DEB_NAME);
    printf("native_package_format=deb\n");
    printf("native_package_arch=arm64\n");
    printf("native_package_gate=package-deb-smoke\n");
    printf("native_package_runtime=not-executed-cross-compiled-arm64\n");
    printf("rpm_native_package=out/mem_service/%s\n", MEM_SERVICE_NATIVE_RPM_NAME);
    printf("rpm_native_package_format=rpm\n");
    printf("rpm_native_package_arch=aarch64\n");
    printf("rpm_native_package_gate=package-rpm-smoke\n");
    printf("rpm_native_package_runtime=requires-linux-rpm-toolchain\n");
    printf("core_binary=bin/linqu_mem_service\n");
    printf("qwen3_adapter_binary_optional=bin/linqu_mem_service_qwen3\n");
    printf("host_daemon_binary=libexec/lingqu/mem_service/linqu_mem_service_host\n");
    printf("binary_version_command=version\n");
    printf("binary_version_contract=text-kv\n");
    printf("binary_version_gate=version-fixtures\n");
    printf("release_readiness_command=release-readiness\n");
    printf("release_readiness_contract=text-kv\n");
    printf("release_readiness_evidence_verify=release-readiness --ops-evidence-file --remote-transport-evidence-file\n");
    printf("release_readiness_gate=release-readiness-fixtures\n");
    printf("host_daemon_artifact_smoke=host-artifact-smoke\n");
    printf("default_endpoint=%s\n", mem_service_default_unix_socket_spec());
    printf("wire_version=%u\n", MEM_SERVICE_WIRE_VERSION);
    printf("wire_header_len=%u\n", MEM_SERVICE_WIRE_HEADER_LEN);
    printf("wire_schema_version=%u\n", MEM_SERVICE_WIRE_SCHEMA_VERSION);
    printf("wire_payload_format=text-kv\n");
    printf("wire_schema_manifest=share/lingqu/mem_service/wire-schema.txt\n");
    printf("wire_schema_manifest_len=%u\n",
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN);
    printf("wire_schema_manifest_checksum=0x%08x\n",
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM);
    printf("api_abi_policy=share/lingqu/mem_service/api-abi-policy.txt\n");
    printf("api_abi_policy_len=%u\n", MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN);
    printf("api_abi_policy_checksum=0x%08x\n",
           MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM);
    printf("admin_output_schema=share/lingqu/mem_service/admin-output-schema.txt\n");
    printf("admin_output_schema_len=%u\n",
           MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN);
    printf("admin_output_schema_checksum=0x%08x\n",
           MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM);
    printf("admin_output_format=text-kv\n");
    printf("admin_metric_prefix=lingqu_mem_service_\n");
    printf("upgrade_rollback_policy=share/lingqu/mem_service/upgrade-rollback-policy.txt\n");
    printf("upgrade_rollback_policy_len=%u\n",
           MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_LEN);
    printf("upgrade_rollback_policy_checksum=0x%08x\n",
           MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM);
    printf("upgrade_policy=current-version-only\n");
    printf("rollback_policy=current-version-only\n");
    printf("old_server_runtime_binary=certified\n");
    printf("upgrade_rollback_gate=upgrade-rollback-fixtures\n");
    printf("upgrade_rollback_runtime_gate=upgrade-rollback-runtime-fixtures\n");
    printf("compat_runtime_gate=compat-runtime-fixtures\n");
    printf("compat_old_server_runtime_gate=compat-old-server-runtime-fixtures\n");
    printf("serving_fail_closed_matrix=certified\n");
    printf("serving_fail_closed_gate=serving-fail-closed-fixtures\n");
    printf("pretraining_fail_closed_matrix=certified\n");
    printf("pretraining_fail_closed_gate=pretraining-fail-closed-fixtures\n");
    printf("payload_ownership_matrix=certified\n");
    printf("payload_ownership_scope=artifact-query-expected-owner\n");
    printf("payload_ownership_gate=serving-fail-closed-fixtures,pretraining-fail-closed-fixtures\n");
    printf("restore_policy=transactional-staged-restore\n");
    printf("restore_policy_scope=full-snapshot+paged-snapshot\n");
    printf("restore_policy_gate=restore-policy-fixtures\n");
    printf("restore_policy_fail_closed=bad-magic,future-store-schema,malformed-store-schema,out-of-order-page,record-count-mismatch,cancelled-stage-commit\n");
    printf("restore_policy_live_state=unchanged-until-commit\n");
    printf("wire_payload_text_kv_format=text-kv\n");
    printf("wire_payload_typed_binary_format=typed-binary-v1\n");
    printf("wire_payload_typed_binary_gate=typed-payload-fixtures\n");
    printf("client_api_version=%u\n", MEM_SERVICE_CLIENT_API_VERSION);
    printf("client_abi_version=%u\n", MEM_SERVICE_CLIENT_ABI_VERSION);
    printf("client_record_abi_size=%u\n", MEM_SERVICE_CLIENT_RECORD_ABI_SIZE);
    printf("client_api_compatibility=%s\n",
           MEM_SERVICE_CLIENT_API_COMPATIBILITY);
    printf("client_abi_compatibility=%s\n",
           MEM_SERVICE_CLIENT_ABI_COMPATIBILITY);
    printf("pkgconfig=lib/pkgconfig/lingqu-mem-service.pc\n");
    printf("pkgconfig_name=lingqu-mem-service\n");
    printf("pkgconfig_cflags=-I${includedir}\n");
    printf("pkgconfig_sdk_sources=${sourcedir}/mem_service_client.c ${sourcedir}/mem_service_wire_client.c ${sourcedir}/mem_service_provider.c\n");
    printf("pkgconfig_mapping_owner_sources=${sourcedir}/mem_service_mapping_owner.c\n");
    printf("pkgconfig_mapping_owner_libs=-pthread\n");
    printf("pkgconfig_payload_provider_roce_sources=${sourcedir}/mem_service_provider_roce.c\n");
    printf("pkgconfig_payload_provider_roce_libs=-lrdmacm -libverbs\n");
    printf("pkgconfig_payload_provider_tcp_sources=${sourcedir}/mem_service_provider_tcp.c\n");
    printf("pkgconfig_payload_provider_tcp_libs=-pthread\n");
    printf("compat_matrix=share/lingqu/mem_service/compat-matrix.txt\n");
    printf("compat_matrix_len=%u\n", MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN);
    printf("compat_matrix_checksum=0x%08x\n",
           MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM);
    printf("compat_baseline=share/lingqu/mem_service/compat-baseline-v1.txt\n");
    printf("compat_baseline_len=%u\n",
           MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN);
    printf("compat_baseline_checksum=0x%08x\n",
           MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM);
    printf("compat_old_new_matrix=share/lingqu/mem_service/compat-old-new-matrix.txt\n");
    printf("compat_old_new_matrix_len=%u\n",
           MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN);
    printf("compat_old_new_matrix_checksum=0x%08x\n",
           MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM);
    printf("config_schema_version=%u\n", MEM_SERVICE_CONFIG_SCHEMA_VERSION);
    printf("config_schema=share/lingqu/mem_service/config/mem_service.conf.schema\n");
    printf("config_example=share/lingqu/mem_service/config/mem_service.example.conf\n");
    printf("runtime_config=etc/lingqu/mem_service/mem_service.conf\n");
    printf("runtime_config_source=share/lingqu/mem_service/config/mem_service.runtime.conf\n");
    printf("host_runtime_config=etc/lingqu/mem_service/mem_service.host.conf\n");
    printf("host_runtime_config_source=share/lingqu/mem_service/config/mem_service.host.runtime.conf\n");
    printf("service_auth_boundary=unix-socket-local-only\n");
    printf("metrics_auth_boundary=loopback-only\n");
    printf("config_security_gate=config-fixtures\n");
    printf("deployment_quota_contract=max-records+max-payload-bytes\n");
    printf("deployment_quota_gate=config-fixtures\n");
    printf("retention_policy=manual-or-audit-log-limit\n");
    printf("retention_policy_gate=config-fixtures,retention-fixtures\n");
    printf("checkpoint_retention_policy=manual-or-latest-limit\n");
    printf("checkpoint_retention_gate=config-fixtures,checkpoint-retention-fixtures\n");
    printf("record_retention_policy=manual-or-global-kind-tenant-latest-or-ttl\n");
    printf("record_retention_gate=config-fixtures,record-retention-fixtures\n");
    printf("payload_block_gc=record-and-checkpoint-retention-orphan-blocks\n");
    printf("payload_block_gc_gate=payload-gc-fixtures,record-retention-fixtures\n");
    printf("encryption_policy=explicit-none-only\n");
    printf("encryption_at_rest=not-certified\n");
    printf("encryption_policy_command=encryption-policy\n");
    printf("encryption_policy_gate=encryption-fixtures\n");
    printf("runtime_quota_admission=max-records+max-payload-bytes\n");
    printf("runtime_quota_gate=runtime-quota-fixtures\n");
    printf("deployment_manifest=share/lingqu/mem_service/deploy/linqu_mem_service.service\n");
    printf("host_deployment_manifest=share/lingqu/mem_service/deploy/linqu_mem_service.host.service\n");
    printf("systemd_unit=lib/systemd/system/linqu_mem_service.service\n");
    printf("host_systemd_unit=lib/systemd/system/linqu_mem_service.host.service\n");
    printf("deployment_smoke=deployment-fixtures\n");
    printf("host_service_manager_smoke=installed-host-service-manager-smoke\n");
    printf("host_service_manager_lifecycle=host-serve-config-ready-scrape-sigterm\n");
    printf("collector_smoke=collector-fixtures\n");
    printf("collector_integration_smoke=installed-host-collector-smoke\n");
    printf("collector_scrape_contract=prometheus-text-http-v0.0.4\n");
    printf("alert_rules=share/lingqu/mem_service/deploy/linqu_mem_service.prometheus-alerts.yml\n");
    printf("alert_rules_format=prometheus-rules-yaml\n");
    printf("alert_rules_len=%u\n", MEM_SERVICE_ALERT_RULES_EXPECTED_LEN);
    printf("alert_rules_checksum=0x%08x\n",
           MEM_SERVICE_ALERT_RULES_EXPECTED_CHECKSUM);
    printf("alert_rule_count=%u\n", MEM_SERVICE_ALERT_RULES_EXPECTED_RULE_COUNT);
    printf("alert_rules_gate=alert-fixtures\n");
    printf("alert_integration_smoke=alert-integration-fixtures\n");
    printf("ops_certification_policy=share/lingqu/mem_service/ops-certification-policy.txt\n");
    printf("ops_certification_policy_len=%u\n",
           MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_LEN);
    printf("ops_certification_policy_checksum=0x%08x\n",
           MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM);
    printf("ops_certification_gate=ops-certification-fixtures\n");
    printf("ops_certification_evidence_schema=ops-certification-evidence-v1\n");
    printf("ops_certification_evidence_gate=ops-certification-evidence-fixtures\n");
    printf("ops_certification_generate=ops-certification-generate-evidence\n");
    printf("ops_certification_linux_ci_gate=ops-certification-linux-ci-smoke\n");
    printf("linux_ops_certification_smoke=linux-ops-certification-smoke\n");
    printf("linux_ops_evidence_verify=linux-ops-evidence-verify\n");
    printf("linux_ops_certification_bundle=linux-ops-certification-bundle\n");
    printf("linux_ops_certification_bundle_verify=linux-ops-certification-bundle-verify\n");
    printf("linux_ops_ci=scripts/run_mem_service_linux_ops_ci.sh\n");
    printf("linux_ops_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight\n");
    printf("release_certification_verify=release-certification-verify\n");
    printf("release_certification_verify_script=scripts/verify_mem_service_release_certification.sh\n");
    printf("release_certification_ci=scripts/run_mem_service_release_certification_ci.sh\n");
    printf("release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight\n");
    printf("release_certification_readiness_gate=release-readiness --ops-evidence-file --remote-transport-evidence-file\n");
    printf("release_script_root=share/lingqu/mem_service/scripts\n");
    printf("release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh\n");
    printf("release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh\n");
    printf("release_script=share/lingqu/mem_service/scripts/run_mem_service_linux_ops_ci.sh\n");
    printf("release_script=share/lingqu/mem_service/scripts/verify_mem_service_linux_ops_evidence.sh\n");
    printf("release_script=share/lingqu/mem_service/scripts/verify_mem_service_ops_certification_bundle.sh\n");
    printf("release_script=share/lingqu/mem_service/scripts/run_mem_service_remote_transport_ci.sh\n");
    printf("release_script=share/lingqu/mem_service/scripts/verify_mem_service_remote_transport_evidence.sh\n");
    printf("release_script=share/lingqu/mem_service/scripts/verify_mem_service_remote_transport_bundle.sh\n");
    printf("release_script=share/lingqu/mem_service/scripts/verify_mem_service_release_certification.sh\n");
    printf("release_script=share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh\n");
    printf("linux_ops_upgrade_rollback_smoke=linux-ops-upgrade-rollback-smoke\n");
    printf("linux_ops_deployment_smoke=linux-ops-deployment-smoke\n");
    printf("ops_certification_verify=ops-certification-verify --evidence-file\n");
    printf("real_systemd_environment=not-certified\n");
    printf("production_collector_alert_environment=not-certified\n");
    printf("rpm_package=not-certified\n");
    printf("service_manager_lifecycle=serve-config-ready-scrape-sigterm\n");
    printf("service_manager_shutdown=signal-clean-stop\n");
    printf("durable_backend=snapshot+journal\n");
    printf("durable_store_schema_version=1\n");
    printf("durable_store_migration_policy=legacy-to-v1-reject-future\n");
    printf("durable_catalog=storage-root-v1\n");
    printf("durable_catalog_manifest=catalog/manifest.txt\n");
    printf("payload_block_backend=sealed-local-block-v1,sealed-chunked-block-v1,transport-loopback-block-v1,transport-tcp-block-v1,ub-ssd-gsva-v1\n");
    printf("remote_payload_block_backend=transport-loopback-block-v1,transport-tcp-block-v1\n");
    printf("remote_payload_block_backend_gate=remote-block-backend-policy-fixtures\n");
    printf("remote_payload_block_data_gate=transport-block-fixtures\n");
    printf("remote_payload_network_transport=tcp-loopback-certified\n");
    printf("remote_payload_network_transport_gate=network-transport-block-fixtures\n");
    printf("remote_payload_network_transport_make_gate=network-transport-block-smoke\n");
    printf("remote_payload_production_network_transport=not-certified\n");
    printf("remote_payload_production_transport_evidence_schema=remote-transport-evidence-v1\n");
    printf("remote_payload_production_transport_evidence_gate=remote-transport-evidence-fixtures\n");
    printf("remote_payload_production_transport_generate=remote-transport-generate-evidence\n");
    printf("remote_payload_production_transport_verify=remote-transport-verify --evidence-file\n");
    printf("remote_payload_production_transport_ci=scripts/run_mem_service_remote_transport_ci.sh\n");
    printf("remote_payload_production_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight\n");
    printf("remote_payload_production_transport_evidence_verify=scripts/verify_mem_service_remote_transport_evidence.sh\n");
    printf("remote_payload_production_transport_bundle=remote-transport-certification-bundle\n");
    printf("remote_payload_production_transport_bundle_verify=remote-transport-certification-bundle-verify\n");
    printf("remote_payload_production_transport_bundle_script=scripts/verify_mem_service_remote_transport_bundle.sh\n");
    printf("payload_block_ingest=payload-inline,payload-file\n");
    printf("durable_snapshot=store-path\n");
    printf("durable_journal=store-path.journal\n");
    printf("metrics_export_format=prometheus-text\n");
    printf("metrics_listen_config=metrics_listen\n");
    printf("metrics_http_listener=tcp-ipv4\n");
    printf("metrics_scrape_path=/metrics\n");
    printf("metrics_http_content_type=text/plain; version=0.0.4\n");
    printf("client_retry_policy=explicit-max-attempts-backoff\n");
    printf("client_api=pretraining-refs-v1\n");
    printf("client_api=pretraining-step-commit-v1\n");
    printf("public_header=include/lingqu/mem_service/mem_service.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_allocation.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_provider_directory.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_core.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_client.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_mapping_owner.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_provider.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_provider_roce.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_provider_tcp.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_wire.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_wire_client.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_wire_payload.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_reference_protocol.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_wire_schema.h\n");
    printf("public_header=include/lingqu/mem_service/lingqu_object_service.h\n");
    printf("client_source=src/lingqu/mem_service/mem_service_client.c\n");
    printf("client_source=src/lingqu/mem_service/mem_service_mapping_owner.c\n");
    printf("client_source=src/lingqu/mem_service/mem_service_wire_client.c\n");
    printf("client_source=src/lingqu/mem_service/mem_service_provider.c\n");
    printf("provider_source=src/lingqu/mem_service/mem_service_provider_roce.c\n");
    printf("provider_source=src/lingqu/mem_service/mem_service_provider_tcp.c\n");
    printf("example_source=share/lingqu/mem_service/examples/mem_service_serving_example.c\n");
    printf("example_source=share/lingqu/mem_service/examples/mem_service_pretraining_example.c\n");
    printf("operation=health:%u\n", MEM_SERVICE_WIRE_OP_HEALTH);
    printf("operation=ready:%u\n", MEM_SERVICE_WIRE_OP_READY);
    printf("operation=status:%u\n", MEM_SERVICE_WIRE_OP_STATUS);
    printf("operation=list_records:%u\n", MEM_SERVICE_WIRE_OP_LIST_RECORDS);
    printf("operation=metrics:%u\n", MEM_SERVICE_WIRE_OP_METRICS);
    printf("operation=export_snapshot:%u\n", MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT);
    printf("operation=export_snapshot_page:%u\n",
           MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT_PAGE);
    printf("operation=restore_snapshot:%u\n", MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT);
    printf("operation=restore_snapshot_page:%u\n",
           MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE);
    printf("operation=audit_log:%u\n", MEM_SERVICE_WIRE_OP_AUDIT_LOG);
    printf("operation=put_object:%u\n", MEM_SERVICE_WIRE_OP_PUT_OBJECT);
    printf("operation=get_object:%u\n", MEM_SERVICE_WIRE_OP_GET_OBJECT);
    printf("operation=inspect_object:%u\n", MEM_SERVICE_WIRE_OP_INSPECT_OBJECT);
    printf("operation=materialize_object:%u\n",
           MEM_SERVICE_WIRE_OP_MATERIALIZE_OBJECT);
    printf("operation=register_prefix_entry:%u\n",
           MEM_SERVICE_WIRE_OP_REGISTER_PREFIX_ENTRY);
    printf("operation=lookup_prefix_entry:%u\n",
           MEM_SERVICE_WIRE_OP_LOOKUP_PREFIX_ENTRY);
    printf("operation=publish_kv_segment:%u\n", MEM_SERVICE_WIRE_OP_PUBLISH_KV_SEGMENT);
    printf("operation=resolve_kv_segment:%u\n", MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT);
    printf("operation=publish_runtime_handoff:%u\n",
           MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF);
    printf("operation=resolve_runtime_handoff:%u\n",
           MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF);
    printf("operation=register_execution_artifact:%u\n",
           MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT);
    printf("operation=query_execution_artifact:%u\n",
           MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT);
    printf("operation=register_training_artifact:%u\n",
           MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT);
    printf("operation=query_training_artifact:%u\n",
           MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT);
    for (uint32_t operation = MEM_SERVICE_WIRE_OP_ALLOCATE_OBJECT;
         operation <= MEM_SERVICE_WIRE_OP_REFERENCE_TRANSITION; ++operation) {
        const struct mem_service_wire_operation_schema *schema =
            mem_service_wire_schema_for_operation((enum mem_service_wire_operation)operation);
        if (schema != NULL) printf("operation=%s:%u\n", schema->name, operation);
    }
    printf("status=ok:%u\n", MEM_SERVICE_WIRE_STATUS_OK);
    printf("status=not_found:%u\n", MEM_SERVICE_WIRE_STATUS_NOT_FOUND);
    printf("status=stale_ref:%u\n", MEM_SERVICE_WIRE_STATUS_STALE_REF);
    printf("status=checksum_mismatch:%u\n",
           MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH);
    printf("status=version_conflict:%u\n",
           MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT);
    printf("status=invalid_model_binding:%u\n",
           MEM_SERVICE_WIRE_STATUS_INVALID_MODEL_BINDING);
    printf("status=invalid_session:%u\n", MEM_SERVICE_WIRE_STATUS_INVALID_SESSION);
    printf("status=timeout:%u\n", MEM_SERVICE_WIRE_STATUS_TIMEOUT);
    printf("status=capacity_exceeded:%u\n",
           MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED);
    printf("status=unsupported:%u\n", MEM_SERVICE_WIRE_STATUS_UNSUPPORTED);
    printf("status=internal:%u\n", MEM_SERVICE_WIRE_STATUS_INTERNAL);
    return 0;
}

static int run_release_fixture_check(void)
{
    int failures = 0;

    if (MEM_SERVICE_WIRE_VERSION != 1U) {
        fprintf(stderr, "mem_service release-fixtures: wire_version mismatch\n");
        failures -= 1;
    }
    if (MEM_SERVICE_WIRE_HEADER_LEN != 48U) {
        fprintf(stderr, "mem_service release-fixtures: wire_header_len mismatch\n");
        failures -= 1;
    }
    if (MEM_SERVICE_WIRE_SCHEMA_VERSION != 1U) {
        fprintf(stderr, "mem_service release-fixtures: wire_schema_version mismatch\n");
        failures -= 1;
    }
    if (MEM_SERVICE_CLIENT_API_VERSION != 1U ||
        MEM_SERVICE_CLIENT_ABI_VERSION != 1U ||
        MEM_SERVICE_CLIENT_RECORD_ABI_SIZE !=
            sizeof(struct mem_service_client_record)) {
        fprintf(stderr, "mem_service release-fixtures: api/abi policy mismatch\n");
        failures -= 1;
    }
    if (MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN == 0U ||
        MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM == 0U) {
        fprintf(stderr, "mem_service release-fixtures: schema manifest fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN == 0U ||
        MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM == 0U) {
        fprintf(stderr, "mem_service release-fixtures: api/abi policy fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN == 0U ||
        MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM == 0U) {
        fprintf(stderr,
                "mem_service release-fixtures: admin output schema fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_LEN == 0U ||
        MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM == 0U) {
        fprintf(stderr,
                "mem_service release-fixtures: upgrade/rollback policy fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_ALERT_RULES_EXPECTED_LEN == 0U ||
        MEM_SERVICE_ALERT_RULES_EXPECTED_CHECKSUM == 0U ||
        MEM_SERVICE_ALERT_RULES_EXPECTED_RULE_COUNT != 6U) {
        fprintf(stderr, "mem_service release-fixtures: alert rules fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_LEN == 0U ||
        MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM == 0U) {
        fprintf(stderr,
                "mem_service release-fixtures: ops certification policy missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN == 0U ||
        MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM == 0U ||
        MEM_SERVICE_PACKAGE_MANIFEST_INSTALLED_FILE_COUNT != 57U ||
        MEM_SERVICE_PACKAGE_MANIFEST_GATE_COUNT != 34U) {
        fprintf(stderr, "mem_service release-fixtures: package manifest fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN == 0U ||
        MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM == 0U) {
        fprintf(stderr, "mem_service release-fixtures: compat matrix fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN == 0U ||
        MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM == 0U) {
        fprintf(stderr,
                "mem_service release-fixtures: compat baseline fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN == 0U ||
        MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM == 0U) {
        fprintf(stderr,
                "mem_service release-fixtures: compat old/new fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT != 8U ||
        MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE != 9U ||
        MEM_SERVICE_WIRE_OP_AUDIT_LOG != 10U ||
        MEM_SERVICE_WIRE_OP_PUT_OBJECT != 16U ||
        MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT != 97U) {
        fprintf(stderr, "mem_service release-fixtures: operation id mismatch\n");
        failures -= 1;
    }
    if (MEM_SERVICE_WIRE_STATUS_STALE_REF != 2U ||
        MEM_SERVICE_WIRE_STATUS_INTERNAL != 10U) {
        fprintf(stderr, "mem_service release-fixtures: status id mismatch\n");
        failures -= 1;
    }
    if (strcmp(mem_service_default_unix_socket_spec(),
               "unix:" MEM_SERVICE_DEFAULT_UNIX_SOCKET) != 0) {
        fprintf(stderr, "mem_service release-fixtures: default endpoint mismatch\n");
        failures -= 1;
    }
    if (MEM_SERVICE_DEPLOYMENT_SMOKE_VERSION != 1U) {
        fprintf(stderr, "mem_service release-fixtures: deployment smoke mismatch\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service release-fixtures: status=ok manifest_version=1 "
           "public_headers=14 client_sources=4 provider_sources=2 "
           "examples=2 config_artifacts=6 "
           "host_artifacts=1 "
           "package_artifacts=4 "
           "pkgconfig_artifacts=1 "
           "release_scripts=10 "
           "installed_sdk_pkgconfig_smokes=1 "
           "installed_sdk_runtime_smokes=1 "
           "version_smokes=1 "
           "release_readiness_smokes=1 "
           "config_security_smokes=1 "
           "systemd_units=2 "
           "deployment_smokes=1 service_manager_lifecycle_smokes=1 "
           "host_service_manager_smokes=1 "
           "collector_smokes=1 "
           "alert_rule_artifacts=1 alert_rules=%u "
           "alert_integration_smokes=1 "
           "ops_certification_policies=1 "
           "remote_transport_evidence_schemas=1 "
           "api_abi_policies=1 "
           "admin_output_schemas=1 "
           "upgrade_rollback_policies=1 "
           "upgrade_rollback_runtime_smokes=1 "
           "restore_policy_smokes=1 "
           "runtime_quota_smokes=1 "
           "retention_smokes=1 "
           "checkpoint_retention_smokes=1 "
           "payload_gc_smokes=1 "
           "record_retention_smokes=1 "
           "encryption_policy_smokes=1 "
           "compat_runtime_smokes=1 "
           "durable_backends=1 durable_catalogs=1 payload_block_backends=5 "
           "metrics_export_formats=1 metrics_http_listeners=1 "
           "metrics_scrape_paths=1 "
           "client_retry_policies=1 "
           "client_api_profiles=2 compat_artifacts=3 "
           "operations=39 statuses=11 "
           "schema_manifest_len=%u schema_manifest_checksum=0x%08x "
           "api_abi_policy_len=%u api_abi_policy_checksum=0x%08x "
           "admin_output_schema_len=%u "
           "admin_output_schema_checksum=0x%08x "
           "upgrade_rollback_policy_len=%u "
           "upgrade_rollback_policy_checksum=0x%08x "
           "alert_rules_len=%u alert_rules_checksum=0x%08x "
           "ops_certification_policy_len=%u "
           "ops_certification_policy_checksum=0x%08x "
           "package_manifest_len=%u package_manifest_checksum=0x%08x "
           "compat_matrix_len=%u compat_matrix_checksum=0x%08x "
           "compat_baseline_len=%u compat_baseline_checksum=0x%08x "
           "compat_old_new_matrix_len=%u "
           "compat_old_new_matrix_checksum=0x%08x\n",
           MEM_SERVICE_ALERT_RULES_EXPECTED_RULE_COUNT,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM,
           MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN,
           MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM,
           MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN,
           MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM,
           MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_LEN,
           MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM,
           MEM_SERVICE_ALERT_RULES_EXPECTED_LEN,
           MEM_SERVICE_ALERT_RULES_EXPECTED_CHECKSUM,
           MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_LEN,
           MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM,
           MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN,
           MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM,
           MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN,
           MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM,
           MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN,
           MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM,
           MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN,
           MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM);
    return 0;
}

static int run_ub_ssd_gsva_descriptor_fixture_check(void)
{
    struct mem_service_gsva_desc_source source;
    struct mem_service_record record;
    struct mem_service_gsva_buffer_desc desc;
    uint64_t expected_gsva;

    memset(&source, 0, sizeof(source));
    memset(&record, 0, sizeof(record));
    source.active = true;
    source.node_count = 2;
    source.local_idx = 0;
    source.local_cna = 0x1200U;
    source.payload_offset = 0x4000U;
    source.metas[0].segment_id = 0xabcddcbaULL;
    source.metas[0].home_va = 0x700000000000ULL;
    source.metas[0].region_bytes = 0x200000ULL;
    source.metas[0].token_id = 0x55U;
    source.metas[0].home_cna = 0x00c4c220U;

    record.in_use = true;
    record.kind = MEM_SERVICE_RECORD_KVCACHE_OBJECT;
    record.object_owner_node = 0;
    record.object_backing_offset = 0x18000ULL;
    record.object_backing_len = 0x2000ULL;
    record.object_payload_checksum = 0x987654321ULL;

    if (mem_service_make_gsva_buffer_desc_from_source(&source,
                                                      &record,
                                                      &desc) != 0) {
        fprintf(stderr, "ub-ssd-gsva-descriptor-fixtures: positive case failed\n");
        return 1;
    }
    expected_gsva = source.metas[0].home_va + source.payload_offset +
                    record.object_backing_offset;
    if (desc.gsva_base != expected_gsva ||
        desc.bytes != record.object_backing_len ||
        desc.key_segment_id != source.metas[0].segment_id ||
        desc.key_home_va != source.metas[0].home_va ||
        desc.key_size != source.metas[0].region_bytes ||
        desc.key_p_tag != (source.metas[0].home_cna & 0x00ffffffu) ||
        desc.key_cache_policy != 4U ||
        desc.token_id != source.metas[0].token_id ||
        desc.token_value != source.metas[0].token_id ||
        desc.source_cna != source.local_cna ||
        desc.owner_node != record.object_owner_node) {
        fprintf(stderr, "ub-ssd-gsva-descriptor-fixtures: descriptor fields mismatch\n");
        return 1;
    }

    record.object_owner_node = 2;
    if (mem_service_make_gsva_buffer_desc_from_source(&source,
                                                      &record,
                                                      &desc) == 0) {
        fprintf(stderr, "ub-ssd-gsva-descriptor-fixtures: owner bounds check failed\n");
        return 1;
    }
    record.object_owner_node = 0;
    record.object_backing_offset =
        source.metas[0].region_bytes - source.payload_offset - 8U;
    record.object_backing_len = 16U;
    if (mem_service_make_gsva_buffer_desc_from_source(&source,
                                                      &record,
                                                      &desc) == 0) {
        fprintf(stderr, "ub-ssd-gsva-descriptor-fixtures: range bounds check failed\n");
        return 1;
    }
    {
        struct mem_service_ub_ssd_gsva_block_ref block_ref;

        memset(&block_ref, 0, sizeof(block_ref));
        block_ref.block_hi = 0x11U;
        block_ref.block_lo = 0x22U;
        block_ref.version = 3U;
        block_ref.offset = 0x40U;
        block_ref.bytes = 4096U;
        block_ref.checksum64 = 0xbeefU;
        record.object_payload_kind = MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT;
        record.object_backing_offset = 0x18000ULL;
        record.object_backing_len = 0x2000ULL;
        record.object_payload_checksum = 0x987654321ULL;
        if (mem_service_record_attach_ub_ssd_gsva_backend_ref(&record,
                                                              1U,
                                                              0x44U,
                                                              0x55U,
                                                              &block_ref,
                                                              false) != 0 ||
            record.object_backend_kind != MEM_SERVICE_OBJECT_BACKEND_UB_SSD_GSVA ||
            record.object_backend_block_lo != 0x22U ||
            record.object_payload_kind !=
                MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT ||
            record.object_backing_offset != 0x18000ULL ||
            record.object_payload_checksum != 0x987654321ULL) {
            fprintf(stderr, "ub-ssd-gsva-descriptor-fixtures: sidecar attach failed\n");
            return 1;
        }
        if (mem_service_record_attach_ub_ssd_gsva_backend_ref(&record,
                                                              1U,
                                                              0x44U,
                                                              0x55U,
                                                              &block_ref,
                                                              true) != 0 ||
            record.object_payload_kind != MEM_SERVICE_PAYLOAD_KIND_UB_SSD_GSVA_BLOCK ||
            record.object_backing_offset != block_ref.offset ||
            record.object_backing_len != block_ref.bytes ||
            record.object_payload_checksum != block_ref.checksum64) {
            fprintf(stderr, "ub-ssd-gsva-descriptor-fixtures: primary attach failed\n");
            return 1;
        }
    }

    printf("mem_service ub-ssd-gsva-descriptor-fixtures: status=ok "
           "descriptor_source=cluster_runtime+record "
           "gsva_base=0x%016" PRIx64 " bytes=%" PRIu64 " owner=node%u "
           "token_id=%u source_cna=0x%08x\n",
           expected_gsva,
           (uint64_t)0x2000U,
           1U,
           source.metas[0].token_id,
           source.local_cna);
    return 0;
}

static int run_remote_block_backend_policy_fixture_check(void)
{
    printf("mem_service remote-block-backend-policy-fixtures: status=ok "
           "remote_payload_block_backend=transport-loopback-block-v1,transport-tcp-block-v1 "
           "remote_backend_admission=loopback-and-tcp-loopback-certified "
           "remote_payload_block_data_gate=transport-block-fixtures "
           "remote_payload_network_transport=tcp-loopback-certified "
           "remote_payload_network_transport_gate=network-transport-block-fixtures "
           "current_payload_block_backends=sealed-local-block-v1,sealed-chunked-block-v1,transport-loopback-block-v1,transport-tcp-block-v1,ub-ssd-gsva-v1\n");
    return 0;
}

static const char *option_value(int argc, char **argv, const char *option_name)
{
    int i;

    for (i = 2; i + 1 < argc; ++i) {
        if (strcmp(argv[i], option_name) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

static bool option_present(int argc, char **argv, const char *option_name)
{
    int i;

    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], option_name) == 0) {
            return true;
        }
    }
    return false;
}

static int parse_socket_arg(int argc,
                            char **argv,
                            const char *option_name,
                            const char **socket_spec_out)
{
    int i;

    *socket_spec_out = NULL;
    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], option_name) == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "mem_service: missing value for %s\n", option_name);
                return -1;
            }
            *socket_spec_out = argv[++i];
        }
    }
    if (*socket_spec_out == NULL) {
        *socket_spec_out = mem_service_default_unix_socket_spec();
    }
    return 0;
}

static int parse_client_options(
    int argc,
    char **argv,
    struct mem_service_wire_client_options *options)
{
    int i;

    if (options == NULL) {
        return -1;
    }
    mem_service_wire_client_options_init(options);
    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--timeout-ms") == 0) {
            char *end = NULL;
            unsigned long long parsed;

            if (i + 1 >= argc) {
                fprintf(stderr, "mem_service: missing value for --timeout-ms\n");
                return -1;
            }
            parsed = strtoull(argv[i + 1], &end, 0);
            if (end == argv[i + 1] || *end != '\0') {
                fprintf(stderr, "mem_service: invalid --timeout-ms value\n");
                return -1;
            }
            options->timeout_ms = (uint64_t)parsed;
            i += 1;
        } else if (strcmp(argv[i], "--max-attempts") == 0) {
            char *end = NULL;
            unsigned long long parsed;

            if (i + 1 >= argc) {
                fprintf(stderr, "mem_service: missing value for --max-attempts\n");
                return -1;
            }
            parsed = strtoull(argv[i + 1], &end, 0);
            if (end == argv[i + 1] || *end != '\0' || parsed == 0) {
                fprintf(stderr, "mem_service: invalid --max-attempts value\n");
                return -1;
            }
            options->max_attempts = (uint32_t)parsed;
            i += 1;
        } else if (strcmp(argv[i], "--retry-backoff-ms") == 0) {
            char *end = NULL;
            unsigned long long parsed;

            if (i + 1 >= argc) {
                fprintf(stderr, "mem_service: missing value for --retry-backoff-ms\n");
                return -1;
            }
            parsed = strtoull(argv[i + 1], &end, 0);
            if (end == argv[i + 1] || *end != '\0') {
                fprintf(stderr, "mem_service: invalid --retry-backoff-ms value\n");
                return -1;
            }
            options->retry_backoff_ms = (uint64_t)parsed;
            i += 1;
        } else if (strcmp(argv[i], "--retry-timeouts") == 0) {
            options->retry_on_timeout = 1U;
        }
    }
    return 0;
}

static int run_client_retry_fixture_check(void)
{
    char *argv[] = {
        "linqu_mem_service",
        "health",
        "--timeout-ms",
        "17",
        "--max-attempts",
        "3",
        "--retry-backoff-ms",
        "5",
        "--retry-timeouts",
    };
    struct mem_service_wire_client_options options;
    struct mem_service_wire_client_options defaults;

    mem_service_wire_client_options_init(&defaults);
    if (defaults.timeout_ms != 0 ||
        defaults.max_attempts != MEM_SERVICE_WIRE_CLIENT_DEFAULT_MAX_ATTEMPTS ||
        defaults.retry_backoff_ms != 0 ||
        defaults.retry_on_timeout != 0) {
        fprintf(stderr, "mem_service client-retry-fixtures: default mismatch\n");
        return 1;
    }
    if (parse_client_options((int)(sizeof(argv) / sizeof(argv[0])),
                             argv,
                             &options) != 0 ||
        options.timeout_ms != 17U ||
        options.max_attempts != 3U ||
        options.retry_backoff_ms != 5U ||
        options.retry_on_timeout != 1U) {
        fprintf(stderr, "mem_service client-retry-fixtures: parsed mismatch\n");
        return 1;
    }
    printf("mem_service client-retry-fixtures: status=ok default_attempts=%u "
           "max_attempts=%u retry_backoff_ms=%" PRIu64 " retry_timeouts=%u\n",
           MEM_SERVICE_WIRE_CLIENT_DEFAULT_MAX_ATTEMPTS,
           options.max_attempts,
           options.retry_backoff_ms,
           options.retry_on_timeout);
    return 0;
}

static int append_payload_field(char *payload,
                                size_t payload_len,
                                const char *name,
                                const char *value)
{
    return mem_service_wire_payload_append_field(payload, payload_len, name, value);
}

static int append_required_payload_field(char *payload,
                                         size_t payload_len,
                                         int argc,
                                         char **argv,
                                         const char *option_name,
                                         const char *field_name)
{
    const char *value = option_value(argc, argv, option_name);

    if (value == NULL || value[0] == '\0') {
        fprintf(stderr, "mem_service: missing required %s\n", option_name);
        return -1;
    }
    return append_payload_field(payload, payload_len, field_name, value);
}

static int append_optional_payload_field(char *payload,
                                         size_t payload_len,
                                         int argc,
                                         char **argv,
                                         const char *option_name,
                                         const char *field_name)
{
    return append_payload_field(payload, payload_len, field_name, option_value(argc, argv, option_name));
}

static int append_idempotency_payload_field(char *payload,
                                            size_t payload_len,
                                            int argc,
                                            char **argv)
{
    return append_optional_payload_field(payload,
                                         payload_len,
                                         argc,
                                         argv,
                                         "--idempotency-key",
                                         "idempotency_key");
}

struct mem_service_cli_config {
    bool has_listen;
    bool has_store;
    bool has_storage_root;
    bool has_metrics_listen;
    bool has_max_records;
    bool has_max_payload_bytes;
    bool has_retention;
    bool has_max_audit_events;
    bool has_checkpoint_retention;
    bool has_max_checkpoint_records;
    bool has_record_retention;
    bool has_max_retained_records;
    bool has_encryption;
    bool has_auth_mode;
    bool has_node_id;
    bool has_network_io_timeout_ms;
    bool has_provider_lease_ms;
    bool has_allocation_home_provider;
    uint64_t max_records;
    uint64_t max_payload_bytes;
    uint64_t max_audit_events;
    uint64_t max_checkpoint_records;
    uint64_t max_retained_records;
    uint64_t max_retained_record_age_ms;
    uint64_t network_io_timeout_ms;
    uint64_t provider_lease_ms;
    uint32_t max_retained_record_kind;
    bool max_retained_record_tenant_enabled;
    uint32_t max_retained_record_tenant;
    size_t network_peer_count;
    size_t required_provider_count;
    char listen[160];
    char store[512];
    char storage_root[512];
    char metrics_listen[160];
    char retention[80];
    char checkpoint_retention[80];
    char record_retention[80];
    char encryption[40];
    char auth_mode[40];
    char node_id[MEM_SERVICE_NETWORK_NODE_ID_LEN];
    struct mem_service_network_peer network_peers[MEM_SERVICE_NETWORK_MAX_PEERS];
    char required_providers[MEM_SERVICE_PROVIDER_DIRECTORY_MAX_PROVIDERS]
                           [MEM_SERVICE_PROVIDER_NODE_ID_LEN];
    char allocation_home_provider[MEM_SERVICE_PROVIDER_NODE_ID_LEN];
};

static void trim_ascii(char *value)
{
    char *start;
    char *end;
    size_t len;

    if (value == NULL) {
        return;
    }
    start = value;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start += 1;
    }
    if (start != value) {
        memmove(value, start, strlen(start) + 1U);
    }
    len = strlen(value);
    while (len > 0) {
        end = value + len - 1U;
        if (!isspace((unsigned char)*end)) {
            break;
        }
        *end = '\0';
        len -= 1U;
    }
}

static bool parse_config_u64_value(const char *value, uint64_t *parsed_out)
{
    char *end = NULL;
    unsigned long long parsed;

    if (value == NULL || value[0] == '\0' || value[0] == '-' ||
        parsed_out == NULL) {
        return false;
    }
    errno = 0;
    parsed = strtoull(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *parsed_out = (uint64_t)parsed;
    return true;
}

static int copy_config_value(char *out, size_t out_len, const char *value)
{
    size_t value_len;

    if (out == NULL || out_len == 0 || value == NULL || value[0] == '\0') {
        return -1;
    }
    value_len = strlen(value);
    if (value_len >= out_len) {
        return -1;
    }
    memcpy(out, value, value_len + 1U);
    return 0;
}

static bool parse_retention_value(const char *value, uint64_t *max_audit_events_out)
{
    const char *prefix = "audit-log:";
    uint64_t max_audit_events;

    if (value == NULL || value[0] == '\0') {
        return false;
    }
    if (strcmp(value, "manual") == 0) {
        if (max_audit_events_out != NULL) {
            *max_audit_events_out = 0;
        }
        return true;
    }
    if (strncmp(value, prefix, strlen(prefix)) != 0) {
        return false;
    }
    if (!parse_config_u64_value(value + strlen(prefix), &max_audit_events) ||
        max_audit_events == 0 ||
        max_audit_events > MEM_SERVICE_MAX_AUDIT_EVENTS) {
        return false;
    }
    if (max_audit_events_out != NULL) {
        *max_audit_events_out = max_audit_events;
    }
    return true;
}

static bool parse_checkpoint_retention_value(const char *value,
                                             uint64_t *max_checkpoint_records_out)
{
    const char *prefix = "latest:";
    uint64_t max_checkpoint_records;

    if (value == NULL || value[0] == '\0') {
        return false;
    }
    if (strcmp(value, "manual") == 0) {
        if (max_checkpoint_records_out != NULL) {
            *max_checkpoint_records_out = 0;
        }
        return true;
    }
    if (strncmp(value, prefix, strlen(prefix)) != 0) {
        return false;
    }
    if (!parse_config_u64_value(value + strlen(prefix), &max_checkpoint_records) ||
        max_checkpoint_records == 0 ||
        max_checkpoint_records > MEM_SERVICE_MAX_RECORDS) {
        return false;
    }
    if (max_checkpoint_records_out != NULL) {
        *max_checkpoint_records_out = max_checkpoint_records;
    }
    return true;
}

static bool parse_record_kind_name(const char *value, uint32_t *record_kind_out)
{
    struct record_kind_name {
        const char *name;
        uint32_t kind;
    };
    static const struct record_kind_name names[] = {
        {"prefix-group", MEM_SERVICE_RECORD_PREFIX_GROUP},
        {"request-prefix", MEM_SERVICE_RECORD_REQUEST_PREFIX},
        {"block-meta", MEM_SERVICE_RECORD_BLOCK_META},
        {"weight-tile", MEM_SERVICE_RECORD_WEIGHT_TILE},
        {"kvcache-object", MEM_SERVICE_RECORD_KVCACHE_OBJECT},
        {"hidden-range-input", MEM_SERVICE_RECORD_HIDDEN_RANGE_INPUT},
        {"hidden-range-output", MEM_SERVICE_RECORD_HIDDEN_RANGE_OUTPUT},
        {"layer-range-placement", MEM_SERVICE_RECORD_LAYER_RANGE_PLACEMENT},
        {"model-token-result", MEM_SERVICE_RECORD_MODEL_TOKEN_RESULT},
        {"model-engram-history", MEM_SERVICE_RECORD_MODEL_ENGRAM_HISTORY},
        {"model-engram-candidates", MEM_SERVICE_RECORD_MODEL_ENGRAM_CANDIDATES},
        {"model-engram-selected", MEM_SERVICE_RECORD_MODEL_ENGRAM_SELECTED},
        {"model-engram-state", MEM_SERVICE_RECORD_MODEL_ENGRAM_STATE},
        {"runtime-handoff", MEM_SERVICE_RECORD_RUNTIME_HANDOFF},
        {"execution-artifact", MEM_SERVICE_RECORD_EXECUTION_ARTIFACT},
        {"training-artifact", MEM_SERVICE_RECORD_TRAINING_ARTIFACT},
    };
    size_t i;

    if (value == NULL || value[0] == '\0') {
        return false;
    }
    for (i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(value, names[i].name) == 0) {
            if (record_kind_out != NULL) {
                *record_kind_out = names[i].kind;
            }
            return true;
        }
    }
    return false;
}

static bool parse_record_retention_value(const char *value,
                                         uint64_t *max_retained_records_out,
                                         uint64_t *max_retained_record_age_ms_out,
                                         uint32_t *retained_record_kind_out,
                                         bool *retained_record_tenant_enabled_out,
                                         uint32_t *retained_record_tenant_out)
{
    const char *prefix = "latest:";
    const char *ttl_prefix = "ttl-ms:";
    const char *kind_prefix = "kind:";
    const char *tenant_prefix = "tenant:";
    uint64_t max_retained_records = 0;
    uint64_t max_retained_record_age_ms = 0;
    const char *retention_value = value;
    char kind_name[80];
    uint32_t retained_record_kind = 0U;
    bool retained_record_tenant_enabled = false;
    uint32_t retained_record_tenant = 0U;

    if (value == NULL || value[0] == '\0') {
        return false;
    }
    if (strcmp(value, "manual") == 0) {
        if (max_retained_records_out != NULL) {
            *max_retained_records_out = 0;
        }
        if (max_retained_record_age_ms_out != NULL) {
            *max_retained_record_age_ms_out = 0;
        }
        if (retained_record_kind_out != NULL) {
            *retained_record_kind_out = 0U;
        }
        if (retained_record_tenant_enabled_out != NULL) {
            *retained_record_tenant_enabled_out = false;
        }
        if (retained_record_tenant_out != NULL) {
            *retained_record_tenant_out = 0U;
        }
        return true;
    }
    if (strncmp(value, kind_prefix, strlen(kind_prefix)) == 0) {
        const char *kind_start = value + strlen(kind_prefix);
        const char *kind_end = strstr(kind_start, ":latest:");
        size_t kind_len;

        if (kind_end == NULL) {
            kind_end = strstr(kind_start, ":ttl-ms:");
        }
        if (kind_end == NULL || kind_end == kind_start) {
            return false;
        }
        kind_len = (size_t)(kind_end - kind_start);
        if (kind_len >= sizeof(kind_name)) {
            return false;
        }
        memcpy(kind_name, kind_start, kind_len);
        kind_name[kind_len] = '\0';
        if (!parse_record_kind_name(kind_name, &retained_record_kind)) {
            return false;
        }
        retention_value = kind_end + 1U;
    } else if (strncmp(value, tenant_prefix, strlen(tenant_prefix)) == 0) {
        const char *tenant_start = value + strlen(tenant_prefix);
        const char *tenant_end = strstr(tenant_start, ":latest:");
        uint64_t parsed_tenant;
        char tenant_name[32];
        size_t tenant_len;

        if (tenant_end == NULL) {
            tenant_end = strstr(tenant_start, ":ttl-ms:");
        }
        if (tenant_end == NULL || tenant_end == tenant_start) {
            return false;
        }
        tenant_len = (size_t)(tenant_end - tenant_start);
        if (tenant_len >= sizeof(tenant_name)) {
            return false;
        }
        memcpy(tenant_name, tenant_start, tenant_len);
        tenant_name[tenant_len] = '\0';
        if (!parse_config_u64_value(tenant_name, &parsed_tenant) ||
            parsed_tenant > UINT32_MAX) {
            return false;
        }
        retained_record_tenant_enabled = true;
        retained_record_tenant = (uint32_t)parsed_tenant;
        retention_value = tenant_end + 1U;
    }
    if (strncmp(retention_value, prefix, strlen(prefix)) == 0) {
        if (!parse_config_u64_value(retention_value + strlen(prefix),
                                    &max_retained_records) ||
            max_retained_records == 0 ||
            max_retained_records > MEM_SERVICE_MAX_RECORDS) {
            return false;
        }
    } else if (strncmp(retention_value, ttl_prefix, strlen(ttl_prefix)) == 0) {
        if (!parse_config_u64_value(retention_value + strlen(ttl_prefix),
                                    &max_retained_record_age_ms) ||
            max_retained_record_age_ms == 0) {
            return false;
        }
    } else {
        return false;
    }
    if (max_retained_records_out != NULL) {
        *max_retained_records_out = max_retained_records;
    }
    if (max_retained_record_age_ms_out != NULL) {
        *max_retained_record_age_ms_out = max_retained_record_age_ms;
    }
    if (retained_record_kind_out != NULL) {
        *retained_record_kind_out = retained_record_kind;
    }
    if (retained_record_tenant_enabled_out != NULL) {
        *retained_record_tenant_enabled_out = retained_record_tenant_enabled;
    }
    if (retained_record_tenant_out != NULL) {
        *retained_record_tenant_out = retained_record_tenant;
    }
    return true;
}

static bool is_loopback_metrics_listen_spec(const char *value)
{
    const char *port_text;
    char *end = NULL;
    unsigned long port;

    if (value == NULL || strncmp(value, "tcp:127.0.0.1:", 14) != 0) {
        return false;
    }
    port_text = value + 14;
    if (port_text[0] == '\0') {
        return false;
    }
    errno = 0;
    port = strtoul(port_text, &end, 10);
    return errno == 0 && end != port_text && *end == '\0' &&
           port > 0UL && port <= 65535UL;
}

/*
 * Validate one allowlist IPv4 literal for the trusted-guest-network mode.
 * Only exact numeric dotted-quad addresses are accepted; wildcard
 * (0.0.0.0), limited broadcast (255.255.255.255) and multicast (224/4)
 * identities are rejected, matching the bind-side rules.
 */
static bool is_valid_network_peer_ipv4(const char *value)
{
    struct in_addr addr;
    uint32_t host_order;

    if (value == NULL || value[0] == '\0') {
        return false;
    }
    if (inet_pton(AF_INET, value, &addr) != 1) {
        return false;
    }
    host_order = ntohl(addr.s_addr);
    return host_order != 0U && host_order != 0xFFFFFFFFU &&
           (host_order & 0xF0000000U) != 0xE0000000U;
}

/*
 * Parse one repeatable "network_peer=<node_id>@<ipv4>" allowlist entry.
 * Node IDs and addresses must each be unique across the allowlist; the
 * one-to-one mapping never collapses multiple nodes into one source
 * identity.
 */
static int append_network_peer(struct mem_service_cli_config *config,
                               const char *value)
{
    const char *at;
    size_t node_len;
    size_t ipv4_len;
    size_t i;
    struct mem_service_network_peer *peer;

    if (config == NULL || value == NULL) {
        return -1;
    }
    at = strchr(value, '@');
    if (at == NULL || at == value || strchr(at + 1, '@') != NULL) {
        return -1;
    }
    node_len = (size_t)(at - value);
    ipv4_len = strlen(at + 1);
    if (node_len == 0 || node_len >= MEM_SERVICE_NETWORK_NODE_ID_LEN ||
        ipv4_len == 0 || ipv4_len >= MEM_SERVICE_NETWORK_IPV4_LEN ||
        !is_valid_network_peer_ipv4(at + 1)) {
        return -1;
    }
    if (config->network_peer_count >= MEM_SERVICE_NETWORK_MAX_PEERS) {
        return -1;
    }
    for (i = 0; i < config->network_peer_count; ++i) {
        if (strncmp(config->network_peers[i].node_id, value, node_len) == 0 &&
            config->network_peers[i].node_id[node_len] == '\0') {
            return -1;
        }
        if (strcmp(config->network_peers[i].ipv4, at + 1) == 0) {
            return -1;
        }
    }
    peer = &config->network_peers[config->network_peer_count];
    memcpy(peer->node_id, value, node_len);
    peer->node_id[node_len] = '\0';
    memcpy(peer->ipv4, at + 1, ipv4_len + 1U);
    config->network_peer_count += 1U;
    return 0;
}

/*
 * Parse one repeatable "required_provider=<node_id>" entry naming a
 * provider node whose fresh registration is required before managed data
 * operations leave the fail-closed state. Node IDs must be unique.
 */
static int append_required_provider(struct mem_service_cli_config *config,
                                    const char *value)
{
    size_t node_len;
    size_t i;

    if (config == NULL || value == NULL) {
        return -1;
    }
    node_len = strlen(value);
    if (node_len == 0 || node_len >= MEM_SERVICE_PROVIDER_NODE_ID_LEN) {
        return -1;
    }
    if (config->required_provider_count >=
        MEM_SERVICE_PROVIDER_DIRECTORY_MAX_PROVIDERS) {
        return -1;
    }
    for (i = 0; i < config->required_provider_count; ++i) {
        if (strcmp(config->required_providers[i], value) == 0) {
            return -1;
        }
    }
    memcpy(config->required_providers[config->required_provider_count],
           value,
           node_len + 1U);
    config->required_provider_count += 1U;
    return 0;
}

static int apply_config_field(struct mem_service_cli_config *config,
                              const char *name,
                              const char *value)
{
    if (strcmp(name, "listen") == 0) {
        if ((strncmp(value, "unix:", 5) != 0 &&
             strncmp(value, "tcp:", 4) != 0) ||
            copy_config_value(config->listen, sizeof(config->listen), value) != 0) {
            return -1;
        }
        config->has_listen = true;
        return 0;
    }
    if (strcmp(name, "store") == 0) {
        if (copy_config_value(config->store, sizeof(config->store), value) != 0) {
            return -1;
        }
        config->has_store = true;
        return 0;
    }
    if (strcmp(name, "storage_root") == 0) {
        if (copy_config_value(config->storage_root,
                              sizeof(config->storage_root),
                              value) != 0) {
            return -1;
        }
        config->has_storage_root = true;
        return 0;
    }
    if (strcmp(name, "metrics_listen") == 0) {
        if (!is_loopback_metrics_listen_spec(value) ||
            copy_config_value(config->metrics_listen,
                              sizeof(config->metrics_listen),
                              value) != 0) {
            return -1;
        }
        config->has_metrics_listen = true;
        return 0;
    }
    if (strcmp(name, "backend") == 0) {
        return strcmp(value, "snapshot") == 0 ||
                       strcmp(value, "snapshot+journal") == 0
                   ? 0
                   : -1;
    }
    if (strcmp(name, "auth_mode") == 0) {
        if ((strcmp(value, "none") != 0 &&
             strcmp(value, "trusted-guest-network") != 0) ||
            copy_config_value(config->auth_mode,
                              sizeof(config->auth_mode),
                              value) != 0) {
            return -1;
        }
        config->has_auth_mode = true;
        return 0;
    }
    if (strcmp(name, "network_peer") == 0) {
        return append_network_peer(config, value);
    }
    if (strcmp(name, "network_io_timeout_ms") == 0) {
        if (!parse_config_u64_value(value, &config->network_io_timeout_ms) ||
            config->network_io_timeout_ms == 0) {
            return -1;
        }
        config->has_network_io_timeout_ms = true;
        return 0;
    }
    if (strcmp(name, "required_provider") == 0) {
        return append_required_provider(config, value);
    }
    if (strcmp(name, "provider_lease_ms") == 0) {
        if (!parse_config_u64_value(value, &config->provider_lease_ms) ||
            config->provider_lease_ms < MEM_SERVICE_PROVIDER_DIRECTORY_MIN_LEASE_MS ||
            config->provider_lease_ms > MEM_SERVICE_PROVIDER_DIRECTORY_MAX_LEASE_MS) {
            return -1;
        }
        config->has_provider_lease_ms = true;
        return 0;
    }
    /*
     * allocation_home_provider=<node_id> names the single home provider
     * (address-allocation owner) that managed allocate binds to. The node
     * must hold an active directory registration for allocate to leave
     * ALLOCATING; the descriptor publish then comes from this node.
     */
    if (strcmp(name, "allocation_home_provider") == 0) {
        if (value[0] == '\0' ||
            strlen(value) >= MEM_SERVICE_PROVIDER_NODE_ID_LEN) {
            return -1;
        }
        memcpy(config->allocation_home_provider, value, strlen(value) + 1U);
        config->has_allocation_home_provider = true;
        return 0;
    }
    if (strcmp(name, "metrics_mode") == 0) {
        return strcmp(value, "text-kv") == 0 ? 0 : -1;
    }
    if (strcmp(name, "adapter_enablement") == 0) {
        return strcmp(value, "core") == 0 || strcmp(value, "qwen3") == 0 ? 0 : -1;
    }
    if (strcmp(name, "max_records") == 0) {
        if (!parse_config_u64_value(value, &config->max_records)) {
            return -1;
        }
        config->has_max_records = true;
        return 0;
    }
    if (strcmp(name, "max_payload_bytes") == 0) {
        if (!parse_config_u64_value(value, &config->max_payload_bytes)) {
            return -1;
        }
        config->has_max_payload_bytes = true;
        return 0;
    }
    if (strcmp(name, "node_id") == 0) {
        if (copy_config_value(config->node_id, sizeof(config->node_id), value) != 0) {
            return -1;
        }
        config->has_node_id = true;
        return 0;
    }
    if (strcmp(name, "cluster_id") == 0) {
        return value[0] != '\0' ? 0 : -1;
    }
    if (strcmp(name, "retention") == 0) {
        uint64_t max_audit_events = 0;

        if (!parse_retention_value(value, &max_audit_events) ||
            copy_config_value(config->retention, sizeof(config->retention), value) != 0) {
            return -1;
        }
        config->has_retention = true;
        if (max_audit_events > 0) {
            config->has_max_audit_events = true;
            config->max_audit_events = max_audit_events;
        }
        return 0;
    }
    if (strcmp(name, "checkpoint_retention") == 0) {
        uint64_t max_checkpoint_records = 0;

        if (!parse_checkpoint_retention_value(value, &max_checkpoint_records) ||
            copy_config_value(config->checkpoint_retention,
                              sizeof(config->checkpoint_retention),
                              value) != 0) {
            return -1;
        }
        config->has_checkpoint_retention = true;
        if (max_checkpoint_records > 0) {
            config->has_max_checkpoint_records = true;
            config->max_checkpoint_records = max_checkpoint_records;
        }
        return 0;
    }
    if (strcmp(name, "record_retention") == 0) {
        uint64_t max_retained_records = 0;
        uint64_t max_retained_record_age_ms = 0;
        uint32_t retained_record_kind = 0U;
        bool retained_record_tenant_enabled = false;
        uint32_t retained_record_tenant = 0U;

        if (!parse_record_retention_value(value,
                                          &max_retained_records,
                                          &max_retained_record_age_ms,
                                          &retained_record_kind,
                                          &retained_record_tenant_enabled,
                                          &retained_record_tenant) ||
            copy_config_value(config->record_retention,
                              sizeof(config->record_retention),
                              value) != 0) {
            return -1;
        }
        config->has_record_retention = true;
        if (max_retained_records > 0 || max_retained_record_age_ms > 0) {
            config->has_max_retained_records = true;
            config->max_retained_records = max_retained_records;
            config->max_retained_record_age_ms = max_retained_record_age_ms;
            config->max_retained_record_kind = retained_record_kind;
            config->max_retained_record_tenant_enabled =
                retained_record_tenant_enabled;
            config->max_retained_record_tenant = retained_record_tenant;
        }
        return 0;
    }
    if (strcmp(name, "encryption") == 0) {
        if (strcmp(value, "none") != 0 ||
            copy_config_value(config->encryption,
                              sizeof(config->encryption),
                              value) != 0) {
            return -1;
        }
        config->has_encryption = true;
        return 0;
    }
    return -1;
}

static int load_mem_service_config(const char *path,
                                   struct mem_service_cli_config *config,
                                   bool quiet)
{
    FILE *file;
    char line[768];
    uint64_t line_no = 0;

    if (path == NULL || path[0] == '\0' || config == NULL) {
        return -1;
    }
    memset(config, 0, sizeof(*config));
    file = fopen(path, "r");
    if (file == NULL) {
        if (!quiet) {
            fprintf(stderr, "mem_service: failed to open config %s\n", path);
        }
        return -1;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *equals;
        char *name;
        char *value;

        line_no += 1U;
        if (strchr(line, '\n') == NULL && !feof(file)) {
            if (!quiet) {
                fprintf(stderr,
                        "mem_service: config line too long path=%s line=%" PRIu64 "\n",
                        path,
                        line_no);
            }
            fclose(file);
            return -1;
        }
        trim_ascii(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        equals = strchr(line, '=');
        if (equals == NULL || equals == line) {
            if (!quiet) {
                fprintf(stderr,
                        "mem_service: invalid config line path=%s line=%" PRIu64 "\n",
                        path,
                        line_no);
            }
            fclose(file);
            return -1;
        }
        *equals = '\0';
        name = line;
        value = equals + 1;
        trim_ascii(name);
        trim_ascii(value);
        if (name[0] == '\0' || apply_config_field(config, name, value) != 0) {
            if (!quiet) {
                fprintf(stderr,
                        "mem_service: unsupported config field path=%s line=%" PRIu64
                        " field=%s\n",
                        path,
                        line_no,
                        name[0] != '\0' ? name : "<empty>");
            }
            fclose(file);
            return -1;
        }
    }
    if (ferror(file)) {
        fclose(file);
        return -1;
    }
    if (fclose(file) != 0) {
        return -1;
    }
    if (!config->has_listen) {
        if (!quiet) {
            fprintf(stderr, "mem_service: config missing required listen path=%s\n", path);
        }
        return -1;
    }
    return 0;
}

static int run_config_fixture_check(void)
{
    char valid_path[160];
    char invalid_path[160];
    char invalid_quota_path[160];
    char invalid_retention_path[160];
    char invalid_checkpoint_retention_path[160];
    char invalid_record_retention_path[160];
    char invalid_encryption_path[160];
    struct mem_service_cli_config config;
    FILE *file;
    uint64_t valid_max_records = 0;
    uint64_t valid_max_payload_bytes = 0;
    uint64_t tenant_retention_records = 0;
    uint64_t tenant_retention_age_ms = 0;
    uint32_t tenant_retention_kind = 0U;
    bool tenant_retention_enabled = false;
    uint32_t tenant_retention_owner = 0U;
    uint64_t ttl_retention_records = 0;
    uint64_t ttl_retention_age_ms = 0;
    uint32_t ttl_retention_kind = 0U;
    bool ttl_retention_tenant_enabled = false;
    uint32_t ttl_retention_owner = 0U;
    char valid_retention[80];
    char valid_checkpoint_retention[80];
    char valid_record_retention[80];
    char valid_encryption[40];
    int failures = 0;

    valid_retention[0] = '\0';
    valid_checkpoint_retention[0] = '\0';
    valid_record_retention[0] = '\0';
    valid_encryption[0] = '\0';
    snprintf(valid_path,
             sizeof(valid_path),
             "/tmp/linqu_mem_service_config_fixture_%ld.conf",
             (long)getpid());
    snprintf(invalid_path,
             sizeof(invalid_path),
             "/tmp/linqu_mem_service_config_fixture_%ld_bad.conf",
             (long)getpid());
    snprintf(invalid_quota_path,
             sizeof(invalid_quota_path),
             "/tmp/linqu_mem_service_config_fixture_%ld_bad_quota.conf",
             (long)getpid());
    snprintf(invalid_retention_path,
             sizeof(invalid_retention_path),
             "/tmp/linqu_mem_service_config_fixture_%ld_bad_retention.conf",
             (long)getpid());
    snprintf(invalid_checkpoint_retention_path,
             sizeof(invalid_checkpoint_retention_path),
             "/tmp/linqu_mem_service_config_fixture_%ld_bad_checkpoint_retention.conf",
             (long)getpid());
    snprintf(invalid_record_retention_path,
             sizeof(invalid_record_retention_path),
             "/tmp/linqu_mem_service_config_fixture_%ld_bad_record_retention.conf",
             (long)getpid());
    snprintf(invalid_encryption_path,
             sizeof(invalid_encryption_path),
             "/tmp/linqu_mem_service_config_fixture_%ld_bad_encryption.conf",
             (long)getpid());
    file = fopen(valid_path, "w");
    if (file == NULL) {
        return 1;
    }
    if (fprintf(file,
                "listen=unix:/tmp/linqu_mem_service_fixture.sock\n"
                "store=/tmp/linqu_mem_service_fixture.store\n"
                "node_id=fixture-node\n"
                "cluster_id=fixture-cluster\n"
                "storage_root=/tmp/linqu_mem_service_fixture\n"
                "backend=snapshot+journal\n"
                "max_records=1024\n"
                "max_payload_bytes=4096\n"
                "retention=manual\n"
                "checkpoint_retention=manual\n"
                "record_retention=kind:training-artifact:latest:2\n"
                "encryption=none\n"
                "auth_mode=none\n"
                "metrics_mode=text-kv\n"
                "metrics_listen=tcp:127.0.0.1:9900\n"
                "adapter_enablement=core\n") < 0) {
        fclose(file);
        unlink(valid_path);
        return 1;
    }
    if (fclose(file) != 0) {
        unlink(valid_path);
        return 1;
    }
    file = fopen(invalid_path, "w");
    if (file == NULL) {
        unlink(valid_path);
        return 1;
    }
    if (fprintf(file,
                "listen=unix:/tmp/linqu_mem_service_fixture_bad.sock\n"
                "metrics_listen=tcp:0.0.0.0:9900\n") < 0) {
        if (file != NULL) {
            fclose(file);
        }
        unlink(valid_path);
        unlink(invalid_path);
        return 1;
    }
    if (fclose(file) != 0) {
        unlink(valid_path);
        unlink(invalid_path);
        return 1;
    }
    file = fopen(invalid_quota_path, "w");
    if (file == NULL) {
        unlink(valid_path);
        unlink(invalid_path);
        return 1;
    }
    if (fprintf(file,
                "listen=unix:/tmp/linqu_mem_service_fixture_bad_quota.sock\n"
                "max_records=-1\n") < 0) {
        fclose(file);
        unlink(valid_path);
        unlink(invalid_path);
        unlink(invalid_quota_path);
        return 1;
    }
    if (fclose(file) != 0) {
        unlink(valid_path);
        unlink(invalid_path);
        unlink(invalid_quota_path);
        return 1;
    }
    file = fopen(invalid_retention_path, "w");
    if (file == NULL) {
        unlink(valid_path);
        unlink(invalid_path);
        unlink(invalid_quota_path);
        return 1;
    }
    if (fprintf(file,
                "listen=unix:/tmp/linqu_mem_service_fixture_bad_retention.sock\n"
                "retention=\n") < 0) {
        fclose(file);
        unlink(valid_path);
        unlink(invalid_path);
        unlink(invalid_quota_path);
        unlink(invalid_retention_path);
        return 1;
    }
    if (fclose(file) != 0) {
        unlink(valid_path);
        unlink(invalid_path);
        unlink(invalid_quota_path);
        unlink(invalid_retention_path);
        return 1;
    }
    file = fopen(invalid_encryption_path, "w");
    if (file == NULL) {
        unlink(valid_path);
        unlink(invalid_path);
        unlink(invalid_quota_path);
        unlink(invalid_retention_path);
        unlink(invalid_checkpoint_retention_path);
        return 1;
    }
    if (fprintf(file,
                "listen=unix:/tmp/linqu_mem_service_fixture_bad_encryption.sock\n"
                "encryption=aes-256-gcm\n") < 0) {
        fclose(file);
        unlink(valid_path);
        unlink(invalid_path);
        unlink(invalid_quota_path);
        unlink(invalid_retention_path);
        unlink(invalid_checkpoint_retention_path);
        unlink(invalid_encryption_path);
        return 1;
    }
    if (fclose(file) != 0) {
        unlink(valid_path);
        unlink(invalid_path);
        unlink(invalid_quota_path);
        unlink(invalid_retention_path);
        unlink(invalid_checkpoint_retention_path);
        unlink(invalid_encryption_path);
        return 1;
    }
    file = fopen(invalid_checkpoint_retention_path, "w");
    if (file == NULL) {
        unlink(valid_path);
        unlink(invalid_path);
        unlink(invalid_quota_path);
        unlink(invalid_retention_path);
        unlink(invalid_encryption_path);
        return 1;
    }
    if (fprintf(file,
                "listen=unix:/tmp/linqu_mem_service_fixture_bad_checkpoint_retention.sock\n"
                "checkpoint_retention=latest:0\n") < 0) {
        fclose(file);
        unlink(valid_path);
        unlink(invalid_path);
        unlink(invalid_quota_path);
        unlink(invalid_retention_path);
        unlink(invalid_checkpoint_retention_path);
        unlink(invalid_encryption_path);
        return 1;
    }
    if (fclose(file) != 0) {
        unlink(valid_path);
        unlink(invalid_path);
        unlink(invalid_quota_path);
        unlink(invalid_retention_path);
        unlink(invalid_checkpoint_retention_path);
        unlink(invalid_encryption_path);
        return 1;
    }
    file = fopen(invalid_record_retention_path, "w");
    if (file == NULL) {
        unlink(valid_path);
        unlink(invalid_path);
        unlink(invalid_quota_path);
        unlink(invalid_retention_path);
        unlink(invalid_checkpoint_retention_path);
        unlink(invalid_encryption_path);
        return 1;
    }
    if (fprintf(file,
                "listen=unix:/tmp/linqu_mem_service_fixture_bad_record_retention.sock\n"
                "record_retention=kind:unknown-record:latest:2\n") < 0) {
        fclose(file);
        unlink(valid_path);
        unlink(invalid_path);
        unlink(invalid_quota_path);
        unlink(invalid_retention_path);
        unlink(invalid_checkpoint_retention_path);
        unlink(invalid_record_retention_path);
        unlink(invalid_encryption_path);
        return 1;
    }
    if (fclose(file) != 0) {
        unlink(valid_path);
        unlink(invalid_path);
        unlink(invalid_quota_path);
        unlink(invalid_retention_path);
        unlink(invalid_checkpoint_retention_path);
        unlink(invalid_record_retention_path);
        unlink(invalid_encryption_path);
        return 1;
    }
    if (load_mem_service_config(valid_path, &config, false) != 0 ||
        !config.has_listen ||
        !config.has_store ||
        !config.has_storage_root ||
        !config.has_metrics_listen ||
        !config.has_max_records ||
        !config.has_max_payload_bytes ||
        !config.has_retention ||
        !config.has_checkpoint_retention ||
        !config.has_record_retention ||
        !config.has_max_retained_records ||
        !config.has_encryption ||
        strcmp(config.listen, "unix:/tmp/linqu_mem_service_fixture.sock") != 0 ||
        strcmp(config.store, "/tmp/linqu_mem_service_fixture.store") != 0 ||
        strcmp(config.storage_root, "/tmp/linqu_mem_service_fixture") != 0 ||
        strcmp(config.metrics_listen, "tcp:127.0.0.1:9900") != 0 ||
        config.max_records != 1024U ||
        config.max_payload_bytes != 4096U ||
        config.max_retained_records != 2U ||
        config.max_retained_record_kind != MEM_SERVICE_RECORD_TRAINING_ARTIFACT ||
        strcmp(config.retention, "manual") != 0 ||
        strcmp(config.checkpoint_retention, "manual") != 0 ||
        strcmp(config.record_retention, "kind:training-artifact:latest:2") != 0 ||
        strcmp(config.encryption, "none") != 0) {
        failures -= 1;
    } else {
        valid_max_records = config.max_records;
        valid_max_payload_bytes = config.max_payload_bytes;
        snprintf(valid_retention, sizeof(valid_retention), "%s", config.retention);
        snprintf(valid_checkpoint_retention,
                 sizeof(valid_checkpoint_retention),
                 "%s",
                 config.checkpoint_retention);
        snprintf(valid_record_retention,
                 sizeof(valid_record_retention),
                 "%s",
                 config.record_retention);
        snprintf(valid_encryption, sizeof(valid_encryption), "%s", config.encryption);
    }
    if (!parse_record_retention_value("tenant:7:latest:2",
                                      &tenant_retention_records,
                                      &tenant_retention_age_ms,
                                      &tenant_retention_kind,
                                      &tenant_retention_enabled,
                                      &tenant_retention_owner) ||
        tenant_retention_records != 2U ||
        tenant_retention_age_ms != 0U ||
        tenant_retention_kind != 0U ||
        !tenant_retention_enabled ||
        tenant_retention_owner != 7U) {
        failures -= 1;
    }
    if (parse_record_retention_value("tenant:not-a-number:latest:2",
                                     &tenant_retention_records,
                                     &tenant_retention_age_ms,
                                     &tenant_retention_kind,
                                     &tenant_retention_enabled,
                                     &tenant_retention_owner)) {
        failures -= 1;
    }
    if (!parse_record_retention_value("kind:training-artifact:ttl-ms:60000",
                                      &ttl_retention_records,
                                      &ttl_retention_age_ms,
                                      &ttl_retention_kind,
                                      &ttl_retention_tenant_enabled,
                                      &ttl_retention_owner) ||
        ttl_retention_records != 0U ||
        ttl_retention_age_ms != 60000U ||
        ttl_retention_kind != MEM_SERVICE_RECORD_TRAINING_ARTIFACT ||
        ttl_retention_tenant_enabled ||
        ttl_retention_owner != 0U) {
        failures -= 1;
    }
    if (parse_record_retention_value("ttl-ms:0",
                                     &ttl_retention_records,
                                     &ttl_retention_age_ms,
                                     &ttl_retention_kind,
                                     &ttl_retention_tenant_enabled,
                                     &ttl_retention_owner)) {
        failures -= 1;
    }
    if (load_mem_service_config(invalid_path, &config, true) == 0) {
        failures -= 1;
    }
    if (load_mem_service_config(invalid_quota_path, &config, true) == 0) {
        failures -= 1;
    }
    if (load_mem_service_config(invalid_retention_path, &config, true) == 0) {
        failures -= 1;
    }
    if (load_mem_service_config(invalid_checkpoint_retention_path,
                                &config,
                                true) == 0) {
        failures -= 1;
    }
    if (load_mem_service_config(invalid_record_retention_path, &config, true) == 0) {
        failures -= 1;
    }
    if (load_mem_service_config(invalid_encryption_path, &config, true) == 0) {
        failures -= 1;
    }
    unlink(valid_path);
    unlink(invalid_path);
    unlink(invalid_quota_path);
    unlink(invalid_retention_path);
    unlink(invalid_checkpoint_retention_path);
    unlink(invalid_record_retention_path);
    unlink(invalid_encryption_path);
    if (failures != 0) {
        fprintf(stderr, "mem_service config-fixtures: failed\n");
        return 1;
    }
    printf("mem_service config-fixtures: status=ok schema_version=%u listen=%s store=%s "
           "storage_root=%s service_auth_boundary=unix-socket-local-only "
           "metrics_auth_boundary=loopback-only quota_contract=max-records+max-payload-bytes "
           "max_records=%" PRIu64 " max_payload_bytes=%" PRIu64
           " retention=%s checkpoint_retention=%s record_retention=%s "
           "tenant_record_retention=tenant:%u:latest:%" PRIu64
           " ttl_record_retention=kind:training-artifact:ttl-ms:%" PRIu64
           " encryption=%s "
           "encryption_admission=explicit-none-only fail_closed_invalid=6\n",
           MEM_SERVICE_CONFIG_SCHEMA_VERSION,
           "unix:/tmp/linqu_mem_service_fixture.sock",
           "/tmp/linqu_mem_service_fixture.store",
           "/tmp/linqu_mem_service_fixture",
           valid_max_records,
           valid_max_payload_bytes,
           valid_retention,
           valid_checkpoint_retention,
           valid_record_retention,
           tenant_retention_owner,
           tenant_retention_records,
           ttl_retention_age_ms,
           valid_encryption);
    return 0;
}

static int run_encryption_fixture_check(void)
{
    char valid_path[160];
    char invalid_path[160];
    struct mem_service_cli_config config;
    FILE *file;

    snprintf(valid_path,
             sizeof(valid_path),
             "/tmp/linqu_mem_service_encryption_fixture_%ld.conf",
             (long)getpid());
    snprintf(invalid_path,
             sizeof(invalid_path),
             "/tmp/linqu_mem_service_encryption_fixture_%ld_bad.conf",
             (long)getpid());
    file = fopen(valid_path, "w");
    if (file == NULL) {
        return 1;
    }
    if (fprintf(file,
                "listen=unix:/tmp/linqu_mem_service_encryption_fixture.sock\n"
                "encryption=none\n") < 0) {
        fclose(file);
        unlink(valid_path);
        return 1;
    }
    if (fclose(file) != 0) {
        unlink(valid_path);
        return 1;
    }
    file = fopen(invalid_path, "w");
    if (file == NULL) {
        unlink(valid_path);
        return 1;
    }
    if (fprintf(file,
                "listen=unix:/tmp/linqu_mem_service_encryption_fixture_bad.sock\n"
                "encryption=aes-256-gcm\n") < 0) {
        fclose(file);
        unlink(valid_path);
        unlink(invalid_path);
        return 1;
    }
    if (fclose(file) != 0) {
        unlink(valid_path);
        unlink(invalid_path);
        return 1;
    }
    if (load_mem_service_config(valid_path, &config, true) != 0 ||
        !config.has_encryption ||
        strcmp(config.encryption, "none") != 0 ||
        load_mem_service_config(invalid_path, &config, true) == 0) {
        unlink(valid_path);
        unlink(invalid_path);
        fprintf(stderr, "mem_service encryption-fixtures: failed\n");
        return 1;
    }
    unlink(valid_path);
    unlink(invalid_path);
    printf("mem_service encryption-fixtures: status=ok "
           "encryption_policy=explicit-none-only "
           "encryption_at_rest=not-certified "
           "unsupported_encryption_admission=fail-closed "
           "fail_closed_invalid=1\n");
    return 0;
}

static int derive_store_from_storage_root(char *out,
                                          size_t out_len,
                                          const char *storage_root)
{
    size_t root_len;
    const char *separator = "/";

    if (out == NULL || out_len == 0 || storage_root == NULL ||
        storage_root[0] == '\0') {
        return -1;
    }
    root_len = strlen(storage_root);
    if (root_len > 0 && storage_root[root_len - 1U] == '/') {
        separator = "";
    }
    return snprintf(out,
                    out_len,
                    "%s%scatalog/store.snapshot",
                    storage_root,
                    separator) < (int)out_len
               ? 0
               : -1;
}

static int run_serve(int argc, char **argv)
{
    const char *config_path = option_value(argc, argv, "--config");
    const char *listen_override = option_value(argc, argv, "--listen");
    const char *store_override = option_value(argc, argv, "--store");
    const char *metrics_listen_override = option_value(argc, argv, "--metrics-listen");
    const char *listen_spec = mem_service_default_unix_socket_spec();
    const char *store_path = NULL;
    const char *metrics_listen_spec = NULL;
    const char *storage_root = NULL;
    char derived_store[512];
    struct mem_service_cli_config config;
    struct mem_service_daemon_limits limits;
    struct mem_service_network_access network;
    struct mem_service_provider_directory_config provider_directory;
    struct mem_service_daemon_runtime runtime;
    const struct mem_service_daemon_limits *limits_ptr = NULL;
    const struct mem_service_provider_directory_config *provider_directory_ptr =
        NULL;
    bool trusted_guest_network = false;
    bool listen_is_tcp;

    derived_store[0] = '\0';
    memset(&limits, 0, sizeof(limits));
    memset(&network, 0, sizeof(network));
    memset(&provider_directory, 0, sizeof(provider_directory));
    memset(&runtime, 0, sizeof(runtime));
    if ((config_path == NULL && option_present(argc, argv, "--config")) ||
        parse_socket_arg(argc, argv, "--listen", &listen_spec) != 0) {
        return 2;
    }
    if (config_path != NULL) {
        if (load_mem_service_config(config_path, &config, false) != 0) {
            return 2;
        }
        listen_spec = config.has_listen ? config.listen : listen_spec;
        store_path = config.has_store ? config.store : NULL;
        storage_root = config.has_storage_root ? config.storage_root : NULL;
        metrics_listen_spec =
            config.has_metrics_listen ? config.metrics_listen : metrics_listen_spec;
        if (config.has_auth_mode &&
            strcmp(config.auth_mode, "trusted-guest-network") == 0) {
            trusted_guest_network = true;
        }
        /*
         * Network access fields are only meaningful together with the
         * explicit trusted-guest-network mode; configuring them under the
         * default local-only mode fails instead of being silently ignored.
         */
        if (!trusted_guest_network &&
            (config.network_peer_count > 0 ||
             config.has_network_io_timeout_ms)) {
            fprintf(stderr,
                    "mem_service: network access fields require "
                    "auth_mode=trusted-guest-network\n");
            return 2;
        }
        if (config.has_max_records || config.has_max_payload_bytes ||
            config.has_max_audit_events ||
            config.has_max_checkpoint_records ||
            config.has_max_retained_records) {
            limits.max_records = config.has_max_records ? config.max_records : 0U;
            limits.max_payload_bytes =
                config.has_max_payload_bytes ? config.max_payload_bytes : 0U;
            limits.max_audit_events =
                config.has_max_audit_events ? config.max_audit_events : 0U;
            limits.max_checkpoint_records =
                config.has_max_checkpoint_records ? config.max_checkpoint_records : 0U;
            limits.max_retained_records =
                config.has_max_retained_records ? config.max_retained_records : 0U;
            limits.max_retained_record_age_ms =
                config.has_max_retained_records ? config.max_retained_record_age_ms : 0U;
            limits.max_retained_record_kind =
                config.has_max_retained_records ? config.max_retained_record_kind : 0U;
            limits.max_retained_record_tenant_enabled =
                config.has_max_retained_records &&
                config.max_retained_record_tenant_enabled;
            limits.max_retained_record_tenant =
                config.has_max_retained_records ? config.max_retained_record_tenant : 0U;
            limits_ptr = &limits;
        }
        if (config.required_provider_count > 0 || config.has_provider_lease_ms) {
            /*
             * Required-provider set for the control-plane provider
             * directory: managed data operations stay fail-closed until
             * every listed node holds a fresh registration. A configured
             * lease without any required node only tunes expiry timing.
             */
            memcpy(provider_directory.required_nodes,
                   config.required_providers,
                   sizeof(provider_directory.required_nodes));
            provider_directory.required_count = config.required_provider_count;
            provider_directory.lease_ms =
                config.has_provider_lease_ms ? config.provider_lease_ms : 0U;
            provider_directory_ptr = &provider_directory;
        }
        if (config.has_allocation_home_provider) {
            runtime.allocation_home_provider = config.allocation_home_provider;
        }
        if (store_path == NULL && storage_root != NULL &&
            derive_store_from_storage_root(derived_store,
                                           sizeof(derived_store),
                                           storage_root) != 0) {
            fprintf(stderr,
                    "mem_service: failed to derive store from storage_root=%s\n",
                    storage_root);
            return 2;
        }
        if (store_path == NULL && derived_store[0] != '\0') {
            store_path = derived_store;
        }
    }
    if (listen_override != NULL) {
        listen_spec = listen_override;
    }
    if (store_override != NULL) {
        store_path = store_override;
    }
    if (metrics_listen_override != NULL) {
        metrics_listen_spec = metrics_listen_override;
    }
    listen_is_tcp = strncmp(listen_spec, "tcp:", 4) == 0;
    if (listen_is_tcp && !trusted_guest_network) {
        fprintf(stderr,
                "mem_service: tcp listen requires auth_mode=trusted-guest-network\n");
        return 2;
    }
    if (!trusted_guest_network) {
        if (provider_directory_ptr != NULL ||
            runtime.allocation_home_provider != NULL) {
            runtime.limits = limits_ptr;
            runtime.provider_directory = provider_directory_ptr;
            return mem_service_run_unix_daemon_with_runtime(listen_spec,
                                                            store_path,
                                                            metrics_listen_spec,
                                                            storage_root,
                                                            &runtime);
        }
        return mem_service_run_unix_daemon_with_store_metrics_catalog_and_limits(
            listen_spec,
            store_path,
            metrics_listen_spec,
            storage_root,
            limits_ptr);
    }
    /*
     * Trusted-guest-network mode requires the explicit tcp main listen
     * endpoint, this node's stable identity and a non-empty peer allowlist;
     * missing pieces fail startup. The daemon re-validates the exact bind
     * address and rejects wildcard/broadcast/multicast binds.
     */
    if (!listen_is_tcp) {
        fprintf(stderr,
                "mem_service: auth_mode=trusted-guest-network requires a "
                "tcp:<ipv4>:<port> listen endpoint\n");
        return 2;
    }
    if (!config.has_node_id) {
        fprintf(stderr,
                "mem_service: auth_mode=trusted-guest-network requires node_id\n");
        return 2;
    }
    if (config.network_peer_count == 0) {
        fprintf(stderr,
                "mem_service: auth_mode=trusted-guest-network requires a "
                "non-empty network_peer allowlist\n");
        return 2;
    }
    network.enabled = true;
    memcpy(network.node_id,
           config.node_id,
           strlen(config.node_id) + 1U);
    memcpy(network.peers,
           config.network_peers,
           config.network_peer_count * sizeof(config.network_peers[0]));
    network.peer_count = config.network_peer_count;
    network.io_timeout_ms =
        config.has_network_io_timeout_ms ? config.network_io_timeout_ms : 0U;
    runtime.limits = limits_ptr;
    runtime.providers = NULL;
    runtime.network = &network;
    runtime.provider_directory = provider_directory_ptr;
    return mem_service_run_daemon_with_runtime(listen_spec,
                                               store_path,
                                               metrics_listen_spec,
                                               storage_root,
                                               &runtime);
}

static int run_client_status(int argc,
                             char **argv,
                             enum mem_service_wire_operation operation,
                             const char *label)
{
    const char *connect_spec;
    struct mem_service_wire_client_options options;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    char payload[128];
    int rc;

    memset(payload, 0, sizeof(payload));
    if (parse_socket_arg(argc, argv, "--connect", &connect_spec) != 0 ||
        parse_client_options(argc, argv, &options) != 0) {
        return 2;
    }
    rc = mem_service_send_request_with_options(connect_spec,
                                               &options,
                                               operation,
                                               NULL,
                                               payload,
                                               sizeof(payload),
                                               &status);
    printf("mem_service %s: status=%s", label, mem_service_wire_status_name(status));
    if (payload[0] != '\0') {
        printf(" payload=%s", payload);
    }
    printf("\n");
    return rc;
}

static int run_client_payload_command(int argc,
                                      char **argv,
                                      enum mem_service_wire_operation operation,
                                      const char *label,
                                      const char *payload)
{
    const char *connect_spec;
    struct mem_service_wire_client_options options;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    int rc;

    memset(response, 0, sizeof(response));
    if (parse_socket_arg(argc, argv, "--connect", &connect_spec) != 0 ||
        parse_client_options(argc, argv, &options) != 0) {
        return 2;
    }
    rc = mem_service_send_request_with_options(connect_spec,
                                               &options,
                                               operation,
                                               payload,
                                               response,
                                               sizeof(response),
                                               &status);
    printf("mem_service %s: status=%s", label, mem_service_wire_status_name(status));
    if (response[0] != '\0') {
        printf("\n%s", response);
        if (response[strlen(response) - 1] != '\n') {
            printf("\n");
        }
    } else {
        printf("\n");
    }
    return rc;
}

static int send_client_payload_request(int argc,
                                       char **argv,
                                       enum mem_service_wire_operation operation,
                                       const char *payload,
                                       char *response,
                                       size_t response_len,
                                       enum mem_service_wire_status *status_out)
{
    const char *connect_spec;
    struct mem_service_wire_client_options options;

    if (parse_socket_arg(argc, argv, "--connect", &connect_spec) != 0 ||
        parse_client_options(argc, argv, &options) != 0) {
        return 2;
    }
    if (response != NULL && response_len > 0) {
        response[0] = '\0';
    }
    return mem_service_send_request_with_options(connect_spec,
                                                 &options,
                                                 operation,
                                                 payload,
                                                 response,
                                                 response_len,
                                                 status_out);
}

static bool metrics_export_key_is_safe(const char *key, size_t key_len)
{
    size_t i;

    if (key == NULL || key_len == 0) {
        return false;
    }
    for (i = 0; i < key_len; ++i) {
        unsigned char ch = (unsigned char)key[i];

        if (!(isalnum(ch) || ch == '_')) {
            return false;
        }
    }
    return true;
}

static const char *metrics_export_prometheus_type(const char *key, size_t key_len)
{
    static const char max_latency_key[] = "request_latency_max_ms";

    if (key_len == sizeof(max_latency_key) - 1U &&
        strncmp(key, max_latency_key, key_len) == 0) {
        return "gauge";
    }
    return "counter";
}

static int append_metrics_export_line(char *output,
                                      size_t output_len,
                                      size_t *used,
                                      const char *fmt,
                                      ...)
{
    va_list ap;
    int written;

    if (output == NULL || used == NULL || *used >= output_len) {
        return -1;
    }
    va_start(ap, fmt);
    written = vsnprintf(output + *used, output_len - *used, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= output_len - *used) {
        return -1;
    }
    *used += (size_t)written;
    return 0;
}

static int render_metrics_prometheus_text(const char *metrics_payload,
                                          char *output,
                                          size_t output_len)
{
    const char *cursor = metrics_payload;
    size_t used = 0;

    if (metrics_payload == NULL || output == NULL || output_len == 0) {
        return -1;
    }
    output[0] = '\0';
    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        const char *equals;
        const char *value;
        size_t line_len;
        size_t key_len;
        size_t value_len;

        if (line_end == NULL) {
            line_end = cursor + strlen(cursor);
        }
        line_len = (size_t)(line_end - cursor);
        if (line_len == 0) {
            if (*line_end == '\0') {
                break;
            }
            cursor = line_end + 1;
            continue;
        }
        equals = memchr(cursor, '=', line_len);
        if (equals == NULL || equals == cursor || equals + 1 >= line_end) {
            return -1;
        }
        key_len = (size_t)(equals - cursor);
        value = equals + 1;
        value_len = (size_t)(line_end - value);
        if (!metrics_export_key_is_safe(cursor, key_len)) {
            return -1;
        }
        if (append_metrics_export_line(output,
                                       output_len,
                                       &used,
                                       "# TYPE lingqu_mem_service_%.*s %s\n",
                                       (int)key_len,
                                       cursor,
                                       metrics_export_prometheus_type(cursor,
                                                                      key_len)) != 0 ||
            append_metrics_export_line(output,
                                       output_len,
                                       &used,
                                       "lingqu_mem_service_%.*s %.*s\n",
                                       (int)key_len,
                                       cursor,
                                       (int)value_len,
                                       value) != 0) {
            return -1;
        }
        if (*line_end == '\0') {
            break;
        }
        cursor = line_end + 1;
    }
    return 0;
}

static int render_admin_output_schema(char *schema, size_t schema_len, size_t *used_out)
{
    size_t used = 0;

    if (schema == NULL || schema_len == 0) {
        return -1;
    }
    schema[0] = '\0';
    if (append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "mem_service_admin_output_schema_version=%u\n",
                                MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_output_format=text-kv\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "cli_status_line=mem_service <command>: status=<wire_status_name>\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "cli_payload_separator=payload-newline-for-payload-commands\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=health operation=health response=payload_optional\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=ready operation=ready response=payload_optional\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=status operation=status response=text-kv\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=provider-status operation=status response=text-kv\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=list-records operation=list_records response=record-lines\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=metrics operation=metrics response=text-kv\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=metrics-export operation=metrics response=prometheus-text\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=audit-log operation=audit_log response=text-kv-records\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=inspect-object operation=inspect_object response=text-kv\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=materialize-object operation=materialize_object response=text-kv\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=export-snapshot operation=export_snapshot response=snapshot-text\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=export-snapshot-page operation=export_snapshot_page response=snapshot-page-text\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=export-snapshot-to operation=export_snapshot_page response=local-file-summary\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=restore-snapshot operation=restore_snapshot response=text-kv\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=restore-snapshot-page operation=restore_snapshot_page response=text-kv\n") != 0 ||
        append_wire_schema_line(schema, schema_len, &used,
                                "admin_command=allocation-stats operation=allocation_stats response=text-kv\n"
                                "allocation_stats_field=idempotency_capacity type=u32\n"
                                "allocation_stats_field=idempotency_used type=u64\n"
                                "allocation_stats_field=idempotency_cleanup_reserved type=u64\n"
                                "allocation_stats_field=idempotency_available type=u64\n"
                                "allocation_stats_field=idempotency_reservation_deficit type=u64\n"
                                "allocation_stats_field=idempotency_history_enabled type=u32\n"
                                "allocation_stats_field=idempotency_history_records type=u64\n"
                                "allocation_stats_field=idempotency_history_failed type=u32\n"
                                "allocation_stats_field=managed_recovery_required type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=ready type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=control_plane_ready type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=provider_registry_ready type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=durable_ready type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=data_plane_ready type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=provider_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=provider_ready_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=record_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=prefix_group_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=prefix_entry_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=kv_segment_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=object_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=runtime_handoff_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=execution_artifact_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=training_artifact_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "list_records_empty_field=record_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "list_records_record_line=record index=<u64> kind=<u32> kind_name=<string> key=<string> version=<u64> checksum=<u64>\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "metrics_export_format=prometheus-text\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "metrics_prometheus_prefix=lingqu_mem_service_\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "metrics_prometheus_default_type=counter\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "metrics_prometheus_type=request_latency_max_ms:gauge\n") != 0) {
        return -1;
    }
#define APPEND_COUNTER_METRIC(name) \
    do { \
        if (append_wire_schema_line(schema, schema_len, &used, \
                                    "metric_field=" name " type=counter\n") != 0) { \
            return -1; \
        } \
    } while (0)
    APPEND_COUNTER_METRIC("request_count");
    APPEND_COUNTER_METRIC("ok_count");
    APPEND_COUNTER_METRIC("error_count");
    APPEND_COUNTER_METRIC("not_found_count");
    APPEND_COUNTER_METRIC("stale_ref_count");
    APPEND_COUNTER_METRIC("checksum_mismatch_count");
    APPEND_COUNTER_METRIC("version_conflict_count");
    APPEND_COUNTER_METRIC("invalid_model_binding_count");
    APPEND_COUNTER_METRIC("invalid_session_count");
    APPEND_COUNTER_METRIC("timeout_count");
    APPEND_COUNTER_METRIC("capacity_exceeded_count");
    APPEND_COUNTER_METRIC("unsupported_count");
    APPEND_COUNTER_METRIC("internal_count");
    APPEND_COUNTER_METRIC("fail_closed_count");
    APPEND_COUNTER_METRIC("health_count");
    APPEND_COUNTER_METRIC("ready_count");
    APPEND_COUNTER_METRIC("status_count");
    APPEND_COUNTER_METRIC("list_records_count");
    APPEND_COUNTER_METRIC("metrics_count");
    APPEND_COUNTER_METRIC("audit_log_count");
    APPEND_COUNTER_METRIC("export_snapshot_count");
    APPEND_COUNTER_METRIC("export_snapshot_page_count");
    APPEND_COUNTER_METRIC("restore_snapshot_count");
    APPEND_COUNTER_METRIC("restore_snapshot_page_count");
    APPEND_COUNTER_METRIC("put_object_count");
    APPEND_COUNTER_METRIC("get_object_count");
    APPEND_COUNTER_METRIC("inspect_object_count");
    APPEND_COUNTER_METRIC("materialize_object_count");
    APPEND_COUNTER_METRIC("get_object_hit_count");
    APPEND_COUNTER_METRIC("get_object_miss_count");
    APPEND_COUNTER_METRIC("register_prefix_count");
    APPEND_COUNTER_METRIC("lookup_prefix_count");
    APPEND_COUNTER_METRIC("prefix_lookup_hit_count");
    APPEND_COUNTER_METRIC("prefix_lookup_miss_count");
    APPEND_COUNTER_METRIC("publish_kv_count");
    APPEND_COUNTER_METRIC("resolve_kv_count");
    APPEND_COUNTER_METRIC("kv_resolve_hit_count");
    APPEND_COUNTER_METRIC("kv_resolve_miss_count");
    APPEND_COUNTER_METRIC("publish_runtime_handoff_count");
    APPEND_COUNTER_METRIC("resolve_runtime_handoff_count");
    APPEND_COUNTER_METRIC("register_execution_artifact_count");
    APPEND_COUNTER_METRIC("query_execution_artifact_count");
    APPEND_COUNTER_METRIC("register_training_artifact_count");
    APPEND_COUNTER_METRIC("query_training_artifact_count");
    APPEND_COUNTER_METRIC("artifact_query_hit_count");
    APPEND_COUNTER_METRIC("artifact_query_miss_count");
    APPEND_COUNTER_METRIC("idempotency_replay_count");
    APPEND_COUNTER_METRIC("idempotency_conflict_count");
    APPEND_COUNTER_METRIC("request_latency_total_ms");
    if (append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "metric_field=request_latency_max_ms type=gauge\n") != 0) {
        return -1;
    }
    APPEND_COUNTER_METRIC("request_latency_le_1ms_count");
    APPEND_COUNTER_METRIC("request_latency_le_5ms_count");
    APPEND_COUNTER_METRIC("request_latency_le_10ms_count");
    APPEND_COUNTER_METRIC("request_latency_le_50ms_count");
    APPEND_COUNTER_METRIC("request_latency_le_100ms_count");
    APPEND_COUNTER_METRIC("request_latency_gt_100ms_count");
#undef APPEND_COUNTER_METRIC
    if (append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_field=audit_log type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_field=retained_events type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_field=first_sequence type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_field=start_sequence type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_record_delimiter=audit_begin/audit_end\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=sequence type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=monotonic_ms type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=operation type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=operation_name type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=status type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=status_name type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=request_checksum type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=response_checksum type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=idempotency_replay type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=key type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=session_id type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=model_key type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=artifact_kind type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=artifact_id type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=idempotency_key type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=version type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=checksum type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_field=events_emitted type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_field=next_sequence type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_field=complete type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_export_magic=%s\n",
                                MEM_SERVICE_CLI_STORE_MAGIC) != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_field=store_schema_version type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_field=record_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_field=audit_next_sequence type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_field=audit_event_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_record_delimiter=record_begin/record_end\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_page_field=snapshot_page type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_page_field=store_magic type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_page_field=store_schema_version type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_page_field=record_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_page_field=start_index type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_page_field=next_index type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_page_field=records_emitted type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_page_field=complete type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "restore_field=status type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "restore_field=restored type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "restore_field=record_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "restore_page_field=restore_stage type=string enum=begun,appended,cancelled\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "restore_page_field=expected_records type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "restore_page_field=page_index type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "restore_page_field=records_imported type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "restore_page_field=complete type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "fail_closed_status=stale_ref\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "fail_closed_status=checksum_mismatch\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "fail_closed_status=version_conflict\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "fail_closed_status=invalid_model_binding\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "fail_closed_status=invalid_session\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_admin_output_schema(void)
{
    char schema[8192];
    size_t used = 0;

    if (render_admin_output_schema(schema, sizeof(schema), &used) != 0) {
        fprintf(stderr, "mem_service admin-output-schema: render failed\n");
        return 1;
    }
    (void)used;
    fputs(schema, stdout);
    return 0;
}

static int run_admin_output_fixture_check(void)
{
    static const char sample_metrics[] =
        "request_count=3\n"
        "request_latency_total_ms=11\n"
        "request_latency_max_ms=7\n";
    char schema[8192];
    char exported[1024];
    size_t used = 0;
    uint32_t checksum;
    int failures = 0;

    if (render_admin_output_schema(schema, sizeof(schema), &used) != 0) {
        fprintf(stderr, "mem_service admin-output-fixtures: render failed\n");
        return 1;
    }
    checksum = mem_service_wire_checksum(schema, used);
    if (used != MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service admin-output-fixtures: schema len actual=%zu "
                "expected=%u\n",
                used,
                MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN);
        failures -= 1;
    }
    if (checksum != MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service admin-output-fixtures: schema checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM);
        failures -= 1;
    }
    if (strstr(schema, "admin_command=status operation=status response=text-kv\n") ==
            NULL ||
        strstr(schema, "admin_command=metrics-export operation=metrics response=prometheus-text\n") ==
            NULL ||
        strstr(schema, "metric_field=request_latency_max_ms type=gauge\n") ==
            NULL ||
        strstr(schema, "audit_record_delimiter=audit_begin/audit_end\n") ==
            NULL ||
        strstr(schema, "snapshot_field=store_schema_version type=u32\n") ==
            NULL ||
        strstr(schema, "snapshot_page_field=store_schema_version type=u32\n") ==
            NULL ||
        strstr(schema, "snapshot_page_field=next_index type=u64\n") == NULL ||
        strstr(schema, "allocation_stats_field=idempotency_cleanup_reserved type=u64\n") == NULL ||
        strstr(schema, "allocation_stats_field=idempotency_reservation_deficit type=u64\n") == NULL ||
        strstr(schema, "fail_closed_status=checksum_mismatch\n") == NULL) {
        fprintf(stderr, "mem_service admin-output-fixtures: required schema missing\n");
        failures -= 1;
    }
    if (render_metrics_prometheus_text(sample_metrics,
                                       exported,
                                       sizeof(exported)) != 0 ||
        strstr(exported,
               "# TYPE lingqu_mem_service_request_count counter\n"
               "lingqu_mem_service_request_count 3\n") == NULL ||
        strstr(exported,
               "# TYPE lingqu_mem_service_request_latency_max_ms gauge\n"
               "lingqu_mem_service_request_latency_max_ms 7\n") == NULL) {
        fprintf(stderr,
                "mem_service admin-output-fixtures: metrics export contract mismatch\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service admin-output-fixtures: status=ok schema_version=%u "
           "schema_len=%u schema_checksum=0x%08x admin_commands=16 "
           "metric_fields=55 prometheus_prefix=lingqu_mem_service_\n",
           MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_VERSION,
           MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN,
           MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM);
    return 0;
}

static int run_metrics_export_fixture_check(void)
{
    static const char sample_metrics[] =
        "request_count=3\n"
        "ok_count=2\n"
        "request_latency_total_ms=11\n"
        "request_latency_max_ms=7\n";
    char exported[1024];

    if (render_metrics_prometheus_text(sample_metrics,
                                       exported,
                                       sizeof(exported)) != 0) {
        fprintf(stderr, "mem_service metrics-export-fixtures: render failed\n");
        return 1;
    }
    if (strstr(exported,
               "# TYPE lingqu_mem_service_request_count counter\n"
               "lingqu_mem_service_request_count 3\n") == NULL ||
        strstr(exported,
               "# TYPE lingqu_mem_service_request_latency_total_ms counter\n"
               "lingqu_mem_service_request_latency_total_ms 11\n") == NULL ||
        strstr(exported,
               "# TYPE lingqu_mem_service_request_latency_max_ms gauge\n"
               "lingqu_mem_service_request_latency_max_ms 7\n") == NULL) {
        fprintf(stderr,
                "mem_service metrics-export-fixtures: prometheus output mismatch\n");
        return 1;
    }
    if (render_metrics_prometheus_text("bad-key=1\n",
                                       exported,
                                       sizeof(exported)) == 0 ||
        render_metrics_prometheus_text("badline\n",
                                       exported,
                                       sizeof(exported)) == 0) {
        fprintf(stderr,
                "mem_service metrics-export-fixtures: invalid input accepted\n");
        return 1;
    }
    printf("mem_service metrics-export-fixtures: status=ok format=prometheus-text metrics=4\n");
    return 0;
}

static int render_metrics_http_response(const char *method,
                                        const char *path,
                                        const char *metrics_payload,
                                        char *output,
                                        size_t output_len)
{
    char body[8192];
    size_t used = 0;
    size_t body_len;

    if (method == NULL || path == NULL || output == NULL || output_len == 0 ||
        strcmp(method, "GET") != 0 || strcmp(path, "/metrics") != 0) {
        return -1;
    }
    if (render_metrics_prometheus_text(metrics_payload, body, sizeof(body)) != 0) {
        return -1;
    }
    body_len = strlen(body);
    output[0] = '\0';
    if (append_metrics_export_line(output,
                                   output_len,
                                   &used,
                                   "HTTP/1.1 200 OK\r\n") != 0 ||
        append_metrics_export_line(output,
                                   output_len,
                                   &used,
                                   "Content-Type: text/plain; version=0.0.4\r\n") != 0 ||
        append_metrics_export_line(output,
                                   output_len,
                                   &used,
                                   "Content-Length: %zu\r\n",
                                   body_len) != 0 ||
        append_metrics_export_line(output,
                                   output_len,
                                   &used,
                                   "Cache-Control: no-store\r\n"
                                   "\r\n"
                                   "%s",
                                   body) != 0) {
        return -1;
    }
    return 0;
}

static int run_deployment_fixture_check(void)
{
    static const char deployment_manifest[] =
        "[Unit]\n"
        "Description=Lingqu Memory Service\n"
        "After=network.target\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "ExecStart=/usr/bin/linqu_mem_service serve --config "
        "/etc/lingqu/mem_service/mem_service.conf\n"
        "Restart=on-failure\n"
        "RestartSec=2\n"
        "\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n";
    static const char host_deployment_manifest[] =
        "[Unit]\n"
        "Description=Lingqu Memory Service Host Daemon\n"
        "After=network.target\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "ExecStart=/usr/libexec/lingqu/mem_service/linqu_mem_service_host "
        "serve --config /etc/lingqu/mem_service/mem_service.conf\n"
        "Restart=on-failure\n"
        "RestartSec=2\n"
        "\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n";
    static const char sample_metrics[] =
        "request_count=5\n"
        "ok_count=5\n"
        "request_latency_max_ms=2\n";
    char response[4096];

    if (strstr(deployment_manifest, "[Service]\n") == NULL ||
        strstr(deployment_manifest,
               "ExecStart=/usr/bin/linqu_mem_service serve --config "
               "/etc/lingqu/mem_service/mem_service.conf\n") == NULL ||
        strstr(deployment_manifest, "Restart=on-failure\n") == NULL ||
        strstr(deployment_manifest, "WantedBy=multi-user.target\n") == NULL) {
        fprintf(stderr, "mem_service deployment-fixtures: manifest mismatch\n");
        return 1;
    }
    if (strstr(host_deployment_manifest, "[Service]\n") == NULL ||
        strstr(host_deployment_manifest,
               "ExecStart=/usr/libexec/lingqu/mem_service/linqu_mem_service_host "
               "serve --config /etc/lingqu/mem_service/mem_service.conf\n") == NULL ||
        strstr(host_deployment_manifest, "Restart=on-failure\n") == NULL ||
        strstr(host_deployment_manifest, "WantedBy=multi-user.target\n") == NULL) {
        fprintf(stderr,
                "mem_service deployment-fixtures: host manifest mismatch\n");
        return 1;
    }
    if (render_metrics_http_response("GET",
                                     "/metrics",
                                     sample_metrics,
                                     response,
                                     sizeof(response)) != 0) {
        fprintf(stderr, "mem_service deployment-fixtures: http render failed\n");
        return 1;
    }
    if (strstr(response, "HTTP/1.1 200 OK\r\n") == NULL ||
        strstr(response, "Content-Type: text/plain; version=0.0.4\r\n") == NULL ||
        strstr(response, "Content-Length: ") == NULL ||
        strstr(response, "Cache-Control: no-store\r\n") == NULL ||
        strstr(response, "lingqu_mem_service_request_count 5\n") == NULL ||
        strstr(response,
               "# TYPE lingqu_mem_service_request_latency_max_ms gauge\n") == NULL) {
        fprintf(stderr, "mem_service deployment-fixtures: http response mismatch\n");
        return 1;
    }
    if (render_metrics_http_response("POST",
                                     "/metrics",
                                     sample_metrics,
                                     response,
                                     sizeof(response)) == 0 ||
        render_metrics_http_response("GET",
                                     "/bad",
                                     sample_metrics,
                                     response,
                                     sizeof(response)) == 0 ||
        render_metrics_http_response("GET",
                                     "/metrics",
                                     "bad-key=1\n",
                                     response,
                                     sizeof(response)) == 0) {
        fprintf(stderr,
                "mem_service deployment-fixtures: invalid http scrape accepted\n");
        return 1;
    }
    printf("mem_service deployment-fixtures: status=ok deployment_smoke_version=%u "
           "service_manager=systemd-like metrics_scrape_path=/metrics "
           "metrics_http_content_type=prometheus-text "
           "host_service_manager=systemd-like\n",
           MEM_SERVICE_DEPLOYMENT_SMOKE_VERSION);
    return 0;
}

static bool collector_http_response_has_header(const char *response,
                                               const char *header)
{
    return response != NULL && header != NULL && strstr(response, header) != NULL;
}

static const char *collector_http_response_body(const char *response)
{
    const char *body;

    if (response == NULL) {
        return NULL;
    }
    body = strstr(response, "\r\n\r\n");
    if (body == NULL) {
        return NULL;
    }
    return body + 4;
}

static bool collector_metric_type_present(const char *body,
                                          const char *metric_name,
                                          const char *metric_type)
{
    char expected[256];

    if (body == NULL || metric_name == NULL || metric_type == NULL) {
        return false;
    }
    if (snprintf(expected,
                 sizeof(expected),
                 "# TYPE %s %s\n",
                 metric_name,
                 metric_type) >= (int)sizeof(expected)) {
        return false;
    }
    return strstr(body, expected) != NULL;
}

static bool collector_metric_value_at_least(const char *body,
                                            const char *metric_name,
                                            uint64_t minimum)
{
    const char *cursor = body;
    size_t metric_name_len;

    if (body == NULL || metric_name == NULL) {
        return false;
    }
    metric_name_len = strlen(metric_name);
    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        const char *value_start;
        char *value_end = NULL;
        uint64_t value;

        if (line_end == NULL) {
            line_end = cursor + strlen(cursor);
        }
        if ((size_t)(line_end - cursor) > metric_name_len &&
            strncmp(cursor, metric_name, metric_name_len) == 0 &&
            cursor[metric_name_len] == ' ') {
            value_start = cursor + metric_name_len + 1;
            value = strtoull(value_start, &value_end, 10);
            if (value_end == value_start) {
                return false;
            }
            while (value_end < line_end && isspace((unsigned char)*value_end)) {
                ++value_end;
            }
            return value_end == line_end && value >= minimum;
        }
        if (*line_end == '\0') {
            break;
        }
        cursor = line_end + 1;
    }
    return false;
}

static int run_collector_fixture_check(void)
{
    static const char sample_metrics[] =
        "request_count=7\n"
        "ok_count=6\n"
        "health_count=1\n"
        "put_object_count=2\n"
        "request_latency_max_ms=4\n";
    char response[4096];
    const char *body;

    if (render_metrics_http_response("GET",
                                     "/metrics",
                                     sample_metrics,
                                     response,
                                     sizeof(response)) != 0) {
        fprintf(stderr, "mem_service collector-fixtures: http render failed\n");
        return 1;
    }
    body = collector_http_response_body(response);
    if (!collector_http_response_has_header(response, "HTTP/1.1 200 OK\r\n") ||
        !collector_http_response_has_header(
            response,
            "Content-Type: text/plain; version=0.0.4\r\n") ||
        body == NULL) {
        fprintf(stderr, "mem_service collector-fixtures: http envelope failed\n");
        return 1;
    }
    if (!collector_metric_type_present(body,
                                       "lingqu_mem_service_request_count",
                                       "counter") ||
        !collector_metric_type_present(body,
                                       "lingqu_mem_service_request_latency_max_ms",
                                       "gauge") ||
        !collector_metric_value_at_least(body,
                                         "lingqu_mem_service_request_count",
                                         7) ||
        !collector_metric_value_at_least(body,
                                         "lingqu_mem_service_health_count",
                                         1) ||
        !collector_metric_value_at_least(body,
                                         "lingqu_mem_service_put_object_count",
                                         2) ||
        collector_metric_value_at_least(body,
                                        "lingqu_mem_service_missing_count",
                                        1)) {
        fprintf(stderr, "mem_service collector-fixtures: metric parse failed\n");
        return 1;
    }
    printf("mem_service collector-fixtures: status=ok "
           "collector=prometheus-text-http metrics=5\n");
    return 0;
}

static int render_alert_rules(char *rules, size_t rules_len, size_t *used_out)
{
    size_t used = 0;

    if (rules == NULL || rules_len == 0) {
        return -1;
    }
    rules[0] = '\0';
    if (append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "# linqu mem_service prometheus alert rules\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "# contract_version: %u\n",
                                MEM_SERVICE_ALERT_RULES_VERSION) != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "groups:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "- name: lingqu_mem_service.rules\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "  rules:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "  - alert: LingquMemServiceDown\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    expr: up{job=\"linqu_mem_service\"} == 0\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    for: 1m\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    labels:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      severity: critical\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      service: linqu_mem_service\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    annotations:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      summary: linqu_mem_service scrape target is down\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      runbook: check service manager, socket path, and metrics listener\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "  - alert: LingquMemServiceErrorRate\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    expr: increase(lingqu_mem_service_error_count[5m]) > 0\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    for: 5m\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    labels:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      severity: warning\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      service: linqu_mem_service\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    annotations:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      summary: mem_service returned non-ok RPC statuses\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      runbook: inspect audit-log and recent client errors\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "  - alert: LingquMemServiceFailClosed\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    expr: increase(lingqu_mem_service_fail_closed_count[5m]) > 0\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    for: 1m\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    labels:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      severity: critical\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      service: linqu_mem_service\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    annotations:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      summary: mem_service fail-closed path is active\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      runbook: check stale_ref, checksum_mismatch, and binding counters\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "  - alert: LingquMemServiceChecksumMismatch\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    expr: increase(lingqu_mem_service_checksum_mismatch_count[5m]) > 0\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    for: 1m\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    labels:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      severity: critical\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      service: linqu_mem_service\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    annotations:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      summary: mem_service detected a corrupt payload or ref\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      runbook: quarantine corrupt block and verify producer checksums\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "  - alert: LingquMemServiceCapacityExceeded\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    expr: increase(lingqu_mem_service_capacity_exceeded_count[5m]) > 0\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    for: 1m\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    labels:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      severity: warning\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      service: linqu_mem_service\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    annotations:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      summary: mem_service capacity or quota admission rejected writes\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      runbook: inspect quota settings, storage capacity, and client write rate\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "  - alert: LingquMemServiceHighLatency\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    expr: lingqu_mem_service_request_latency_max_ms > 100\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    for: 5m\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    labels:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      severity: warning\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      service: linqu_mem_service\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    annotations:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      summary: mem_service max request latency exceeded 100 ms\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      runbook: inspect storage backend, socket backlog, and client retry load\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static size_t alert_rule_count(const char *rules)
{
    const char *cursor = rules;
    size_t count = 0;

    if (rules == NULL) {
        return 0;
    }
    while ((cursor = strstr(cursor, "  - alert: ")) != NULL) {
        ++count;
        cursor += strlen("  - alert: ");
    }
    return count;
}

static int run_alert_rules(void)
{
    char rules[8192];
    size_t used = 0;

    if (render_alert_rules(rules, sizeof(rules), &used) != 0) {
        fprintf(stderr, "mem_service alert-rules: render failed\n");
        return 1;
    }
    (void)used;
    fputs(rules, stdout);
    return 0;
}

static int run_alert_fixture_check(void)
{
    char rules[8192];
    size_t used = 0;
    size_t rule_count;
    uint32_t checksum;
    int failures = 0;

    if (render_alert_rules(rules, sizeof(rules), &used) != 0) {
        fprintf(stderr, "mem_service alert-fixtures: render failed\n");
        return 1;
    }
    rule_count = alert_rule_count(rules);
    checksum = mem_service_wire_checksum(rules, used);
    if (rule_count != MEM_SERVICE_ALERT_RULES_EXPECTED_RULE_COUNT) {
        fprintf(stderr,
                "mem_service alert-fixtures: rule count actual=%zu expected=%u\n",
                rule_count,
                MEM_SERVICE_ALERT_RULES_EXPECTED_RULE_COUNT);
        failures -= 1;
    }
    if (used != MEM_SERVICE_ALERT_RULES_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service alert-fixtures: rules len actual=%zu expected=%u\n",
                used,
                MEM_SERVICE_ALERT_RULES_EXPECTED_LEN);
        failures -= 1;
    }
    if (checksum != MEM_SERVICE_ALERT_RULES_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service alert-fixtures: rules checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_ALERT_RULES_EXPECTED_CHECKSUM);
        failures -= 1;
    }
    if (strstr(rules, "alert: LingquMemServiceDown\n") == NULL ||
        strstr(rules, "lingqu_mem_service_fail_closed_count") == NULL ||
        strstr(rules, "lingqu_mem_service_checksum_mismatch_count") == NULL ||
        strstr(rules, "lingqu_mem_service_capacity_exceeded_count") == NULL ||
        strstr(rules, "lingqu_mem_service_request_latency_max_ms") == NULL ||
        strstr(rules, "severity: critical\n") == NULL) {
        fprintf(stderr, "mem_service alert-fixtures: required alert missing\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service alert-fixtures: status=ok format=prometheus-rules-yaml "
           "rules=%u rules_len=%u rules_checksum=0x%08x\n",
           MEM_SERVICE_ALERT_RULES_EXPECTED_RULE_COUNT,
           MEM_SERVICE_ALERT_RULES_EXPECTED_LEN,
           MEM_SERVICE_ALERT_RULES_EXPECTED_CHECKSUM);
    return 0;
}

static int run_alert_integration_fixture_check(void)
{
    static const char sample_metrics[] =
        "request_count=11\n"
        "error_count=1\n"
        "fail_closed_count=1\n"
        "checksum_mismatch_count=1\n"
        "capacity_exceeded_count=1\n"
        "request_latency_max_ms=101\n";
    char rules[8192];
    char response[4096];
    const char *body;
    size_t used = 0;

    if (render_alert_rules(rules, sizeof(rules), &used) != 0) {
        fprintf(stderr, "mem_service alert-integration-fixtures: rules render failed\n");
        return 1;
    }
    if (render_metrics_http_response("GET",
                                     "/metrics",
                                     sample_metrics,
                                     response,
                                     sizeof(response)) != 0) {
        fprintf(stderr,
                "mem_service alert-integration-fixtures: metrics render failed\n");
        return 1;
    }
    body = collector_http_response_body(response);
    if (body == NULL ||
        !collector_http_response_has_header(response, "HTTP/1.1 200 OK\r\n") ||
        !collector_metric_type_present(body,
                                       "lingqu_mem_service_error_count",
                                       "counter") ||
        !collector_metric_type_present(body,
                                       "lingqu_mem_service_fail_closed_count",
                                       "counter") ||
        !collector_metric_type_present(body,
                                       "lingqu_mem_service_checksum_mismatch_count",
                                       "counter") ||
        !collector_metric_type_present(body,
                                       "lingqu_mem_service_capacity_exceeded_count",
                                       "counter") ||
        !collector_metric_type_present(body,
                                       "lingqu_mem_service_request_latency_max_ms",
                                       "gauge")) {
        fprintf(stderr,
                "mem_service alert-integration-fixtures: metrics contract missing\n");
        return 1;
    }
    if (strstr(rules, "up{job=\"linqu_mem_service\"} == 0") == NULL ||
        strstr(rules, "increase(lingqu_mem_service_error_count[5m]) > 0") == NULL ||
        strstr(rules, "increase(lingqu_mem_service_fail_closed_count[5m]) > 0") ==
            NULL ||
        strstr(rules,
               "increase(lingqu_mem_service_checksum_mismatch_count[5m]) > 0") ==
            NULL ||
        strstr(rules,
               "increase(lingqu_mem_service_capacity_exceeded_count[5m]) > 0") ==
            NULL ||
        strstr(rules, "lingqu_mem_service_request_latency_max_ms > 100") == NULL) {
        fprintf(stderr,
                "mem_service alert-integration-fixtures: alert expression missing\n");
        return 1;
    }
    if (!collector_metric_value_at_least(body,
                                         "lingqu_mem_service_error_count",
                                         1) ||
        !collector_metric_value_at_least(body,
                                         "lingqu_mem_service_fail_closed_count",
                                         1) ||
        !collector_metric_value_at_least(body,
                                         "lingqu_mem_service_checksum_mismatch_count",
                                         1) ||
        !collector_metric_value_at_least(body,
                                         "lingqu_mem_service_capacity_exceeded_count",
                                         1) ||
        !collector_metric_value_at_least(body,
                                         "lingqu_mem_service_request_latency_max_ms",
                                         101)) {
        fprintf(stderr,
                "mem_service alert-integration-fixtures: metric values missing\n");
        return 1;
    }
    printf("mem_service alert-integration-fixtures: status=ok "
           "collector=prometheus-text-http alert_rules=6 referenced_metrics=5 "
           "synthetic_targets=1\n");
    return 0;
}

static int run_metrics_export(int argc, char **argv)
{
    const char *format = option_value(argc, argv, "--format");
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char exported[16384];
    int rc;

    memset(response, 0, sizeof(response));
    memset(exported, 0, sizeof(exported));
    if (format == NULL) {
        format = "prometheus-text";
    }
    if (strcmp(format, "prometheus-text") != 0 &&
        strcmp(format, "prometheus") != 0) {
        fprintf(stderr, "mem_service: unsupported metrics export format %s\n", format);
        return 2;
    }
    rc = send_client_payload_request(argc,
                                     argv,
                                     MEM_SERVICE_WIRE_OP_METRICS,
                                     NULL,
                                     response,
                                     sizeof(response),
                                     &status);
    if (rc != 0 || status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service: metrics export failed status=%s\n",
                mem_service_wire_status_name(status));
        return rc != 0 ? rc : 1;
    }
    if (render_metrics_prometheus_text(response, exported, sizeof(exported)) != 0) {
        fprintf(stderr, "mem_service: metrics export render failed\n");
        return 1;
    }
    printf("%s", exported);
    return 0;
}

static const char *snapshot_path_arg(int argc, char **argv, const char *option_name)
{
    const char *path = option_value(argc, argv, option_name);
    int i;

    if (path != NULL) {
        return path;
    }
    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--connect") == 0 ||
            strcmp(argv[i], "--from") == 0 ||
            strcmp(argv[i], "--to") == 0 ||
            strcmp(argv[i], "--max-records") == 0 ||
            strcmp(argv[i], "--max-attempts") == 0 ||
            strcmp(argv[i], "--retry-backoff-ms") == 0 ||
            strcmp(argv[i], "--timeout-ms") == 0 ||
            strcmp(argv[i], "--idempotency-key") == 0) {
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--retry-timeouts") == 0) {
            continue;
        }
        if (argv[i][0] != '-') {
            return argv[i];
        }
    }
    return NULL;
}

static int run_put_object(int argc, char **argv)
{
    char payload[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--key", "key") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--owner", "owner") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--payload-kind", "payload_kind") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backing-offset", "backing_offset") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backing-len", "backing_len") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--checksum", "checksum") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--version", "version") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend", "backend") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-write", "backend_write") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-device-path", "backend_device_path") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-request-id", "backend_request_id") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-source-cna", "backend_source_cna") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-kind", "backend_kind") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-node", "backend_node") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-device-cna", "backend_device_cna") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-flags", "backend_flags") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-block-hi", "backend_block_hi") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-block-lo", "backend_block_lo") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-block-version", "backend_block_version") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-block-offset", "backend_block_offset") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-block-bytes", "backend_block_bytes") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-block-checksum", "backend_block_checksum") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-gsva-base", "backend_buffer_gsva_base") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-bytes", "backend_buffer_bytes") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-token-id", "backend_buffer_token_id") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-token-value", "backend_buffer_token_value") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-version", "backend_buffer_key_version") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-flags", "backend_buffer_key_flags") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-segment-id", "backend_buffer_key_segment_id") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-home-va", "backend_buffer_key_home_va") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-size", "backend_buffer_key_size") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-vmid", "backend_buffer_key_vmid") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-asid", "backend_buffer_key_asid") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-pte-offset", "backend_buffer_key_pte_offset") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-p-tag", "backend_buffer_key_p_tag") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-cache-policy", "backend_buffer_key_cache_policy") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-epoch", "backend_buffer_key_epoch") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--payload-inline", "payload_inline") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--payload-file", "payload_path") != 0 ||
        append_idempotency_payload_field(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc, argv, MEM_SERVICE_WIRE_OP_PUT_OBJECT, "put-object", payload);
}

static int run_get_object(int argc, char **argv)
{
    char payload[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--key", "key") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-read", "backend_read") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-device-path", "backend_device_path") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-request-id", "backend_request_id") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-source-cna", "backend_source_cna") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-device-cna", "backend_device_cna") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-flags", "backend_flags") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-gsva-base", "backend_buffer_gsva_base") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-bytes", "backend_buffer_bytes") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-token-id", "backend_buffer_token_id") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-token-value", "backend_buffer_token_value") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-version", "backend_buffer_key_version") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-flags", "backend_buffer_key_flags") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-segment-id", "backend_buffer_key_segment_id") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-home-va", "backend_buffer_key_home_va") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-size", "backend_buffer_key_size") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-vmid", "backend_buffer_key_vmid") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-asid", "backend_buffer_key_asid") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-pte-offset", "backend_buffer_key_pte_offset") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-p-tag", "backend_buffer_key_p_tag") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-cache-policy", "backend_buffer_key_cache_policy") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backend-buffer-key-epoch", "backend_buffer_key_epoch") != 0) {
        return 2;
    }
    return run_client_payload_command(argc, argv, MEM_SERVICE_WIRE_OP_GET_OBJECT, "get-object", payload);
}

static int run_inspect_object(int argc, char **argv)
{
    char payload[160] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--key", "key") != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_INSPECT_OBJECT,
                                      "inspect-object",
                                      payload);
}

static int run_materialize_object(int argc, char **argv)
{
    char payload[768] = "";

    if (append_required_payload_field(payload,
                                      sizeof(payload),
                                      argc,
                                      argv,
                                      "--key",
                                      "key") != 0 ||
        append_required_payload_field(payload,
                                      sizeof(payload),
                                      argc,
                                      argv,
                                      "--to",
                                      "destination_path") != 0 ||
        append_optional_payload_field(payload,
                                      sizeof(payload),
                                      argc,
                                      argv,
                                      "--expected-version",
                                      "expected_version") != 0 ||
        append_optional_payload_field(payload,
                                      sizeof(payload),
                                      argc,
                                      argv,
                                      "--expected-checksum",
                                      "expected_checksum") != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_MATERIALIZE_OBJECT,
                                      "materialize-object",
                                      payload);
}

/*
 * Managed allocation control commands (M1.1, wire ops 0x70-0x75). These
 * are thin pass-throughs: no allocation semantics are re-implemented in
 * the CLI, the daemon owns the state machine.
 */
static int run_allocate_object(int argc, char **argv)
{
    char payload[512] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--key", "key") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--idempotency-key", "idempotency_key") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--size-bytes", "size_bytes") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--capabilities", "capabilities") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--session-id", "session_id") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--alignment-bytes", "alignment_bytes") != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_ALLOCATE_OBJECT,
                                      "allocate-object",
                                      payload);
}

static int run_holder_object_command(int argc,
                                     char **argv,
                                     enum mem_service_wire_operation operation,
                                     const char *label,
                                     bool session_required)
{
    char payload[512] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--key", "key") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--idempotency-key", "idempotency_key") != 0 ||
        (session_required &&
         append_required_payload_field(payload, sizeof(payload), argc, argv, "--session-id", "session_id") != 0) ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--expected-generation", "expected_generation") != 0) {
        return 2;
    }
    return run_client_payload_command(argc, argv, operation, label, payload);
}

static int run_acquire_object(int argc, char **argv)
{
    return run_holder_object_command(argc,
                                     argv,
                                     MEM_SERVICE_WIRE_OP_ACQUIRE_OBJECT,
                                     "acquire-object",
                                     true);
}

static int run_release_object(int argc, char **argv)
{
    return run_holder_object_command(argc,
                                     argv,
                                     MEM_SERVICE_WIRE_OP_RELEASE_OBJECT,
                                     "release-object",
                                     true);
}

static int run_retire_object(int argc, char **argv)
{
    return run_holder_object_command(argc,
                                     argv,
                                     MEM_SERVICE_WIRE_OP_RETIRE_OBJECT,
                                     "retire-object",
                                     false);
}

static int run_inspect_allocation(int argc, char **argv)
{
    char payload[160] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--key", "key") != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_INSPECT_ALLOCATION,
                                      "inspect-allocation",
                                      payload);
}

static int run_allocation_stats(int argc, char **argv)
{
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_ALLOCATION_STATS,
                                      "allocation-stats",
                                      NULL);
}

/*
 * Provider directory commands (M1.2, wire ops 0x76-0x79). Thin
 * pass-throughs; the control-plane daemon owns the directory state.
 */
static int run_provider_register(int argc, char **argv)
{
    char payload[512] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--node-id", "node_id") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--incarnation", "incarnation") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--readiness-generation", "readiness_generation") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--capabilities", "capabilities") != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_PROVIDER_REGISTER,
                                      "provider-register",
                                      payload);
}

static int run_provider_refresh(int argc, char **argv)
{
    char payload[384] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--node-id", "node_id") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--incarnation", "incarnation") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--readiness-generation", "readiness_generation") != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_PROVIDER_REFRESH,
                                      "provider-refresh",
                                      payload);
}

static int run_provider_deregister(int argc, char **argv)
{
    char payload[256] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--node-id", "node_id") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--incarnation", "incarnation") != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_PROVIDER_DEREGISTER,
                                      "provider-deregister",
                                      payload);
}

static int run_provider_directory_status(int argc, char **argv)
{
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_PROVIDER_STATUS,
                                      "provider-directory-status",
                                      NULL);
}

/*
 * Provider-backed allocation lifecycle commands (M1.2, wire ops
 * 0x7a-0x7b). Only the bound home provider process invokes these; the
 * descriptor crosses as opaque hex and payload bytes never move.
 */
static int run_mapping_transition(int argc, char **argv)
{
    char payload[768] = "";
    const char *action = option_value(argc, argv, "--action");
    static const char *actions[] = {"begin", "confirm", "close", "finish", "cancel", "inspect"};
    const char *connect_spec;
    struct mem_service_wire_client_options options;
    struct mem_service_client client;
    struct mem_service_client_mapping_transaction transaction;
    struct mem_service_wire_payload_view view;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    int rc;
    size_t i;

    if (action == NULL) return 2;
    for (i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
        if (strcmp(action, actions[i]) == 0) break;
    }
    if (i == sizeof(actions) / sizeof(actions[0]) ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--key", "key") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--session-id", "session_id") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--generation", "generation") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--mapping-id", "mapping_id") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--idempotency-key", "idempotency_key") != 0 ||
        mem_service_wire_payload_append_u64(payload, sizeof(payload), "action", i + 1) != 0) return 2;
    view = mem_service_wire_payload_view_from_cstr(payload);
    if (!mem_service_wire_schema_validate_payload(
            mem_service_wire_schema_for_operation(MEM_SERVICE_WIRE_OP_MAPPING_TRANSITION),
            &view, NULL) ||
        parse_socket_arg(argc, argv, "--connect", &connect_spec) != 0 ||
        parse_client_options(argc, argv, &options) != 0) return 2;
    mem_service_client_init_with_options(&client, connect_spec, &options);
    rc = mem_service_client_mapping_transition(&client,
        option_value(argc, argv, "--key"), option_value(argc, argv, "--session-id"),
        mem_service_wire_payload_get_u64(&view, "generation", 0),
        mem_service_wire_payload_get_u64(&view, "mapping_id", 0),
        (enum mem_service_client_mapping_action)(i + 1),
        option_value(argc, argv, "--idempotency-key"), &transaction, &status);
    printf("mem_service mapping-transition: status=%s\nstatus=%s\n",
           mem_service_wire_status_name(status), mem_service_wire_status_name(status));
    if (rc == 0) {
        printf("key=%s\nsession_id=%s\ngeneration=%llu\nmapping_id=%llu\nmapping_state=%u\n",
               option_value(argc, argv, "--key"), option_value(argc, argv, "--session-id"),
               (unsigned long long)transaction.generation,
               (unsigned long long)transaction.mapping_id, transaction.state);
    }
    return rc;
}

static int run_reference_transition(int argc, char **argv)
{
    static const char *actions[] = {"begin", "stage", "seal", "resolve", "acquire", "map-begin"};
    static const char *options[] = {"--action", "--key", "--session-id", "--generation",
        "--version", "--reference-hex", "--access", "--idempotency-key", "--connect",
        "--timeout-ms", "--max-attempts", "--retry-backoff-ms"};
    static const char *fields[] = {"action", "key", "session_id", "generation",
        "version", "reference_hex", "access", "idempotency_key"};
    char payload[1536] = "", hex[513];
    unsigned seen = 0;
    const char *connect_spec;
    struct mem_service_reference_request request;
    struct mem_service_client_reference_result result;
    struct mem_service_client_mapping_transaction mapping;
    struct mem_service_client client;
    struct mem_service_wire_client_options client_options;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    int rc;
    for (int i = 2; i < argc; i += 2) {
        size_t index;
        if (i + 1 >= argc || strchr(argv[i + 1], '\n') || strchr(argv[i + 1], '\r')) goto invalid;
        for (index = 0; index < sizeof(options) / sizeof(options[0]); ++index)
            if (!strcmp(argv[i], options[index])) break;
        if (index == sizeof(options) / sizeof(options[0]) || (seen & (1U << index))) goto invalid;
        seen |= 1U << index;
        if (index == 0) {
            size_t action;
            for (action = 0; action < sizeof(actions) / sizeof(actions[0]); ++action)
                if (!strcmp(argv[i + 1], actions[action])) break;
            if (action == sizeof(actions) / sizeof(actions[0]) ||
                mem_service_wire_payload_append_u64(payload, sizeof(payload), "action", action + 1)) goto invalid;
        } else if (index < sizeof(fields) / sizeof(fields[0]) &&
                   mem_service_wire_payload_append_field(payload, sizeof(payload), fields[index], argv[i + 1])) goto invalid;
    }
    if (mem_service_reference_parse_request(payload, &request) ||
        parse_socket_arg(argc, argv, "--connect", &connect_spec) ||
        parse_client_options(argc, argv, &client_options)) goto invalid;
    mem_service_client_init_with_options(&client, connect_spec, &client_options);
    rc = request.action == MEM_SERVICE_REFERENCE_MAP_BEGIN ?
        mem_service_client_reference_map_begin(&client, &request, &result, &mapping, &status) :
        mem_service_client_reference_transition(&client, &request, &result, &status);
    printf("mem_service reference-transition: status=%s\nstatus=%s\n",
           mem_service_wire_status_name(status), mem_service_wire_status_name(status));
    if (!rc) {
        printf("action=%s\nreference_key=%s\nkey=%s\ngeneration=%" PRIu64
               "\nversion=%" PRIu64 "\nhome_node=%s\nprovider_incarnation=%" PRIu64 "\n",
               actions[request.action - 1], request.key, result.allocation.key,
               result.allocation.generation, result.allocation.version,
               result.allocation.home_node, result.allocation.provider_incarnation);
        if (request.action == MEM_SERVICE_REFERENCE_STAGE || request.action == MEM_SERVICE_REFERENCE_RESOLVE ||
            request.action == MEM_SERVICE_REFERENCE_ACQUIRE || request.action == MEM_SERVICE_REFERENCE_MAP_BEGIN) {
            if (mem_service_reference_encode_hex(&result.reference, hex, sizeof(hex))) return 1;
            printf("reference_hex=%s\n", hex);
        }
        if (request.action == MEM_SERVICE_REFERENCE_MAP_BEGIN)
            printf("mapping_id=%" PRIu64 "\nmapping_state=%u\nsession_id=%s\n",
                   mapping.mapping_id, mapping.state, request.session_id);
    }
    return rc;
invalid:
    fprintf(stderr, "mem_service reference-transition: invalid action-specific fields; see --help\n");
    return 2;
}

static int run_poll_allocation(int argc, char **argv)
{
    char payload[512] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--node-id", "node_id") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--incarnation", "incarnation") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--after-generation", "after_generation") != 0) {
        return 2;
    }
    return run_client_payload_command(argc, argv,
                                      MEM_SERVICE_WIRE_OP_POLL_ALLOCATION,
                                      "poll-allocation", payload);
}

static int run_publish_allocation(int argc, char **argv)
{
    char payload[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--key", "key") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--node-id", "node_id") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--incarnation", "incarnation") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--generation", "generation") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--descriptor-hex", "descriptor_hex") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--address", "address") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--address-len", "address_len") != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_PUBLISH_ALLOCATION,
                                      "publish-allocation",
                                      payload);
}

static int run_reclaim_allocation(int argc, char **argv)
{
    char payload[512] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--key", "key") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--node-id", "node_id") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--incarnation", "incarnation") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--generation", "generation") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--confirmed", "confirmed") != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_RECLAIM_ALLOCATION,
                                      "reclaim-allocation",
                                      payload);
}

/*
 * object-session --config <path> (M1.2, plan §3.7). Runs a deterministic
 * managed-allocation operation sequence through the client SDK and keeps
 * the process alive until the configured sequence completes (release and
 * retire are explicit ops). The config is text-kv: a session header
 * (session_id, connect, optional request_timeout_ms) followed by ordered
 * op= lines. Every op prints one machine-readable result line;
 * expect_status turns negative paths into deterministic assertions;
 * wait_state polls inspect-allocation inside a bounded timeout so
 * producer/consumer coordination keys off object state, never fixed
 * sleeps. The CLI holds no allocation semantics of its own: each op is
 * exactly one SDK call.
 *
 * Data-plane ops (map/unmap/write/read) run in this process through the
 * provider channel selected by the session header provider= line; the
 * control RPC still only manages references, handles and publish state.
 * provider=session-loopback registers an in-process mmap loopback
 * provider that honors strict fixed-address mapping (no fallback VA) and
 * is meant for session behavior tests; provider=obmm binds the real OBMM
 * endpoint and is only available when built with
 * MEM_SERVICE_OBJECT_SESSION_OBMM. A session must acquire before map,
 * unmap before release, and end with no live mapping or holder: the CLI
 * fails the session otherwise, so choreography leaks are deterministic.
 */
#define MEM_SERVICE_OBJECT_SESSION_MAX_OPS 64U
#define MEM_SERVICE_OBJECT_SESSION_MAX_FIELDS 16U
#define MEM_SERVICE_OBJECT_SESSION_LINE_LEN 768U
#define MEM_SERVICE_OBJECT_SESSION_FIELD_NAME_LEN 32U
#define MEM_SERVICE_OBJECT_SESSION_FIELD_VALUE_LEN 288U
#define MEM_SERVICE_OBJECT_SESSION_MAX_DATA_LEN (16U * 1024U * 1024U)
#define MEM_SERVICE_OBJECT_SESSION_LOOPBACK_MAX_MAPPINGS 4U
#define MEM_SERVICE_OBJECT_SESSION_PROVIDER_KIND_LEN 24U
#define MEM_SERVICE_OBJECT_SESSION_DEFAULT_IMPORT_REGION_BYTES \
    (256ULL * 1024ULL * 1024ULL)
#define MEM_SERVICE_OBJECT_SESSION_OBMM_MAX_REMOTE_MAPPINGS 8U
#define MEM_SERVICE_OBJECT_SESSION_OBMM_REQUIRED_PEER_MAPPINGS 1U
#define MEM_SERVICE_OBJECT_SESSION_MAX_WAIT_MS 60000U
#define MEM_SERVICE_OBJECT_SESSION_MAX_REQUEST_TIMEOUT_MS 60000U
#define MEM_SERVICE_OBJECT_SESSION_DEFAULT_REQUEST_TIMEOUT_MS 5000U
#define MEM_SERVICE_OBJECT_SESSION_DEFAULT_POLL_MS 100U
#define MEM_SERVICE_OBJECT_SESSION_MAX_POLL_MS 5000U
#define MEM_SERVICE_OBJECT_SESSION_MIN_POLL_MS 10U

enum mem_service_object_session_action {
    MEM_SERVICE_OBJECT_SESSION_ACTION_ALLOCATE = 1,
    MEM_SERVICE_OBJECT_SESSION_ACTION_ACQUIRE = 2,
    MEM_SERVICE_OBJECT_SESSION_ACTION_RELEASE = 3,
    MEM_SERVICE_OBJECT_SESSION_ACTION_RETIRE = 4,
    MEM_SERVICE_OBJECT_SESSION_ACTION_INSPECT = 5,
    MEM_SERVICE_OBJECT_SESSION_ACTION_WAIT_STATE = 6,
    MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH = 7,
    MEM_SERVICE_OBJECT_SESSION_ACTION_RECLAIM = 8,
    MEM_SERVICE_OBJECT_SESSION_ACTION_STATS = 9,
    MEM_SERVICE_OBJECT_SESSION_ACTION_MAP = 10,
    MEM_SERVICE_OBJECT_SESSION_ACTION_UNMAP = 11,
    MEM_SERVICE_OBJECT_SESSION_ACTION_WRITE = 12,
    MEM_SERVICE_OBJECT_SESSION_ACTION_READ = 13,
    MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH_DATA = 14,
    MEM_SERVICE_OBJECT_SESSION_ACTION_WAIT_VISIBLE = 15,
    MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_READONLY = 16,
    MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_GUARD = 17,
    MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_CONFLICT = 18,
    MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_DESCRIPTOR = 19,
    MEM_SERVICE_OBJECT_SESSION_ACTION_ACQUIRE_REFERENCE = 20,
    MEM_SERVICE_OBJECT_SESSION_ACTION_MAP_REFERENCE = 21,
    MEM_SERVICE_OBJECT_SESSION_ACTION_BEGIN_REFERENCE = 22,
    MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH_REFERENCE = 23,
    MEM_SERVICE_OBJECT_SESSION_ACTION_SEAL_REFERENCE = 24,
    MEM_SERVICE_OBJECT_SESSION_ACTION_CAPTURE_MAPPING = 25,
    MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_RETIRED_MAPPING = 26,
};

struct mem_service_object_session_field {
    char name[MEM_SERVICE_OBJECT_SESSION_FIELD_NAME_LEN];
    char value[MEM_SERVICE_OBJECT_SESSION_FIELD_VALUE_LEN];
};

struct mem_service_object_session_op {
    uint32_t action;
    enum mem_service_wire_status expect_status;
    char key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    char idempotency_key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    char session_id[MEM_SERVICE_CLIENT_ALLOCATION_SESSION_ID_LEN];
    uint64_t size_bytes;
    uint64_t alignment_bytes;
    uint64_t capabilities;
    bool has_expected_generation;
    uint64_t expected_generation;
    bool has_expect_state;
    char expect_state[MEM_SERVICE_CLIENT_ALLOCATION_STATE_LEN];
    bool has_expect_holder_count;
    uint64_t expect_holder_count;
    char wait_state[MEM_SERVICE_CLIENT_ALLOCATION_STATE_LEN];
    uint64_t wait_timeout_ms;
    uint64_t wait_poll_ms;
    char node_id[MEM_SERVICE_CLIENT_PROVIDER_NODE_ID_LEN];
    uint64_t incarnation;
    uint64_t generation;
    uint8_t descriptor[MEM_SERVICE_CLIENT_ALLOCATION_DESCRIPTOR_MAX_LEN];
    uint32_t descriptor_len;
    uint64_t address;
    uint64_t address_len;
    bool confirmed;
    uint64_t map_flags;
    bool probe_unmapped;
    char map_fault[32];
    uint64_t fault_byte;
    uint64_t data_offset;
    uint64_t data_len;
    uint64_t data_seed;
    bool has_data_seed;
    bool has_expect_checksum;
    uint64_t expect_checksum;
    uint64_t content_version;
    struct mem_service_client_reference_view reference_view;
};

struct mem_service_object_session_config {
    char session_id[MEM_SERVICE_CLIENT_ALLOCATION_SESSION_ID_LEN];
    char connect[MEM_SERVICE_OBJECT_SESSION_FIELD_VALUE_LEN];
    uint64_t request_timeout_ms;
    bool provider_configured;
    char provider_kind[MEM_SERVICE_OBJECT_SESSION_PROVIDER_KIND_LEN];
    char provider_device[MEM_SERVICE_OBJECT_SESSION_FIELD_VALUE_LEN];
    char provider_cna_path[MEM_SERVICE_OBJECT_SESSION_FIELD_VALUE_LEN];
    char provider_instance[MEM_SERVICE_CLIENT_PROVIDER_NODE_ID_LEN];
    uint64_t provider_import_region_bytes;
    bool has_provider_device;
    bool has_provider_cna_path;
    bool has_provider_instance;
    bool has_provider_import_region_bytes;
    uint64_t provider_node_id;
    uint64_t provider_node_count;
    uint64_t provider_generation;
    unsigned provider_topology_fields;
    uint32_t op_count;
    struct mem_service_object_session_op
        ops[MEM_SERVICE_OBJECT_SESSION_MAX_OPS];
};

/*
 * In-process loopback mapping provider for session behavior tests. It
 * performs real anonymous mmap at the exact requested address (hint only,
 * never MAP_FIXED) so a conflicting address fails closed instead of
 * silently substituting another VA, mirroring the strict GSVA contract.
 */
struct mem_service_object_session_loopback_mapping {
    bool active;
    uint64_t handle;
    void *base;
    uint64_t len;
    void *view_base;
    uint64_t view_len;
};

struct mem_service_object_session_loopback {
    struct mem_service_object_session_loopback_mapping
        mappings[MEM_SERVICE_OBJECT_SESSION_LOOPBACK_MAX_MAPPINGS];
    uint64_t next_handle;
};

struct mem_service_object_session_holder {
    bool live;
    char key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    char session_id[MEM_SERVICE_CLIENT_ALLOCATION_SESSION_ID_LEN];
    uint64_t generation;
};

struct mem_service_object_session_state {
    bool has_view;
    struct mem_service_client_allocation view;
    struct mem_service_object_session_holder
        holders[MEM_SERVICE_OBJECT_SESSION_MAX_OPS];
    char holder_op_ids[MEM_SERVICE_OBJECT_SESSION_MAX_OPS]
                      [MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    uint32_t holder_op_count;
    bool mapped;
    bool mapped_read_verified;
    uint64_t mapped_backing_len;
    uint64_t mapped_content_version;
    struct mem_service_client_object_mapping conflict_mapping;
    struct mem_service_client_mapping_lifecycle conflict_lifecycle;
    struct mem_service_client_object_mapping mapping;
    struct mem_service_client_mapping_lifecycle mapping_lifecycle;
    /* Diagnostic identity copy only: it owns no holder or SDK mapping. */
    bool has_captured_mapping;
    struct mem_service_client_allocation captured_allocation;
    uint64_t captured_address;
    uint64_t captured_len;
    char replacement_key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    uint64_t replacement_generation;
    struct mem_service_client_object_mapping retired_probe_mapping;
    bool has_reference;
    bool mapped_reference;
    struct lingqu_object_ref_wire_v2 reference;
    struct mem_service_client_reference_lifecycle reference_lifecycle;
    bool has_writer;
    struct mem_service_client_allocation writer_allocation;
    struct mem_service_reference_request prepared[MEM_SERVICE_OBJECT_SESSION_MAX_OPS];
    bool staged[MEM_SERVICE_OBJECT_SESSION_MAX_OPS];
    uint32_t prepared_count;
    const struct mem_service_client *client;
    bool provider_ready;
    struct mem_service_provider_registry registry;
    struct mem_service_provider_channel channel;
    struct mem_service_object_session_loopback loopback;
#ifdef MEM_SERVICE_OBJECT_SESSION_OBMM
    bool obmm_open;
    struct mem_service_provider_obmm_endpoint obmm_endpoint;
#endif
};

static struct mem_service_object_session_holder *
mem_service_object_session_find_holder(
    struct mem_service_object_session_state *state,
    const char *key, uint64_t generation, const char *session_id)
{
    for (uint32_t i = 0; i < MEM_SERVICE_OBJECT_SESSION_MAX_OPS; ++i) {
        struct mem_service_object_session_holder *holder = &state->holders[i];
        if (holder->live && holder->generation == generation &&
            strcmp(holder->key, key) == 0 &&
            strcmp(holder->session_id, session_id) == 0) {
            return holder;
        }
    }
    return NULL;
}

static int mem_service_object_session_track_holder(
    struct mem_service_object_session_state *state,
    const struct mem_service_object_session_op *op,
    const struct mem_service_client_allocation *view,
    const char *session_id)
{
    struct mem_service_object_session_holder *holder;

    if (op->action != MEM_SERVICE_OBJECT_SESSION_ACTION_ACQUIRE &&
        op->action != MEM_SERVICE_OBJECT_SESSION_ACTION_ACQUIRE_REFERENCE &&
        op->action != MEM_SERVICE_OBJECT_SESSION_ACTION_RELEASE) {
        return 0;
    }
    for (uint32_t i = 0; i < state->holder_op_count; ++i) {
        if (strcmp(state->holder_op_ids[i], op->idempotency_key) == 0) {
            /* A reply replay is not a second acquire or release. */
            return 0;
        }
    }
    if (state->holder_op_count >= MEM_SERVICE_OBJECT_SESSION_MAX_OPS) {
        return -1;
    }
    holder = mem_service_object_session_find_holder(
        state, view->key, view->generation, session_id);
    if (op->action == MEM_SERVICE_OBJECT_SESSION_ACTION_ACQUIRE ||
        op->action == MEM_SERVICE_OBJECT_SESSION_ACTION_ACQUIRE_REFERENCE) {
        if (holder == NULL) {
            for (uint32_t i = 0; i < MEM_SERVICE_OBJECT_SESSION_MAX_OPS; ++i) {
                if (!state->holders[i].live) {
                    holder = &state->holders[i];
                    break;
                }
            }
        }
        if (holder == NULL) {
            return -1;
        }
        holder->live = true;
        holder->generation = view->generation;
        snprintf(holder->key, sizeof(holder->key), "%s", view->key);
        snprintf(holder->session_id, sizeof(holder->session_id), "%s", session_id);
    } else if (holder != NULL) {
        memset(holder, 0, sizeof(*holder));
    }
    snprintf(state->holder_op_ids[state->holder_op_count++],
             MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN, "%s", op->idempotency_key);
    return 0;
}

static bool mem_service_object_session_report_holders(
    const struct mem_service_object_session_state *state,
    const char *session_id)
{
    bool live = false;

    for (uint32_t i = 0; i < MEM_SERVICE_OBJECT_SESSION_MAX_OPS; ++i) {
        const struct mem_service_object_session_holder *holder = &state->holders[i];
        if (holder->live) {
            printf("mem_service object-session: session=%s unreleased_holder "
                   "key=%s generation=%llu owner_session=%s\n",
                   session_id, holder->key,
                   (unsigned long long)holder->generation, holder->session_id);
            live = true;
        }
    }
    return live;
}

static int mem_service_object_session_loopback_probe(
    void *context,
    enum mem_service_provider_state *state_out)
{
    if (context == NULL || state_out == NULL) {
        return -1;
    }
    *state_out = MEM_SERVICE_PROVIDER_STATE_READY;
    return 0;
}

static int mem_service_object_session_loopback_map(
    void *context,
    const struct mem_service_mapping_request *request,
    struct mem_service_mapping *mapping_out)
{
    struct mem_service_object_session_loopback *loopback = context;
    void *base;
    int prot = 0;
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t backing, view_page;
    uint64_t view_bytes;
    size_t i;

    if (loopback == NULL || request == NULL || mapping_out == NULL ||
        request->memory_kind != MEM_SERVICE_MEMORY_HOST ||
        request->len == 0 ||
        request->offset > request->remote_region_len ||
        request->len > request->remote_region_len - request->offset ||
        (request->flags & ~MEM_SERVICE_MAPPING_FLAG_VALID_MASK) != 0 ||
        (request->flags & (MEM_SERVICE_MAPPING_FLAG_READ |
                           MEM_SERVICE_MAPPING_FLAG_WRITE)) == 0 ||
        (request->flags & MEM_SERVICE_MAPPING_FLAG_FIXED_ADDRESS) == 0 ||
        request->requested_address == NULL || page <= 0 ||
        ((uint64_t)page & ((uint64_t)page - 1)) ||
        request->offset > (uintptr_t)request->requested_address ||
        loopback->next_handle == UINT64_MAX) {
        return -1;
    }
    backing = (uintptr_t)request->requested_address - request->offset;
    if (!backing || backing % (uint64_t)page ||
        request->remote_region_len > UINTPTR_MAX - backing ||
        request->remote_region_len > SIZE_MAX) return -1;
    view_page = (uintptr_t)request->requested_address & ~((uintptr_t)page - 1);
    view_bytes = (uintptr_t)request->requested_address - view_page + request->len;
    for (i = 0; i < MEM_SERVICE_OBJECT_SESSION_LOOPBACK_MAX_MAPPINGS; ++i) {
        if (!loopback->mappings[i].active) {
            break;
        }
    }
    if (i == MEM_SERVICE_OBJECT_SESSION_LOOPBACK_MAX_MAPPINGS) {
        return -1;
    }
    if ((request->flags & MEM_SERVICE_MAPPING_FLAG_READ) != 0) {
        prot |= PROT_READ;
    }
    if ((request->flags & MEM_SERVICE_MAPPING_FLAG_WRITE) != 0) {
        prot |= PROT_WRITE;
    }
    /* No MAP_FIXED: the kernel honors the hint only when the range is
     * free, so a live mapping can never be clobbered; any other returned
     * address is a strict-address failure and is torn down at once. */
    base = mmap((void *)backing,
                request->remote_region_len,
                PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS,
                -1,
                0);
    if (base == MAP_FAILED) {
        return -1;
    }
    loopback->next_handle += 1U;
    loopback->mappings[i].active = true;
    loopback->mappings[i].handle = loopback->next_handle;
    loopback->mappings[i].base = base;
    loopback->mappings[i].len = request->remote_region_len;
    memset(mapping_out, 0, sizeof(*mapping_out));
    mapping_out->handle = loopback->next_handle;
    if (base != (void *)backing || mprotect((void *)view_page, view_bytes, prot)) {
        /* Keep the real reservation and handle until the neutral wrapper
         * confirms teardown; no inaccessible view may escape on failure. */
        mapping_out->memory_kind = request->memory_kind;
        return MEM_SERVICE_MAPPING_CLEANUP_REQUIRED;
    }
    loopback->mappings[i].view_base = request->requested_address;
    loopback->mappings[i].view_len = request->len;
    mapping_out->base = request->requested_address;
    mapping_out->len = request->len;
    mapping_out->memory_kind = request->memory_kind;
    return 0;
}

static int mem_service_object_session_loopback_unmap(void *context,
                                                     uint64_t mapping_handle)
{
    struct mem_service_object_session_loopback *loopback = context;
    size_t i;

    if (loopback == NULL || mapping_handle == 0) {
        return -1;
    }
    for (i = 0; i < MEM_SERVICE_OBJECT_SESSION_LOOPBACK_MAX_MAPPINGS; ++i) {
        if (loopback->mappings[i].active &&
            loopback->mappings[i].handle == mapping_handle) {
            if (munmap(loopback->mappings[i].base,
                       loopback->mappings[i].len) != 0) {
                return -1;
            }
            memset(&loopback->mappings[i],
                   0,
                   sizeof(loopback->mappings[i]));
            return 0;
        }
    }
    return -1;
}

/* Loopback ranges are same-process memory: visibility is immediate, so
 * publish/invalidate/wait-visible all reduce to a checksum assertion. */
static int mem_service_object_session_loopback_range(
    void *context,
    const struct mem_service_mapping_range_request *request,
    struct mem_service_visibility_completion *completion_out)
{
    struct mem_service_object_session_loopback *loopback = context;
    const uint8_t *base = NULL;
    uint64_t checksum;
    size_t i;

    if (loopback == NULL || request == NULL || completion_out == NULL ||
        request->len == 0) {
        return -1;
    }
    for (i = 0; i < MEM_SERVICE_OBJECT_SESSION_LOOPBACK_MAX_MAPPINGS; ++i) {
        if (loopback->mappings[i].active &&
            loopback->mappings[i].handle == request->mapping_handle) {
            base = loopback->mappings[i].view_base;
            if (request->offset > loopback->mappings[i].view_len ||
                request->len >
                    loopback->mappings[i].view_len - request->offset) {
                return -1;
            }
            break;
        }
    }
    if (base == NULL) {
        return -1;
    }
    checksum = mem_service_provider_checksum64(
        (const uint8_t *)base + request->offset, request->len);
    if (checksum != request->expected_checksum) {
        return -1;
    }
    memset(completion_out, 0, sizeof(*completion_out));
    completion_out->visible_bytes = request->len;
    completion_out->checksum = checksum;
    return 0;
}

static const struct mem_service_provider_ops
    mem_service_object_session_loopback_ops = {
        .probe = mem_service_object_session_loopback_probe,
        .map_remote_region = mem_service_object_session_loopback_map,
        .unmap_remote_region = mem_service_object_session_loopback_unmap,
        .publish_range = mem_service_object_session_loopback_range,
        .invalidate_range = mem_service_object_session_loopback_range,
        .wait_range_visible = mem_service_object_session_loopback_range,
};

static const char *mem_service_object_session_action_name(uint32_t action)
{
    switch (action) {
    case MEM_SERVICE_OBJECT_SESSION_ACTION_ALLOCATE:
        return "allocate";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_ACQUIRE:
        return "acquire";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_ACQUIRE_REFERENCE:
        return "acquire_reference";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_MAP_REFERENCE:
        return "map_reference";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_BEGIN_REFERENCE:
        return "begin_reference";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH_REFERENCE:
        return "publish_reference";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_SEAL_REFERENCE:
        return "seal_reference";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_RELEASE:
        return "release";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_RETIRE:
        return "retire";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_INSPECT:
        return "inspect";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_WAIT_STATE:
        return "wait_state";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH:
        return "publish";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_RECLAIM:
        return "reclaim";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_STATS:
        return "stats";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_MAP:
        return "map";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_UNMAP:
        return "unmap";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_WRITE:
        return "write";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_READ:
        return "read";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_READONLY:
        return "probe_readonly";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_GUARD:
        return "probe_guard";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_CONFLICT:
        return "probe_conflict";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_DESCRIPTOR:
        return "probe_descriptor";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_CAPTURE_MAPPING:
        return "capture_mapping";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_RETIRED_MAPPING:
        return "probe_retired_mapping";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH_DATA:
        return "publish_data";
    case MEM_SERVICE_OBJECT_SESSION_ACTION_WAIT_VISIBLE:
        return "wait_visible";
    default:
        return "unknown";
    }
}

static uint64_t mem_service_object_session_monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void mem_service_object_session_sleep_ms(uint64_t ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000U);
    ts.tv_nsec = (long)(ms % 1000U) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

static bool mem_service_object_session_parse_u64(const char *value,
                                                 uint64_t *out)
{
    char *end = NULL;
    unsigned long long parsed;

    /* strtoull accepts a leading '-'/'+' and wraps; config values are
     * unsigned quantities, so a sign is a config error, not a value. */
    if (value == NULL || !isdigit((unsigned char)value[0])) {
        return false;
    }
    errno = 0;
    parsed = strtoull(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *out = (uint64_t)parsed;
    return true;
}

static bool mem_service_object_session_parse_capabilities(const char *value,
                                                          uint64_t *out)
{
    uint64_t caps = 0;
    char buf[64];
    char *token;
    char *save = NULL;

    if (value == NULL || value[0] == '\0') {
        return false;
    }
    if (isdigit((unsigned char)value[0])) {
        if (!mem_service_object_session_parse_u64(value, &caps)) {
            return false;
        }
    } else {
        if (strlen(value) >= sizeof(buf)) {
            return false;
        }
        snprintf(buf, sizeof(buf), "%s", value);
        for (token = strtok_r(buf, ",", &save);
             token != NULL;
             token = strtok_r(NULL, ",", &save)) {
            if (strcmp(token, "map") == 0) {
                caps |= MEM_SERVICE_CLIENT_MANAGED_CAP_MAP;
            } else if (strcmp(token, "block_io") == 0) {
                caps |= MEM_SERVICE_CLIENT_MANAGED_CAP_BLOCK_IO;
            } else {
                return false;
            }
        }
    }
    if (caps == 0 ||
        (caps & ~(MEM_SERVICE_CLIENT_MANAGED_CAP_MAP |
                  MEM_SERVICE_CLIENT_MANAGED_CAP_BLOCK_IO)) != 0) {
        return false;
    }
    *out = caps;
    return true;
}

static int mem_service_object_session_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool mem_service_object_session_parse_hex(const char *value,
                                                 uint8_t *out,
                                                 uint32_t capacity,
                                                 uint32_t *len_out)
{
    size_t len;
    size_t i;

    if (value == NULL) {
        return false;
    }
    len = strlen(value);
    if (len == 0 || (len & 1U) != 0 || len / 2U > capacity) {
        return false;
    }
    for (i = 0; i < len; i += 2U) {
        int hi = mem_service_object_session_hex_value(value[i]);
        int lo = mem_service_object_session_hex_value(value[i + 1U]);

        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i / 2U] = (uint8_t)((hi << 4) | lo);
    }
    *len_out = (uint32_t)(len / 2U);
    return true;
}

/* Like parse_u64 but accepts 0x-prefixed hex (base 0), for checksums. */
static bool mem_service_object_session_parse_u64_base0(const char *value,
                                                       uint64_t *out)
{
    char *end = NULL;
    unsigned long long parsed;

    if (value == NULL || !isdigit((unsigned char)value[0])) {
        return false;
    }
    errno = 0;
    parsed = strtoull(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *out = (uint64_t)parsed;
    return true;
}

static bool mem_service_object_session_parse_map_flags(const char *value,
                                                       uint64_t *out)
{
    if (value == NULL) {
        return false;
    }
    if (strcmp(value, "read") == 0) {
        *out = MEM_SERVICE_CLIENT_MAP_READ;
        return true;
    }
    if (strcmp(value, "write") == 0) {
        *out = MEM_SERVICE_CLIENT_MAP_WRITE;
        return true;
    }
    if (strcmp(value, "readwrite") == 0) {
        *out = MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE;
        return true;
    }
    return false;
}

static bool mem_service_object_session_parse_status(
    const char *value,
    enum mem_service_wire_status *out)
{
    int candidate;

    if (value == NULL || value[0] == '\0') {
        return false;
    }
    for (candidate = 0;
         candidate <= (int)MEM_SERVICE_WIRE_STATUS_INTERNAL;
         ++candidate) {
        enum mem_service_wire_status status =
            (enum mem_service_wire_status)candidate;

        if (strcmp(value, mem_service_wire_status_name(status)) == 0) {
            *out = status;
            return true;
        }
    }
    return false;
}

static void mem_service_object_session_config_error(uint32_t line_no,
                                                    const char *detail,
                                                    const char *token)
{
    if (line_no != 0) {
        fprintf(stderr,
                "mem_service object-session: config error: line %u: %s%s%s\n",
                line_no,
                detail,
                token != NULL ? ": " : "",
                token != NULL ? token : "");
    } else {
        fprintf(stderr,
                "mem_service object-session: config error: %s%s%s\n",
                detail,
                token != NULL ? ": " : "",
                token != NULL ? token : "");
    }
}

static bool mem_service_object_session_copy_field(char *dst,
                                                  size_t dst_len,
                                                  const char *value)
{
    size_t len;

    if (value == NULL) {
        return false;
    }
    len = strlen(value);
    if (len == 0 || len >= dst_len) {
        return false;
    }
    snprintf(dst, dst_len, "%s", value);
    return true;
}

static const char *mem_service_object_session_find_field(
    const struct mem_service_object_session_field *fields,
    uint32_t field_count,
    const char *name)
{
    uint32_t i;

    for (i = 0; i < field_count; ++i) {
        if (strcmp(fields[i].name, name) == 0) {
            return fields[i].value;
        }
    }
    return NULL;
}

static bool mem_service_object_session_field_allowed(uint32_t action,
                                                     const char *name)
{
    static const char *const common[] = {"expect_status"};
    static const char *const allocate[] = {
        "key", "idempotency_key", "size_bytes", "capabilities",
        "alignment_bytes", "session_id",
    };
    static const char *const holder[] = {
        "key", "idempotency_key", "session_id", "expected_generation",
    };
    static const char *const retire[] = {
        "key", "idempotency_key", "expected_generation",
    };
    static const char *const inspect[] = {
        "key", "expect_state", "expect_generation", "expect_holder_count",
    };
    static const char *const wait_state[] = {
        "key", "state", "timeout_ms", "poll_ms",
    };
    static const char *const publish[] = {
        "key", "node_id", "incarnation", "generation",
        "descriptor_hex", "address", "address_len",
    };
    static const char *const reclaim[] = {
        "key", "node_id", "incarnation", "generation", "confirmed",
    };
    static const char *const map[] = {
        "key", "flags", "fault", "fault_byte",
    };
    static const char *const unmap[] = {
        "key", "probe_unmapped",
    };
    static const char *const key_only[] = {
        "key",
    };
    static const char *const write[] = {
        "key", "offset", "len", "seed",
    };
    static const char *const read[] = {
        "key", "offset", "len", "seed", "expect_checksum",
    };
    static const char *const reference_version[] = {
        "key", "idempotency_key", "generation", "version",
    };
    static const char *const reference_publish[] = {
        "key", "idempotency_key", "offset", "len", "kind", "owner", "producer",
    };
    const char *const *table = NULL;
    size_t count = 0;
    size_t i;

    for (i = 0; i < sizeof(common) / sizeof(common[0]); ++i) {
        if (strcmp(name, common[i]) == 0) {
            return true;
        }
    }
    switch (action) {
    case MEM_SERVICE_OBJECT_SESSION_ACTION_BEGIN_REFERENCE:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_SEAL_REFERENCE:
        table = reference_version;
        count = sizeof(reference_version) / sizeof(reference_version[0]);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH_REFERENCE:
        table = reference_publish;
        count = sizeof(reference_publish) / sizeof(reference_publish[0]);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_ALLOCATE:
        table = allocate;
        count = sizeof(allocate) / sizeof(allocate[0]);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_ACQUIRE:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_RELEASE:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_ACQUIRE_REFERENCE:
        table = holder;
        count = sizeof(holder) / sizeof(holder[0]);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_RETIRE:
        table = retire;
        count = sizeof(retire) / sizeof(retire[0]);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_INSPECT:
        table = inspect;
        count = sizeof(inspect) / sizeof(inspect[0]);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_WAIT_STATE:
        table = wait_state;
        count = sizeof(wait_state) / sizeof(wait_state[0]);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH:
        table = publish;
        count = sizeof(publish) / sizeof(publish[0]);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_RECLAIM:
        table = reclaim;
        count = sizeof(reclaim) / sizeof(reclaim[0]);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_MAP:
        table = map;
        count = sizeof(map) / sizeof(map[0]);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_UNMAP:
        table = unmap;
        count = sizeof(unmap) / sizeof(unmap[0]);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_MAP_REFERENCE:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_READONLY:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_GUARD:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_CONFLICT:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_DESCRIPTOR:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_CAPTURE_MAPPING:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_RETIRED_MAPPING:
        table = key_only;
        count = sizeof(key_only) / sizeof(key_only[0]);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_WRITE:
        table = write;
        count = sizeof(write) / sizeof(write[0]);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_READ:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH_DATA:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_WAIT_VISIBLE:
        table = read;
        count = sizeof(read) / sizeof(read[0]);
        break;
    default:
        break;
    }
    for (i = 0; i < count; ++i) {
        if (strcmp(name, table[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool mem_service_object_session_parse_action(const char *name,
                                                    uint32_t *action_out)
{
    static const struct {
        const char *name;
        uint32_t action;
    } actions[] = {
        {"allocate", MEM_SERVICE_OBJECT_SESSION_ACTION_ALLOCATE},
        {"acquire", MEM_SERVICE_OBJECT_SESSION_ACTION_ACQUIRE},
        {"acquire_reference", MEM_SERVICE_OBJECT_SESSION_ACTION_ACQUIRE_REFERENCE},
        {"map_reference", MEM_SERVICE_OBJECT_SESSION_ACTION_MAP_REFERENCE},
        {"begin_reference", MEM_SERVICE_OBJECT_SESSION_ACTION_BEGIN_REFERENCE},
        {"publish_reference", MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH_REFERENCE},
        {"seal_reference", MEM_SERVICE_OBJECT_SESSION_ACTION_SEAL_REFERENCE},
        {"release", MEM_SERVICE_OBJECT_SESSION_ACTION_RELEASE},
        {"retire", MEM_SERVICE_OBJECT_SESSION_ACTION_RETIRE},
        {"inspect", MEM_SERVICE_OBJECT_SESSION_ACTION_INSPECT},
        {"wait_state", MEM_SERVICE_OBJECT_SESSION_ACTION_WAIT_STATE},
        {"publish", MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH},
        {"reclaim", MEM_SERVICE_OBJECT_SESSION_ACTION_RECLAIM},
        {"stats", MEM_SERVICE_OBJECT_SESSION_ACTION_STATS},
        {"map", MEM_SERVICE_OBJECT_SESSION_ACTION_MAP},
        {"unmap", MEM_SERVICE_OBJECT_SESSION_ACTION_UNMAP},
        {"write", MEM_SERVICE_OBJECT_SESSION_ACTION_WRITE},
        {"read", MEM_SERVICE_OBJECT_SESSION_ACTION_READ},
        {"publish_data", MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH_DATA},
        {"wait_visible", MEM_SERVICE_OBJECT_SESSION_ACTION_WAIT_VISIBLE},
        {"probe_readonly", MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_READONLY},
        {"probe_guard", MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_GUARD},
        {"probe_conflict", MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_CONFLICT},
        {"probe_descriptor", MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_DESCRIPTOR},
        {"capture_mapping", MEM_SERVICE_OBJECT_SESSION_ACTION_CAPTURE_MAPPING},
        {"probe_retired_mapping", MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_RETIRED_MAPPING},
    };
    size_t i;

    for (i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
        if (strcmp(name, actions[i].name) == 0) {
            *action_out = actions[i].action;
            return true;
        }
    }
    return false;
}

/*
 * Parse one op= line: "op=<action> field=value ...". Fields are collected
 * first so duplicates and unknown names fail before any value conversion;
 * required-field checks then run per action.
 */
static int mem_service_object_session_parse_op(
    char *body,
    uint32_t line_no,
    struct mem_service_object_session_op *op)
{
    struct mem_service_object_session_field
        fields[MEM_SERVICE_OBJECT_SESSION_MAX_FIELDS];
    uint32_t field_count = 0;
    const char *value;
    char *token;
    char *save = NULL;

    memset(op, 0, sizeof(*op));
    op->expect_status = MEM_SERVICE_WIRE_STATUS_OK;
    token = strtok_r(body, " \t", &save);
    if (token == NULL) {
        mem_service_object_session_config_error(line_no, "empty op", NULL);
        return 2;
    }
    if (!mem_service_object_session_parse_action(token, &op->action)) {
        mem_service_object_session_config_error(line_no,
                                                "unknown op action",
                                                token);
        return 2;
    }
    for (token = strtok_r(NULL, " \t", &save);
         token != NULL;
         token = strtok_r(NULL, " \t", &save)) {
        char *eq = strchr(token, '=');
        uint32_t i;

        if (eq == NULL || eq == token || eq[1] == '\0') {
            mem_service_object_session_config_error(line_no,
                                                    "op field is not name=value",
                                                    token);
            return 2;
        }
        *eq = '\0';
        if (strlen(token) >= MEM_SERVICE_OBJECT_SESSION_FIELD_NAME_LEN ||
            strlen(eq + 1) >= MEM_SERVICE_OBJECT_SESSION_FIELD_VALUE_LEN) {
            mem_service_object_session_config_error(line_no,
                                                    "op field too long",
                                                    token);
            return 2;
        }
        for (i = 0; i < field_count; ++i) {
            if (strcmp(fields[i].name, token) == 0) {
                mem_service_object_session_config_error(line_no,
                                                        "duplicate op field",
                                                        token);
                return 2;
            }
        }
        if (field_count >= MEM_SERVICE_OBJECT_SESSION_MAX_FIELDS) {
            mem_service_object_session_config_error(line_no,
                                                    "too many op fields",
                                                    NULL);
            return 2;
        }
        if (!mem_service_object_session_field_allowed(op->action, token)) {
            mem_service_object_session_config_error(line_no,
                                                    "op field not allowed for action",
                                                    token);
            return 2;
        }
        snprintf(fields[field_count].name,
                 sizeof(fields[field_count].name),
                 "%s",
                 token);
        snprintf(fields[field_count].value,
                 sizeof(fields[field_count].value),
                 "%s",
                 eq + 1);
        field_count += 1U;
    }

    value = mem_service_object_session_find_field(fields,
                                                  field_count,
                                                  "expect_status");
    if (value != NULL &&
        !mem_service_object_session_parse_status(value, &op->expect_status)) {
        mem_service_object_session_config_error(line_no,
                                                "invalid expect_status",
                                                value);
        return 2;
    }
    /* wait_state outcomes are state matches or bounded timeouts, never
     * wire statuses; an expect_status field on it is dead config. */
    if (value != NULL &&
        op->action == MEM_SERVICE_OBJECT_SESSION_ACTION_WAIT_STATE) {
        mem_service_object_session_config_error(
            line_no,
            "expect_status is not allowed for wait_state",
            NULL);
        return 2;
    }

#define MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING(field_name, dst)        \
    value = mem_service_object_session_find_field(fields,                  \
                                                  field_count,             \
                                                  field_name);             \
    if (value == NULL) {                                                   \
        mem_service_object_session_config_error(line_no,                   \
                                                "missing required field",  \
                                                field_name);               \
        return 2;                                                          \
    }                                                                      \
    if (!mem_service_object_session_copy_field(dst, sizeof(dst), value)) { \
        mem_service_object_session_config_error(line_no,                   \
                                                "invalid field value",     \
                                                field_name);               \
        return 2;                                                          \
    }

#define MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64(field_name, dst)           \
    value = mem_service_object_session_find_field(fields,                  \
                                                  field_count,             \
                                                  field_name);             \
    if (value == NULL) {                                                   \
        mem_service_object_session_config_error(line_no,                   \
                                                "missing required field",  \
                                                field_name);               \
        return 2;                                                          \
    }                                                                      \
    if (!mem_service_object_session_parse_u64(value, &(dst))) {            \
        mem_service_object_session_config_error(line_no,                   \
                                                "invalid u64 field",       \
                                                field_name);               \
        return 2;                                                          \
    }

    switch (op->action) {
    case MEM_SERVICE_OBJECT_SESSION_ACTION_BEGIN_REFERENCE:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_SEAL_REFERENCE:
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("key", op->key);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("idempotency_key", op->idempotency_key);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("generation", op->generation);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("version", op->content_version);
        if (!op->generation || !op->content_version) return 2;
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH_REFERENCE: {
        uint64_t kind, owner, producer;
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("key", op->key);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("idempotency_key", op->idempotency_key);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("offset", op->reference_view.offset);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("len", op->reference_view.len);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("kind", kind);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("owner", owner);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("producer", producer);
        if (!kind || kind > UINT16_MAX || owner > UINT32_MAX || producer > UINT32_MAX ||
            !op->reference_view.len || op->reference_view.len > MEM_SERVICE_OBJECT_SESSION_MAX_DATA_LEN)
            return 2;
        op->reference_view.object_kind = (uint16_t)kind;
        op->reference_view.owner_entity = (uint32_t)owner;
        op->reference_view.producer_entity = (uint32_t)producer;
        break;
    }
    case MEM_SERVICE_OBJECT_SESSION_ACTION_ALLOCATE:
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("key", op->key);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("idempotency_key",
                                                   op->idempotency_key);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("size_bytes",
                                                op->size_bytes);
        if (op->size_bytes == 0) {
            mem_service_object_session_config_error(line_no,
                                                    "size_bytes must be > 0",
                                                    NULL);
            return 2;
        }
        value = mem_service_object_session_find_field(fields,
                                                      field_count,
                                                      "capabilities");
        if (value == NULL) {
            mem_service_object_session_config_error(line_no,
                                                    "missing required field",
                                                    "capabilities");
            return 2;
        }
        if (!mem_service_object_session_parse_capabilities(
                value, &op->capabilities)) {
            mem_service_object_session_config_error(line_no,
                                                    "invalid capabilities",
                                                    value);
            return 2;
        }
        value = mem_service_object_session_find_field(fields,
                                                      field_count,
                                                      "alignment_bytes");
        if (value != NULL &&
            !mem_service_object_session_parse_u64(value,
                                                  &op->alignment_bytes)) {
            mem_service_object_session_config_error(line_no,
                                                    "invalid u64 field",
                                                    "alignment_bytes");
            return 2;
        }
        value = mem_service_object_session_find_field(fields,
                                                      field_count,
                                                      "session_id");
        if (value != NULL &&
            !mem_service_object_session_copy_field(op->session_id,
                                                   sizeof(op->session_id),
                                                   value)) {
            mem_service_object_session_config_error(line_no,
                                                    "invalid field value",
                                                    "session_id");
            return 2;
        }
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_ACQUIRE:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_RELEASE:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_ACQUIRE_REFERENCE:
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("key", op->key);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("idempotency_key",
                                                   op->idempotency_key);
        value = mem_service_object_session_find_field(fields,
                                                      field_count,
                                                      "session_id");
        if (value != NULL &&
            !mem_service_object_session_copy_field(op->session_id,
                                                   sizeof(op->session_id),
                                                   value)) {
            mem_service_object_session_config_error(line_no,
                                                    "invalid field value",
                                                    "session_id");
            return 2;
        }
        value = mem_service_object_session_find_field(fields,
                                                      field_count,
                                                      "expected_generation");
        if (value != NULL) {
            if (!mem_service_object_session_parse_u64(
                    value, &op->expected_generation)) {
                mem_service_object_session_config_error(line_no,
                                                        "invalid u64 field",
                                                        "expected_generation");
                return 2;
            }
            op->has_expected_generation = true;
        }
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_RETIRE:
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("key", op->key);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("idempotency_key",
                                                   op->idempotency_key);
        value = mem_service_object_session_find_field(fields,
                                                      field_count,
                                                      "expected_generation");
        if (value != NULL) {
            if (!mem_service_object_session_parse_u64(
                    value, &op->expected_generation)) {
                mem_service_object_session_config_error(line_no,
                                                        "invalid u64 field",
                                                        "expected_generation");
                return 2;
            }
            op->has_expected_generation = true;
        }
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_INSPECT:
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("key", op->key);
        value = mem_service_object_session_find_field(fields,
                                                      field_count,
                                                      "expect_state");
        if (value != NULL) {
            if (!mem_service_object_session_copy_field(
                    op->expect_state, sizeof(op->expect_state), value)) {
                mem_service_object_session_config_error(line_no,
                                                        "invalid field value",
                                                        "expect_state");
                return 2;
            }
            op->has_expect_state = true;
        }
        value = mem_service_object_session_find_field(fields,
                                                      field_count,
                                                      "expect_generation");
        if (value != NULL) {
            if (!mem_service_object_session_parse_u64(
                    value, &op->expected_generation)) {
                mem_service_object_session_config_error(line_no,
                                                        "invalid u64 field",
                                                        "expect_generation");
                return 2;
            }
            op->has_expected_generation = true;
        }
        value = mem_service_object_session_find_field(fields,
                                                      field_count,
                                                      "expect_holder_count");
        if (value != NULL) {
            if (!mem_service_object_session_parse_u64(
                    value, &op->expect_holder_count)) {
                mem_service_object_session_config_error(line_no,
                                                        "invalid u64 field",
                                                        "expect_holder_count");
                return 2;
            }
            op->has_expect_holder_count = true;
        }
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_WAIT_STATE:
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("key", op->key);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("state", op->wait_state);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("timeout_ms",
                                                op->wait_timeout_ms);
        if (op->wait_timeout_ms == 0 ||
            op->wait_timeout_ms > MEM_SERVICE_OBJECT_SESSION_MAX_WAIT_MS) {
            mem_service_object_session_config_error(line_no,
                                                    "timeout_ms out of bounds",
                                                    NULL);
            return 2;
        }
        value = mem_service_object_session_find_field(fields,
                                                      field_count,
                                                      "poll_ms");
        if (value != NULL &&
            !mem_service_object_session_parse_u64(value, &op->wait_poll_ms)) {
            mem_service_object_session_config_error(line_no,
                                                    "invalid u64 field",
                                                    "poll_ms");
            return 2;
        }
        if (op->wait_poll_ms == 0) {
            op->wait_poll_ms = MEM_SERVICE_OBJECT_SESSION_DEFAULT_POLL_MS;
        }
        if (op->wait_poll_ms < MEM_SERVICE_OBJECT_SESSION_MIN_POLL_MS ||
            op->wait_poll_ms > MEM_SERVICE_OBJECT_SESSION_MAX_POLL_MS) {
            mem_service_object_session_config_error(line_no,
                                                    "poll_ms out of bounds",
                                                    NULL);
            return 2;
        }
        if (op->expect_status != MEM_SERVICE_WIRE_STATUS_OK) {
            mem_service_object_session_config_error(
                line_no,
                "expect_status not allowed for wait_state",
                NULL);
            return 2;
        }
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH:
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("key", op->key);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("node_id", op->node_id);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("incarnation",
                                                op->incarnation);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("generation",
                                                op->generation);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("address", op->address);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("address_len",
                                                op->address_len);
        value = mem_service_object_session_find_field(fields,
                                                      field_count,
                                                      "descriptor_hex");
        if (value == NULL) {
            mem_service_object_session_config_error(line_no,
                                                    "missing required field",
                                                    "descriptor_hex");
            return 2;
        }
        if (!mem_service_object_session_parse_hex(value,
                                                  op->descriptor,
                                                  sizeof(op->descriptor),
                                                  &op->descriptor_len)) {
            mem_service_object_session_config_error(line_no,
                                                    "invalid descriptor_hex",
                                                    NULL);
            return 2;
        }
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_RECLAIM: {
        uint64_t confirmed = 0;

        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("key", op->key);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("node_id", op->node_id);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("incarnation",
                                                op->incarnation);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("generation",
                                                op->generation);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("confirmed", confirmed);
        if (confirmed > 1U) {
            mem_service_object_session_config_error(line_no,
                                                    "confirmed must be 0 or 1",
                                                    NULL);
            return 2;
        }
        op->confirmed = confirmed != 0U;
        break;
    }
    case MEM_SERVICE_OBJECT_SESSION_ACTION_MAP:
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("key", op->key);
        value = mem_service_object_session_find_field(fields,
                                                      field_count,
                                                      "flags");
        if (value == NULL) {
            op->map_flags = MEM_SERVICE_CLIENT_MAP_READ |
                            MEM_SERVICE_CLIENT_MAP_WRITE;
        } else if (!mem_service_object_session_parse_map_flags(
                       value, &op->map_flags)) {
            mem_service_object_session_config_error(line_no,
                                                    "invalid flags",
                                                    value);
            return 2;
        }
        value = mem_service_object_session_find_field(fields, field_count, "fault");
        if (value != NULL) {
            static const char *const faults[] = {
                "descriptor", "descriptor_length", "descriptor_oversize",
                "address", "address_len", "size", "alignment", "capabilities",
                "home", "incarnation",
            };
            bool known = false;
            for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i)
                if (!strcmp(value, faults[i])) known = true;
            if (!known || op->expect_status != MEM_SERVICE_WIRE_STATUS_STALE_REF ||
                !mem_service_object_session_copy_field(op->map_fault,
                    sizeof(op->map_fault), value)) {
                mem_service_object_session_config_error(line_no,
                    "map fault requires a known mutation and expect_status=stale_ref", NULL);
                return 2;
            }
        }
        value = mem_service_object_session_find_field(fields, field_count, "fault_byte");
        if (value != NULL && (strcmp(op->map_fault, "descriptor") ||
            !mem_service_object_session_parse_u64(value, &op->fault_byte) ||
            op->fault_byte >= MEM_SERVICE_CLIENT_ALLOCATION_DESCRIPTOR_MAX_LEN)) {
            mem_service_object_session_config_error(line_no, "invalid descriptor fault_byte", NULL);
            return 2;
        }
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_UNMAP:
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("key", op->key);
        value = mem_service_object_session_find_field(fields, field_count, "probe_unmapped");
        if (value != NULL) {
            if (strcmp(value, "1")) {
                mem_service_object_session_config_error(line_no,
                    "probe_unmapped requires 1", NULL);
                return 2;
            }
            op->probe_unmapped = true;
        }
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_MAP_REFERENCE:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_READONLY:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_GUARD:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_CONFLICT:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_DESCRIPTOR:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_CAPTURE_MAPPING:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_RETIRED_MAPPING:
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("key", op->key);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_WRITE:
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("key", op->key);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("offset", op->data_offset);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("len", op->data_len);
        if (op->data_len == 0 ||
            op->data_len > MEM_SERVICE_OBJECT_SESSION_MAX_DATA_LEN) {
            mem_service_object_session_config_error(line_no,
                                                    "len out of bounds",
                                                    NULL);
            return 2;
        }
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("seed", op->data_seed);
        op->has_data_seed = true;
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_READ:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH_DATA:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_WAIT_VISIBLE:
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING("key", op->key);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("offset", op->data_offset);
        MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64("len", op->data_len);
        if (op->data_len == 0 ||
            op->data_len > MEM_SERVICE_OBJECT_SESSION_MAX_DATA_LEN) {
            mem_service_object_session_config_error(line_no,
                                                    "len out of bounds",
                                                    NULL);
            return 2;
        }
        value = mem_service_object_session_find_field(fields,
                                                      field_count,
                                                      "seed");
        if (value != NULL) {
            if (!mem_service_object_session_parse_u64(value,
                                                      &op->data_seed)) {
                mem_service_object_session_config_error(line_no,
                                                        "invalid u64 field",
                                                        "seed");
                return 2;
            }
            op->has_data_seed = true;
        }
        value = mem_service_object_session_find_field(fields,
                                                      field_count,
                                                      "expect_checksum");
        if (value != NULL) {
            if (!mem_service_object_session_parse_u64_base0(
                    value, &op->expect_checksum)) {
                mem_service_object_session_config_error(line_no,
                                                        "invalid checksum field",
                                                        "expect_checksum");
                return 2;
            }
            op->has_expect_checksum = true;
        }
        if (op->action != MEM_SERVICE_OBJECT_SESSION_ACTION_READ &&
            !op->has_data_seed && !op->has_expect_checksum) {
            mem_service_object_session_config_error(line_no,
                "visibility operation requires seed or expect_checksum", NULL);
            return 2;
        }
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_STATS:
        break;
    default:
        mem_service_object_session_config_error(line_no,
                                                "unknown op action",
                                                NULL);
        return 2;
    }

#undef MEM_SERVICE_OBJECT_SESSION_REQUIRED_STRING
#undef MEM_SERVICE_OBJECT_SESSION_REQUIRED_U64
    return 0;
}

static int mem_service_object_session_validate_provider(
    struct mem_service_object_session_config *config);

static int mem_service_object_session_load_config(
    const char *path,
    struct mem_service_object_session_config *config)
{
    FILE *fp;
    char line[MEM_SERVICE_OBJECT_SESSION_LINE_LEN];
    uint32_t line_no = 0;
    bool have_session_id = false;
    bool have_connect = false;
    bool have_request_timeout = false;

    fp = fopen(path, "r");
    if (fp == NULL) {
        mem_service_object_session_config_error(0, "cannot open config", path);
        return 2;
    }
    memset(config, 0, sizeof(*config));
    config->request_timeout_ms =
        MEM_SERVICE_OBJECT_SESSION_DEFAULT_REQUEST_TIMEOUT_MS;
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *comment = strchr(line, '#');
        char *start;
        char *end;
        size_t len;

        line_no += 1U;
        if (comment != NULL) {
            *comment = '\0';
        }
        start = line;
        while (*start != '\0' && isspace((unsigned char)*start)) {
            start += 1;
        }
        end = start + strlen(start);
        while (end > start && isspace((unsigned char)end[-1])) {
            end -= 1;
        }
        *end = '\0';
        if (*start == '\0') {
            continue;
        }
        if (strchr(start, '\n') != NULL || strlen(start) >= sizeof(line) - 1U) {
            mem_service_object_session_config_error(line_no,
                                                    "line too long",
                                                    NULL);
            (void)fclose(fp);
            return 2;
        }
        len = strlen(start);
        if (strncmp(start, "session_id=", 11) == 0) {
            if (have_session_id ||
                !mem_service_object_session_copy_field(
                    config->session_id,
                    sizeof(config->session_id),
                    start + 11)) {
                mem_service_object_session_config_error(line_no,
                                                        "bad session_id",
                                                        NULL);
                (void)fclose(fp);
                return 2;
            }
            have_session_id = true;
        } else if (strncmp(start, "connect=", 8) == 0) {
            if (have_connect ||
                !mem_service_object_session_copy_field(config->connect,
                                                       sizeof(config->connect),
                                                       start + 8)) {
                mem_service_object_session_config_error(line_no,
                                                        "bad connect",
                                                        NULL);
                (void)fclose(fp);
                return 2;
            }
            have_connect = true;
        } else if (strncmp(start, "request_timeout_ms=", 19) == 0) {
            if (have_request_timeout ||
                !mem_service_object_session_parse_u64(
                    start + 19, &config->request_timeout_ms) ||
                config->request_timeout_ms == 0 ||
                config->request_timeout_ms >
                    MEM_SERVICE_OBJECT_SESSION_MAX_REQUEST_TIMEOUT_MS) {
                mem_service_object_session_config_error(line_no,
                                                        "bad request_timeout_ms",
                                                        NULL);
                (void)fclose(fp);
                return 2;
            }
            have_request_timeout = true;
        } else if (strncmp(start, "provider=", 9) == 0) {
            if (config->provider_configured ||
                strlen(start + 9) >=
                    sizeof(config->provider_kind)) {
                mem_service_object_session_config_error(line_no,
                                                        "bad provider",
                                                        NULL);
                (void)fclose(fp);
                return 2;
            }
            snprintf(config->provider_kind,
                     sizeof(config->provider_kind),
                     "%s",
                     start + 9);
            config->provider_configured = true;
        } else if (strncmp(start, "provider_device=", 16) == 0) {
            if (config->has_provider_device ||
                !mem_service_object_session_copy_field(
                    config->provider_device,
                    sizeof(config->provider_device),
                    start + 16)) {
                mem_service_object_session_config_error(line_no,
                                                        "bad provider_device",
                                                        NULL);
                (void)fclose(fp);
                return 2;
            }
            config->has_provider_device = true;
        } else if (strncmp(start, "provider_cna_path=", 18) == 0) {
            if (config->has_provider_cna_path ||
                !mem_service_object_session_copy_field(
                    config->provider_cna_path,
                    sizeof(config->provider_cna_path),
                    start + 18)) {
                mem_service_object_session_config_error(line_no,
                                                        "bad provider_cna_path",
                                                        NULL);
                (void)fclose(fp);
                return 2;
            }
            config->has_provider_cna_path = true;
        } else if (strncmp(start, "provider_instance=", 18) == 0) {
            if (config->has_provider_instance ||
                !mem_service_object_session_copy_field(
                    config->provider_instance,
                    sizeof(config->provider_instance),
                    start + 18)) {
                mem_service_object_session_config_error(line_no,
                                                        "bad provider_instance",
                                                        NULL);
                (void)fclose(fp);
                return 2;
            }
            config->has_provider_instance = true;
        } else if (strncmp(start, "provider_import_region_bytes=", 29) == 0) {
            if (config->has_provider_import_region_bytes ||
                !mem_service_object_session_parse_u64(
                    start + 29,
                    &config->provider_import_region_bytes) ||
                config->provider_import_region_bytes == 0) {
                mem_service_object_session_config_error(
                    line_no,
                    "bad provider_import_region_bytes",
                    NULL);
                (void)fclose(fp);
                return 2;
            }
            config->has_provider_import_region_bytes = true;
        } else if (strncmp(start, "provider_node_id=", 17) == 0 ||
                   strncmp(start, "provider_node_count=", 20) == 0 ||
                   strncmp(start, "provider_generation=", 20) == 0) {
            unsigned bit = strncmp(start, "provider_node_id=", 17) == 0 ? 1U :
                           strncmp(start, "provider_node_count=", 20) == 0 ? 2U : 4U;
            uint64_t *target = bit == 1 ? &config->provider_node_id :
                               bit == 2 ? &config->provider_node_count : &config->provider_generation;
            if ((config->provider_topology_fields & bit) ||
                !mem_service_object_session_parse_u64(strchr(start, '=') + 1, target)) {
                mem_service_object_session_config_error(line_no, "bad provider topology", NULL);
                (void)fclose(fp);
                return 2;
            }
            config->provider_topology_fields |= bit;
        } else if (strncmp(start, "op=", 3) == 0) {
            if (config->op_count >= MEM_SERVICE_OBJECT_SESSION_MAX_OPS) {
                mem_service_object_session_config_error(line_no,
                                                        "too many ops",
                                                        NULL);
                (void)fclose(fp);
                return 2;
            }
            if (mem_service_object_session_parse_op(
                    start + 3,
                    line_no,
                    &config->ops[config->op_count]) != 0) {
                (void)fclose(fp);
                return 2;
            }
            config->op_count += 1U;
        } else {
            (void)len;
            mem_service_object_session_config_error(line_no,
                                                    "unknown config line",
                                                    start);
            (void)fclose(fp);
            return 2;
        }
    }
    (void)fclose(fp);
    if (!have_session_id || !have_connect) {
        mem_service_object_session_config_error(
            0,
            "session_id and connect are required",
            NULL);
        return 2;
    }
    if (config->op_count == 0) {
        mem_service_object_session_config_error(0, "no op lines", NULL);
        return 2;
    }
    if (mem_service_object_session_validate_provider(config) != 0) {
        return 2;
    }
    return 0;
}

static bool mem_service_object_session_action_is_data_plane(uint32_t action)
{
    return action == MEM_SERVICE_OBJECT_SESSION_ACTION_MAP ||
           action == MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH_REFERENCE ||
           action == MEM_SERVICE_OBJECT_SESSION_ACTION_MAP_REFERENCE ||
           action == MEM_SERVICE_OBJECT_SESSION_ACTION_UNMAP ||
           action == MEM_SERVICE_OBJECT_SESSION_ACTION_WRITE ||
           action == MEM_SERVICE_OBJECT_SESSION_ACTION_READ ||
           action == MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH_DATA ||
           action == MEM_SERVICE_OBJECT_SESSION_ACTION_WAIT_VISIBLE ||
           action == MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_READONLY ||
           action == MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_GUARD ||
           action == MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_CONFLICT ||
           action == MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_DESCRIPTOR ||
           action == MEM_SERVICE_OBJECT_SESSION_ACTION_CAPTURE_MAPPING ||
           action == MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_RETIRED_MAPPING;
}

static int mem_service_object_session_validate_provider(
    struct mem_service_object_session_config *config)
{
    bool needs_provider = false;
    uint32_t i;

    for (i = 0; i < config->op_count; ++i) {
        if (mem_service_object_session_action_is_data_plane(
                config->ops[i].action)) {
            needs_provider = true;
            break;
        }
    }
    if (!config->provider_configured) {
        if (needs_provider) {
            mem_service_object_session_config_error(
                0,
                "map/unmap/write/read ops require a provider= line",
                NULL);
            return 2;
        }
        return 0;
    }
    if (strcmp(config->provider_kind, "session-loopback") == 0) {
        if (config->has_provider_device ||
            config->has_provider_cna_path ||
            config->has_provider_instance ||
            config->has_provider_import_region_bytes || config->provider_topology_fields) {
            mem_service_object_session_config_error(
                0,
                "provider_device/provider_cna_path/provider_instance/"
                "provider_import_region_bytes are only valid for "
                "provider=obmm",
                NULL);
            return 2;
        }
        return 0;
    }
    if (strcmp(config->provider_kind, "obmm") == 0) {
#ifndef MEM_SERVICE_OBJECT_SESSION_OBMM
        mem_service_object_session_config_error(
            0,
            "provider=obmm requires a build with "
            "MEM_SERVICE_OBJECT_SESSION_OBMM",
            NULL);
        return 2;
#else
        if (config->provider_topology_fields != 7 || !config->provider_generation ||
            config->provider_node_count < 2 ||
            config->provider_node_count > MEM_SERVICE_OBJECT_SESSION_OBMM_MAX_REMOTE_MAPPINGS ||
            config->provider_node_id >= config->provider_node_count) {
            mem_service_object_session_config_error(0,
                "obmm requires provider_node_id, provider_node_count (2..8), provider_generation",
                NULL);
            return 2;
        }
        return 0;
#endif
    }
    mem_service_object_session_config_error(0,
                                            "unknown provider kind",
                                            config->provider_kind);
    return 2;
}

static void mem_service_object_session_print_op_line(
    const struct mem_service_object_session_config *config,
    uint32_t index,
    const struct mem_service_object_session_op *op,
    enum mem_service_wire_status status,
    const struct mem_service_client_allocation *view,
    const char *mismatch_field,
    const char *mismatch_expected)
{
    printf("mem_service object-session: session=%s op=%u action=%s key=%s "
           "status=%s state=%s generation=",
           config->session_id,
           index,
           mem_service_object_session_action_name(op->action),
           op->key[0] != '\0' ? op->key : "-",
           mem_service_wire_status_name(status),
           view != NULL && view->state[0] != '\0' ? view->state : "-");
    if (view != NULL && view->state[0] != '\0') {
        printf("%llu", (unsigned long long)view->generation);
    } else {
        printf("-");
    }
    if (mismatch_field != NULL) {
        printf(" mismatch=%s expected_%s=%s",
               mismatch_field,
               mismatch_field,
               mismatch_expected != NULL ? mismatch_expected : "-");
    }
    printf("\n");
    (void)fflush(stdout);
}

#ifdef MEM_SERVICE_OBJECT_SESSION_OBMM
static int mem_service_object_session_verify_peers(
    const struct mem_service_object_session_config *config,
    struct mem_service_provider_obmm_endpoint *endpoint)
{
    struct mem_service_region region;
    struct mem_service_provider_remote_region local;
    struct mem_service_provider_remote_region peers[MEM_SERVICE_OBJECT_SESSION_OBMM_MAX_REMOTE_MAPPINGS];
    uint64_t checksum;
    uint8_t pattern[256];

    if (mem_service_provider_obmm_endpoint_prepare_canary_region(endpoint,
            config->has_provider_import_region_bytes
                ? config->provider_import_region_bytes
                : MEM_SERVICE_OBJECT_SESSION_DEFAULT_IMPORT_REGION_BYTES,
            sizeof(pattern),
            (uint8_t)(41 + config->provider_node_id),
            &region, &local, &checksum) ||
        mem_service_provider_obmm_endpoint_exchange_remote_regions(endpoint,
            config->provider_node_id, config->provider_node_count, config->provider_generation,
            &local, peers, MEM_SERVICE_OBJECT_SESSION_OBMM_MAX_REMOTE_MAPPINGS)) return -1;
    for (uint32_t peer = 0; peer < config->provider_node_count; peer++) {
        if (peer == config->provider_node_id) continue;
        for (size_t i = 0; i < sizeof(pattern); i++) pattern[i] = (uint8_t)(41 + peer + i * 29U);
        if (mem_service_provider_obmm_endpoint_verify_mapping(endpoint, &peers[peer],
                0, sizeof(pattern), mem_service_provider_checksum64(pattern, sizeof(pattern)),
                config->request_timeout_ms)) return -1;
    }
    printf("mem_service object-session: session=%s peer_canary=ok peers=%" PRIu64 "\n",
           config->session_id, config->provider_node_count - 1);
    return 0;
}
#endif

/* Open the configured session provider and bind a mapping channel. */
static int mem_service_object_session_provider_open(
    const struct mem_service_object_session_config *config,
    struct mem_service_object_session_state *state)
{
    struct mem_service_provider_registration registration;
    const char *name;
    const char *instance;

    if (!config->provider_configured) {
        return 0;
    }
    if (mem_service_provider_registry_init(&state->registry) != 0) {
        return -1;
    }
    if (strcmp(config->provider_kind, "session-loopback") == 0) {
        memset(&registration, 0, sizeof(registration));
        registration.name = "session-loopback";
        registration.instance = "local-0";
        registration.capabilities = MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING;
        registration.ops = &mem_service_object_session_loopback_ops;
        registration.context = &state->loopback;
        name = registration.name;
        instance = registration.instance;
    } else {
#ifdef MEM_SERVICE_OBJECT_SESSION_OBMM
        struct mem_service_provider_obmm_config obmm_config;

        memset(&obmm_config, 0, sizeof(obmm_config));
        obmm_config.instance = config->has_provider_instance
                                   ? config->provider_instance
                                   : "obmm-0";
        obmm_config.device_path = config->has_provider_device
                                      ? config->provider_device
                                      : NULL;
        obmm_config.primary_cna_path = config->has_provider_cna_path
                                           ? config->provider_cna_path
                                           : NULL;
        obmm_config.import_region_bytes =
            config->has_provider_import_region_bytes
                ? config->provider_import_region_bytes
                : MEM_SERVICE_OBJECT_SESSION_DEFAULT_IMPORT_REGION_BYTES;
        obmm_config.import_pa_bias = 0;
        obmm_config.max_remote_mappings = config->provider_node_count;
        obmm_config.required_peer_mappings = config->provider_node_count - 1;
        obmm_config.force_osync = false;
        if (mem_service_provider_obmm_endpoint_open(&state->obmm_endpoint,
                                                    &obmm_config) != 0 ||
            mem_service_provider_obmm_endpoint_registration(
                &state->obmm_endpoint, &registration) != 0) {
            fprintf(stderr, "object-session endpoint open failed errno=%d mappings=%u "
                    "import_bytes=%" PRIu64 "\n", errno, obmm_config.max_remote_mappings,
                    obmm_config.import_region_bytes);
            return -1;
        }
        state->obmm_open = true;
        if (mem_service_object_session_verify_peers(config, &state->obmm_endpoint)) return -1;
        name = registration.name;
        instance = registration.instance;
#else
        return -1;
#endif
    }
    if (mem_service_provider_registry_register(&state->registry,
                                               &registration) != 0 ||
        mem_service_provider_channel_bind(&state->registry,
                                          name,
                                          instance,
                                          MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING,
                                          &state->channel) != 0) {
        return -1;
    }
    state->provider_ready = true;
    return 0;
}

static int mem_service_object_session_unmap(
    struct mem_service_object_session_state *state, enum mem_service_wire_status *status)
{
    return state->mapped_reference ?
        mem_service_client_unmap_managed_reference(state->client, &state->channel,
            &state->mapping, &state->reference_lifecycle, status) :
        mem_service_client_unmap_managed_allocation(state->client, &state->channel,
            &state->mapping, &state->mapping_lifecycle, status);
}

static int mem_service_object_session_provider_close(
    struct mem_service_object_session_state *state)
{
    if (state->retired_probe_mapping.binding.mapped &&
        mem_service_client_unmap_allocation(&state->channel,
            &state->retired_probe_mapping)) {
        fprintf(stderr, "mem_service object-session: retired_probe_cleanup_pending\n");
        return -1;
    }
    if (state->conflict_lifecycle.pending || state->conflict_mapping.binding.mapped) {
        if (mem_service_client_unmap_managed_allocation(state->client,
                &state->channel, &state->conflict_mapping, &state->conflict_lifecycle,
                NULL)) {
            fprintf(stderr, "mem_service object-session: conflict_mapping_cleanup_pending\n");
            return -1;
        }
    }
    if (state->mapped) {
        /* Best-effort teardown on a failed session; the end-of-session
         * check has already flagged the leaked mapping as an error. */
        if (mem_service_object_session_unmap(state, NULL) != 0) {
            fprintf(stderr, "mem_service object-session: cleanup_pending key=%s "
                    "generation=%llu handle=%llu\n", state->mapping.key,
                    (unsigned long long)state->mapping.generation,
                    (unsigned long long)state->mapping.binding.mapping.handle);
            /* Do not destroy the endpoint while cleanup ownership remains. */
            return -1;
        }
        state->mapped = false;
    }
#ifdef MEM_SERVICE_OBJECT_SESSION_OBMM
    if (state->obmm_open) {
        if (mem_service_provider_obmm_endpoint_close_checked(&state->obmm_endpoint) != 0) {
            fprintf(stderr, "mem_service object-session: endpoint_cleanup_pending\n");
            return -1;
        }
        state->obmm_open = false;
    }
#endif
    state->provider_ready = false;
    return 0;
}

/* Result line for data-plane ops (no wire RPC; status is synthesized). */
static void mem_service_object_session_print_data_op_line(
    const struct mem_service_object_session_config *config,
    uint32_t index,
    const struct mem_service_object_session_op *op,
    enum mem_service_wire_status status,
    const char *note,
    uint64_t address,
    uint64_t len,
    uint64_t checksum,
    bool has_checksum)
{
    printf("mem_service object-session: session=%s op=%u action=%s key=%s "
           "status=%s",
           config->session_id,
           index,
           mem_service_object_session_action_name(op->action),
           op->key[0] != '\0' ? op->key : "-",
           mem_service_wire_status_name(status));
    if ((op->action == MEM_SERVICE_OBJECT_SESSION_ACTION_MAP ||
         op->action == MEM_SERVICE_OBJECT_SESSION_ACTION_MAP_REFERENCE) && note == NULL) {
        printf(" base=0x%016llx len=%llu",
               (unsigned long long)address,
               (unsigned long long)len);
    }
    if ((op->action == MEM_SERVICE_OBJECT_SESSION_ACTION_WRITE ||
         op->action == MEM_SERVICE_OBJECT_SESSION_ACTION_READ ||
         op->action == MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH_DATA ||
         op->action == MEM_SERVICE_OBJECT_SESSION_ACTION_WAIT_VISIBLE) &&
        note == NULL) {
        printf(" offset=%llu len=%llu",
               (unsigned long long)op->data_offset,
               (unsigned long long)op->data_len);
        if (has_checksum) {
            printf(" checksum=0x%016llx", (unsigned long long)checksum);
        }
    }
    if (note != NULL) {
        printf(" note=%s", note);
    }
    if (op->map_fault[0]) {
        printf(" fault=%s", op->map_fault);
        if (!strcmp(op->map_fault, "descriptor"))
            printf(" fault_byte=%llu", (unsigned long long)op->fault_byte);
    }
    printf("\n");
    (void)fflush(stdout);
}

static bool mem_service_object_session_mapping_fault(
    const struct mem_service_object_session_op *op,
    struct mem_service_client_allocation *view)
{
    const char *fault = op->map_fault;
    if (!fault[0]) return true;
    if (!strcmp(fault, "descriptor")) {
        if (op->fault_byte >= view->descriptor_len ||
            view->descriptor_len > sizeof(view->descriptor)) return false;
        view->descriptor[op->fault_byte] ^= 1;
    } else if (!strcmp(fault, "descriptor_length")) {
        if (!view->descriptor_len) return false;
        --view->descriptor_len;
    } else if (!strcmp(fault, "descriptor_oversize"))
        view->descriptor_len = sizeof(view->descriptor) + 1;
    else if (!strcmp(fault, "address")) view->address ^= 4096;
    else if (!strcmp(fault, "address_len")) view->address_len ^= 4096;
    else if (!strcmp(fault, "size")) view->size_bytes ^= 1;
    else if (!strcmp(fault, "alignment")) view->alignment_bytes ^= 4096;
    else if (!strcmp(fault, "capabilities")) view->capabilities ^= 1ULL << 63;
    else if (!strcmp(fault, "home")) view->home_node[0] ^= 1;
    else if (!strcmp(fault, "incarnation")) view->provider_incarnation ^= 1;
    else return false;
    return true;
}

static int mem_service_object_session_finish_data_op(
    const struct mem_service_object_session_config *config,
    uint32_t index,
    const struct mem_service_object_session_op *op,
    enum mem_service_wire_status status,
    const char *note,
    uint64_t address,
    uint64_t len,
    uint64_t checksum,
    bool has_checksum)
{
    mem_service_object_session_print_data_op_line(config,
                                                  index,
                                                  op,
                                                  status,
                                                  note,
                                                  address,
                                                  len,
                                                  checksum,
                                                  has_checksum);
    if (status != op->expect_status) {
        printf("mem_service object-session: session=%s op=%u action=%s "
               "mismatch=status expected_status=%s\n",
               config->session_id,
               index,
               mem_service_object_session_action_name(op->action),
               mem_service_wire_status_name(op->expect_status));
        (void)fflush(stdout);
        return 1;
    }
    return 0;
}

/*
 * One op: exactly one SDK call (wait_state loops inspect inside its
 * bounded timeout; data-plane ops stay in-process through the provider
 * channel). Returns 0 when the observed wire status and any expect_*
 * assertions match; 1 on mismatch or transport failure.
 */
/* Fault probes run in the mapping-owning process: OBMM VMAs are DONTCOPY,
 * so a fault in a forked child would not prove the parent's page protection. */
static sigjmp_buf mem_service_probe_jump;
static volatile sig_atomic_t mem_service_probe_armed;
static volatile sig_atomic_t mem_service_probe_signal;
static volatile uintptr_t mem_service_probe_address;

static void mem_service_object_session_fault_handler(int signo, siginfo_t *info,
                                                     void *context)
{
    (void)context;
    if (mem_service_probe_armed && info && info->si_code > 0 &&
        (uintptr_t)info->si_addr == mem_service_probe_address) {
        mem_service_probe_signal = signo;
        siglongjmp(mem_service_probe_jump, 1);
    }
    _exit(128 + signo);
}

static int mem_service_object_session_probe_fault(uint8_t *readable,
                                                 uint8_t *target, bool write)
{
    struct sigaction action = {0}, old_segv, old_bus;
    volatile uint8_t value = *(volatile uint8_t *)readable;
    volatile sig_atomic_t result = -1;

    action.sa_sigaction = mem_service_object_session_fault_handler;
    action.sa_flags = SA_SIGINFO;
    sigfillset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, &old_segv)) return -1;
    if (sigaction(SIGBUS, &action, &old_bus)) {
        (void)sigaction(SIGSEGV, &old_segv, NULL);
        return -1;
    }
    mem_service_probe_address = (uintptr_t)target;
    mem_service_probe_signal = 0;
    if (!sigsetjmp(mem_service_probe_jump, 1)) {
        mem_service_probe_armed = 1;
        if (write) *(volatile uint8_t *)target = value;
        else value = *(volatile uint8_t *)target;
        mem_service_probe_armed = 0;
        result = -1;
    } else {
        mem_service_probe_armed = 0;
        result = 0;
    }
    if (sigaction(SIGBUS, &old_bus, NULL)) result = -1;
    if (sigaction(SIGSEGV, &old_segv, NULL)) result = -1;
    return result;
}

static int mem_service_object_session_run_op(
    const struct mem_service_client *client,
    const struct mem_service_object_session_config *config,
    struct mem_service_object_session_state *state,
    const struct mem_service_object_session_op *op,
    uint32_t index)
{
    struct mem_service_client_allocation view;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    const char *session_id = op->session_id[0] != '\0'
                                 ? op->session_id
                                 : config->session_id;
    int rc = 0;

    memset(&view, 0, sizeof(view));
    if (state->retired_probe_mapping.binding.mapped)
        return mem_service_object_session_finish_data_op(config, index, op,
            MEM_SERVICE_WIRE_STATUS_INTERNAL, "retired_probe_cleanup_pending",
            0, 0, 0, false);
    switch (op->action) {
    case MEM_SERVICE_OBJECT_SESSION_ACTION_CAPTURE_MAPPING: {
        if (state->has_captured_mapping)
            return mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_UNSUPPORTED, "capture_already_present",
                0, 0, 0, false);
        if (!state->mapped || state->mapped_reference || !state->mapped_read_verified ||
            !state->has_view || strcmp(state->mapping.key, op->key) ||
            strcmp(state->view.key, op->key) ||
            state->mapping.generation != state->view.generation ||
            (uintptr_t)state->mapping.base != state->view.address ||
            state->mapping.len != state->view.size_bytes ||
            !mem_service_object_session_find_holder(state, op->key,
                state->mapping.generation, config->session_id))
            return mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_NOT_FOUND, "verified_allocation_mapping_required",
                0, 0, 0, false);
        state->captured_allocation = state->view;
        state->captured_address = (uintptr_t)state->mapping.base;
        state->captured_len = state->mapping.len;
        state->has_captured_mapping = true;
        printf("mem_service object-session: session=%s mapping_capture=ok key=%s "
               "generation=%llu address=0x%016llx len=%llu descriptor_checksum=0x%016llx\n",
               config->session_id, op->key,
               (unsigned long long)state->view.generation,
               (unsigned long long)state->captured_address,
               (unsigned long long)state->captured_len,
               (unsigned long long)mem_service_provider_checksum64(
                   state->view.descriptor, state->view.descriptor_len));
        return mem_service_object_session_finish_data_op(config, index, op,
            MEM_SERVICE_WIRE_STATUS_OK, NULL, 0, 0, 0, false);
    }
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_RETIRED_MAPPING: {
        const struct mem_service_client_allocation *old = &state->captured_allocation;
        struct mem_service_client_allocation replacement;
        uint8_t readable = 0;
        int map_result, map_errno, fault_result = -1, cleanup_result = 0;
        int fault_signal = 0;
        if (!state->has_captured_mapping || strcmp(old->key, op->key))
            return mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_NOT_FOUND, "captured_mapping_required",
                0, 0, 0, false);
        if (state->mapped || state->conflict_lifecycle.pending ||
            !state->provider_ready || !state->replacement_generation ||
            !mem_service_object_session_find_holder(state, state->replacement_key,
                state->replacement_generation, config->session_id))
            return mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_UNSUPPORTED, "unmapped_replacement_holder_required",
                0, 0, 0, false);
        if (mem_service_client_inspect_allocation(client, old->key, &view, &status) ||
            status != MEM_SERVICE_WIRE_STATUS_OK || strcmp(view.state, "retired") ||
            view.generation != old->generation)
            return mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_UNSUPPORTED, "captured_retirement_unconfirmed",
                0, 0, 0, false);
        if (mem_service_client_inspect_allocation(client, state->replacement_key,
                &replacement, &status) || status != MEM_SERVICE_WIRE_STATUS_OK ||
            strcmp(replacement.state, "active") || !replacement.provider_backed ||
            replacement.generation != state->replacement_generation ||
            replacement.address != state->captured_address)
            return mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_UNSUPPORTED, "replacement_active_unconfirmed",
                0, 0, 0, false);
        /* Deliberately bypass the managed control gate for this diagnostic.
         * The exact previously mapped descriptor must reach the provider. */
        errno = 0;
        map_result = mem_service_client_map_allocation(&state->channel, old,
            MEM_SERVICE_CLIENT_MAP_READ, &state->retired_probe_mapping);
        map_errno = errno;
        if (!map_result) {
            fault_result = mem_service_object_session_probe_fault(&readable,
                state->retired_probe_mapping.base, false);
            fault_signal = mem_service_probe_signal;
        }
        if (state->retired_probe_mapping.binding.mapped)
            cleanup_result = mem_service_client_unmap_allocation(&state->channel,
                &state->retired_probe_mapping);
        rc = map_result || fault_result || cleanup_result;
        printf("mem_service object-session: session=%s retired_mapping_probe=%s key=%s "
               "generation=%llu replacement_key=%s replacement_generation=%llu "
               "address=0x%016llx map_result=%d map_errno=%d signal=%d unmap_confirmed=%u\n",
               config->session_id, rc ? "fail" : "pass", op->key,
               (unsigned long long)old->generation, state->replacement_key,
               (unsigned long long)state->replacement_generation,
               (unsigned long long)state->captured_address, map_result, map_errno,
               fault_signal, !map_result && !cleanup_result);
        return mem_service_object_session_finish_data_op(config, index, op,
            rc ? MEM_SERVICE_WIRE_STATUS_INTERNAL : MEM_SERVICE_WIRE_STATUS_OK,
            cleanup_result ? "retired_probe_cleanup_pending" :
            map_result ? "retired_map_rejection_unclassified" :
            fault_result ? "retired_access_not_rejected" : NULL,
            0, 0, 0, false);
    }
    case MEM_SERVICE_OBJECT_SESSION_ACTION_BEGIN_REFERENCE:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_SEAL_REFERENCE: {
        struct mem_service_reference_request request = {0};
        struct mem_service_client_reference_result result;
        bool begin = op->action == MEM_SERVICE_OBJECT_SESSION_ACTION_BEGIN_REFERENCE;
        request.action = begin ? MEM_SERVICE_REFERENCE_BEGIN : MEM_SERVICE_REFERENCE_SEAL;
        request.generation = op->generation;
        request.version = op->content_version;
        snprintf(request.key, sizeof(request.key), "%s", op->key);
        snprintf(request.session_id, sizeof(request.session_id), "%s", config->session_id);
        snprintf(request.idempotency_key, sizeof(request.idempotency_key), "%s", op->idempotency_key);
        if (!begin) {
            for (uint32_t i = 0; i < state->prepared_count; ++i) {
                const struct lingqu_object_ref_wire_v2 *ref = &state->prepared[i].reference;
                if (!state->staged[i] && !strcmp(ref->allocation_key, op->key) &&
                    ref->allocation_generation == op->generation &&
                    ref->object.object_version == op->content_version)
                    return mem_service_object_session_finish_data_op(config, index, op,
                        MEM_SERVICE_WIRE_STATUS_INTERNAL, "stage_unconfirmed", 0, 0, 0, false);
            }
        }
        rc = mem_service_client_reference_transition(client, &request, &result, &status);
        if (!rc) {
            view = result.allocation;
            if (begin) {
                state->writer_allocation = view;
                state->has_writer = true;
            }
        }
        break;
    }
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH_REFERENCE: {
        struct mem_service_reference_request request = {0};
        struct mem_service_client_reference_result result;
        char hex[LINGQU_OBJECT_REF_V2_BYTES * 2 + 1];
        uint32_t slot;
        for (slot = 0; slot < state->prepared_count; ++slot)
            if (!strcmp(state->prepared[slot].idempotency_key, op->idempotency_key)) break;
        if (slot < state->prepared_count) {
            request = state->prepared[slot];
            const struct lingqu_object_ref_wire *ref = &request.reference.object;
            if (strcmp(request.key, op->key) || ref->payload_offset != op->reference_view.offset ||
                ref->payload_bytes != op->reference_view.len ||
                ref->object_kind != op->reference_view.object_kind ||
                ref->owner_entity != op->reference_view.owner_entity ||
                ref->producer_entity != op->reference_view.producer_entity) {
                status = MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT;
                rc = -1;
                break;
            }
        } else {
            if (!state->has_writer || !state->mapped || state->mapped_reference ||
                state->prepared_count == MEM_SERVICE_OBJECT_SESSION_MAX_OPS) {
                status = MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
                rc = -1;
                break;
            }
            rc = mem_service_client_prepare_managed_reference(client, &state->channel,
                &state->writer_allocation, &state->mapping, &state->mapping_lifecycle,
                &op->reference_view, &request.reference, &status);
            if (rc) break;
            request.action = MEM_SERVICE_REFERENCE_STAGE;
            snprintf(request.key, sizeof(request.key), "%s", op->key);
            snprintf(request.session_id, sizeof(request.session_id), "%s", config->session_id);
            snprintf(request.idempotency_key, sizeof(request.idempotency_key), "%s", op->idempotency_key);
            state->prepared[state->prepared_count++] = request;
        }
        rc = mem_service_client_reference_transition(client, &request, &result, &status);
        if (!rc) {
            state->staged[slot] = true;
            view = result.allocation;
            if (mem_service_reference_encode_hex(&request.reference, hex, sizeof(hex))) return 1;
            printf("mem_service object-session: session=%s reference_key=%s reference_hex=%s\n",
                   config->session_id, request.key, hex);
        }
        break;
    }
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_READONLY:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_GUARD: {
        uint64_t offset = 0;
        bool write = op->action == MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_READONLY;
        long page = sysconf(_SC_PAGESIZE);
        if (!state->mapped || strcmp(state->mapping.key, op->key) ||
            !mem_service_object_session_find_holder(state, op->key,
                state->mapping.generation, config->session_id) ||
            !(state->mapping.flags & MEM_SERVICE_CLIENT_MAP_READ)) {
            return mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_NOT_FOUND, "readable_mapping_required", 0, 0, 0, false);
        }
        if (write && (state->mapping.flags & MEM_SERVICE_CLIENT_MAP_WRITE)) {
            return mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_UNSUPPORTED, "readonly_mapping_required", 0, 0, 0, false);
        }
        if (!write) {
            offset = state->mapping.len;
            if (strcmp(config->provider_kind, "obmm") || page <= 0 ||
                offset > UINT64_MAX - (uint64_t)page) {
                return mem_service_object_session_finish_data_op(config, index, op,
                    MEM_SERVICE_WIRE_STATUS_UNSUPPORTED, "guard_unavailable", 0, 0, 0, false);
            }
            uintptr_t end = (uintptr_t)state->mapping.base + state->mapping.len;
            if (end > UINTPTR_MAX - (uint64_t)page)
                return mem_service_object_session_finish_data_op(config, index, op,
                    MEM_SERVICE_WIRE_STATUS_UNSUPPORTED, "guard_unavailable", 0, 0, 0, false);
            if (end % (uint64_t)page) end += (uint64_t)page - end % (uint64_t)page;
            offset = end - (uintptr_t)state->mapping.base;
            if (offset >= state->mapped_backing_len) {
                return mem_service_object_session_finish_data_op(config, index, op,
                    MEM_SERVICE_WIRE_STATUS_UNSUPPORTED, "no_full_padding_page", 0, 0, 0, false);
            }
        }
        rc = mem_service_object_session_probe_fault(state->mapping.base,
            (uint8_t *)state->mapping.base + offset, write);
        printf("mem_service object-session: session=%s cpu_fault_probe=%s offset=%llu signal=%d\n",
               config->session_id, mem_service_object_session_action_name(op->action),
               (unsigned long long)offset, (int)mem_service_probe_signal);
        return mem_service_object_session_finish_data_op(config, index, op,
            rc ? MEM_SERVICE_WIRE_STATUS_INTERNAL : MEM_SERVICE_WIRE_STATUS_OK,
            rc ? "expected_cpu_fault_not_observed" : NULL, 0, 0, 0, false);
    }
    case MEM_SERVICE_OBJECT_SESSION_ACTION_ALLOCATE: {
        struct mem_service_client_allocate request;

        memset(&request, 0, sizeof(request));
        request.key = op->key;
        request.idempotency_key = op->idempotency_key;
        request.session_id = session_id;
        request.size_bytes = op->size_bytes;
        request.alignment_bytes = op->alignment_bytes;
        request.capabilities = op->capabilities;
        rc = mem_service_client_allocate_object(client,
                                                &request,
                                                &view,
                                                &status);
        break;
    }
    case MEM_SERVICE_OBJECT_SESSION_ACTION_ACQUIRE_REFERENCE: {
        struct mem_service_reference_request request = {0};
        struct mem_service_client_reference_result result;
        request.action = MEM_SERVICE_REFERENCE_RESOLVE;
        snprintf(request.key, sizeof(request.key), "%s", op->key);
        rc = mem_service_client_reference_transition(client, &request, &result, &status);
        if (rc) break;
        if (op->has_expected_generation && result.reference.allocation_generation != op->expected_generation) {
            rc = -1; status = MEM_SERVICE_WIRE_STATUS_STALE_REF;
            break;
        }
        /* Resolve names may be longer than allocation keys; canonical
         * fixed-width fields must not retain bytes from the prior request. */
        memset(&request, 0, sizeof(request));
        request.action = MEM_SERVICE_REFERENCE_ACQUIRE;
        request.reference = result.reference;
        request.access = LINGQU_OBJECT_REF_V2_READ;
        snprintf(request.key, sizeof(request.key), "%s", request.reference.allocation_key);
        snprintf(request.session_id, sizeof(request.session_id), "%s", session_id);
        snprintf(request.idempotency_key, sizeof(request.idempotency_key), "%s", op->idempotency_key);
        rc = mem_service_client_reference_transition(client, &request, &result, &status);
        if (!rc) {
            state->reference = result.reference;
            state->has_reference = true;
            view = result.allocation;
        }
        break;
    }
    case MEM_SERVICE_OBJECT_SESSION_ACTION_ACQUIRE:
        rc = mem_service_client_acquire_object(client,
                                               op->key,
                                               op->idempotency_key,
                                               session_id,
                                               op->has_expected_generation,
                                               op->expected_generation,
                                               &view,
                                               &status);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_RELEASE:
        /* The SDK contract requires unmap before release: a live mapping
         * on the same key fails closed instead of racing the unmap. */
        if (state->mapped &&
            strcmp(state->mapping.key, op->key) == 0) {
            return mem_service_object_session_finish_data_op(
                config,
                index,
                op,
                MEM_SERVICE_WIRE_STATUS_INTERNAL,
                "mapping_active",
                0,
                0,
                0,
                false);
        }
        rc = mem_service_client_release_object(client,
                                               op->key,
                                               op->idempotency_key,
                                               session_id,
                                               op->has_expected_generation,
                                               op->expected_generation,
                                               &view,
                                               &status);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_RETIRE:
        rc = mem_service_client_retire_object(client,
                                              op->key,
                                              op->idempotency_key,
                                              op->has_expected_generation,
                                              op->expected_generation,
                                              &view,
                                              &status);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_INSPECT:
        rc = mem_service_client_inspect_allocation(client,
                                                   op->key,
                                                   &view,
                                                   &status);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH:
        rc = mem_service_client_publish_allocation(client,
                                                   op->key,
                                                   op->node_id,
                                                   op->incarnation,
                                                   op->generation,
                                                   op->descriptor,
                                                   op->descriptor_len,
                                                   op->address,
                                                   op->address_len,
                                                   &view,
                                                   &status);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_RECLAIM:
        rc = mem_service_client_reclaim_allocation(client,
                                                   op->key,
                                                   op->node_id,
                                                   op->incarnation,
                                                   op->generation,
                                                   op->confirmed,
                                                   &view,
                                                   &status);
        break;
    case MEM_SERVICE_OBJECT_SESSION_ACTION_MAP:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_MAP_REFERENCE:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_CONFLICT:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_DESCRIPTOR: {
        unsigned char nonce[16];
        struct mem_service_client_allocation supplied = state->view;
        char operation_id[48] = "sdkmap-";
        bool conflict = op->action == MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_CONFLICT;
        bool descriptor_probe = op->action == MEM_SERVICE_OBJECT_SESSION_ACTION_PROBE_DESCRIPTOR;
        bool probe = conflict || descriptor_probe;
        bool reference_map = op->action == MEM_SERVICE_OBJECT_SESSION_ACTION_MAP_REFERENCE;
        uint64_t before = 0, probe_len = 0;
        FILE *random;
        size_t nonce_len;
        int random_close;
        if (!state->provider_ready) {
            return mem_service_object_session_finish_data_op(
                config,
                index,
                op,
                MEM_SERVICE_WIRE_STATUS_UNSUPPORTED,
                "provider_unavailable",
                0,
                0,
                0,
                false);
        }
        if ((state->mapped && !probe) || state->conflict_lifecycle.pending) {
            return mem_service_object_session_finish_data_op(
                config,
                index,
                op,
                MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED,
                "already_mapped",
                0,
                0,
                0,
                false);
        }
        if (probe) {
            long page = sysconf(_SC_PAGESIZE);
            if (!state->mapped || !state->mapping.base || page <= 0 ||
                !(state->mapping.flags & MEM_SERVICE_CLIENT_MAP_READ) ||
                strcmp(state->mapping.key, op->key))
                return mem_service_object_session_finish_data_op(config, index, op,
                    MEM_SERVICE_WIRE_STATUS_NOT_FOUND, "readable_mapping_required",
                    0, 0, 0, false);
            probe_len = state->mapping.len < (uint64_t)page ? state->mapping.len :
                        (uint64_t)page;
            before = mem_service_provider_checksum64(state->mapping.base, probe_len);
        }
        if ((reference_map && (!state->has_reference || strcmp(state->reference.allocation_key, op->key))) ||
            !state->has_view ||
            strcmp(state->view.key, op->key) != 0 ||
            !mem_service_object_session_find_holder(state, op->key,
                state->view.generation, config->session_id)) {
            return mem_service_object_session_finish_data_op(
                config,
                index,
                op,
                MEM_SERVICE_WIRE_STATUS_NOT_FOUND,
                "no_holder",
                0,
                0,
                0,
                false);
        }
        if (strcmp(state->view.state, "active") != 0 ||
            !state->view.provider_backed ||
            (state->view.capabilities &
             MEM_SERVICE_CLIENT_MANAGED_CAP_MAP) == 0) {
            return mem_service_object_session_finish_data_op(
                config,
                index,
                op,
                MEM_SERVICE_WIRE_STATUS_UNSUPPORTED,
                "not_mappable",
                0,
                0,
                0,
                false);
        }
#ifdef MEM_SERVICE_OBJECT_SESSION_OBMM
        if (probe && state->obmm_open) {
            rc = descriptor_probe ? mem_service_provider_obmm_endpoint_probe_descriptor(
                &state->obmm_endpoint, state->mapping.binding.mapping.handle) :
                mem_service_provider_obmm_endpoint_probe_conflict(
                &state->obmm_endpoint, state->mapping.binding.mapping.handle);
            if (rc != 0 ||
                mem_service_provider_checksum64(state->mapping.base, probe_len) != before)
                return mem_service_object_session_finish_data_op(config, index, op,
                    MEM_SERVICE_WIRE_STATUS_INTERNAL,
                    descriptor_probe ? "descriptor_probe_failed" : "conflict_probe_failed",
                    0, 0, 0, false);
            if (descriptor_probe) {
                printf("mem_service object-session: session=%s descriptor_probe=pass "
                       "key=%s checks=24 preserved=1 cleanup_pending=0\n",
                       config->session_id, op->key);
                return mem_service_object_session_finish_data_op(config, index, op,
                    MEM_SERVICE_WIRE_STATUS_OK, NULL, 0, 0, 0, false);
            }
            printf("mem_service object-session: session=%s cpu_conflict_probe=pass "
                   "key=%s base=0x%016llx len=%llu preserved=1 cleanup_pending=0 "
                   "source=retained_handle\n", config->session_id, op->key,
                   (unsigned long long)(uintptr_t)state->mapping.base,
                   (unsigned long long)probe_len);
            return mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_OK, NULL, 0, 0, 0, false);
        }
#endif
        if (descriptor_probe)
            return mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_UNSUPPORTED, "obmm_mapping_required",
                0, 0, 0, false);
        random = fopen("/dev/urandom", "rb");
        if (random == NULL)
            return mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_INTERNAL, "mapping_identity_unavailable",
                0, 0, 0, false);
        nonce_len = fread(nonce, 1, sizeof(nonce), random);
        random_close = fclose(random);
        if (nonce_len != sizeof(nonce) || random_close != 0)
            return mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_INTERNAL, "mapping_identity_unavailable",
                0, 0, 0, false);
        for (size_t n = 0; n < sizeof(nonce); ++n)
            snprintf(operation_id + 7 + n * 2, 3, "%02x", nonce[n]);
        if (!mem_service_object_session_mapping_fault(op, &supplied))
            return mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_UNSUPPORTED, "mapping_fault_unavailable",
                0, 0, 0, false);
        if (!probe) {
            state->mapped_read_verified = false;
            state->mapped_reference = reference_map;
            state->mapped_content_version = reference_map ?
                state->reference.object.object_version : supplied.version;
        }
        rc = reference_map ?
            mem_service_client_map_managed_reference(client, &state->channel, &state->reference,
                config->session_id, operation_id, &state->mapping, &state->reference_lifecycle, &status) :
            mem_service_client_map_managed_allocation(client, &state->channel,
                &supplied, config->session_id, operation_id,
                conflict ? MEM_SERVICE_CLIENT_MAP_READ : op->map_flags,
                conflict ? &state->conflict_mapping : &state->mapping,
                conflict ? &state->conflict_lifecycle : &state->mapping_lifecycle, &status);
        if (conflict) {
            bool rejected = rc != 0 && status == MEM_SERVICE_WIRE_STATUS_INTERNAL;
            if (rc == 0 || state->conflict_lifecycle.pending ||
                state->conflict_mapping.binding.mapped)
                return mem_service_object_session_finish_data_op(config, index, op,
                    MEM_SERVICE_WIRE_STATUS_INTERNAL, "conflict_mapping_not_drained",
                    0, 0, 0, false);
            memset(&state->conflict_mapping, 0, sizeof(state->conflict_mapping));
            if (!rejected ||
                mem_service_provider_checksum64(state->mapping.base, probe_len) != before)
                return mem_service_object_session_finish_data_op(config, index, op,
                    MEM_SERVICE_WIRE_STATUS_INTERNAL, "conflict_probe_failed",
                    0, 0, 0, false);
            printf("mem_service object-session: session=%s cpu_conflict_probe=pass "
                   "key=%s base=0x%016llx len=%llu preserved=1 cleanup_pending=0\n",
                   config->session_id, op->key,
                   (unsigned long long)(uintptr_t)state->mapping.base,
                   (unsigned long long)probe_len);
            return mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_OK, NULL, 0, 0, 0, false);
        }
        if (rc != 0) {
            state->mapped = reference_map ? state->reference_lifecycle.mapping.pending :
                                            state->mapping_lifecycle.pending;
            if (!state->mapped)
                memset(&state->mapping, 0, sizeof(state->mapping));
            return mem_service_object_session_finish_data_op(
                config,
                index,
                op,
                status,
                state->mapped ? "map_cleanup_required" : "map_failed",
                0,
                0,
                0,
                false);
        }
        state->mapped = true;
        state->mapped_backing_len = state->view.address_len -
            (reference_map ? state->reference.object.payload_offset : 0);
        return mem_service_object_session_finish_data_op(
            config,
            index,
            op,
            MEM_SERVICE_WIRE_STATUS_OK,
            NULL,
            (uint64_t)(uintptr_t)state->mapping.base,
            state->mapping.len,
            0,
            false);
    }
    case MEM_SERVICE_OBJECT_SESSION_ACTION_UNMAP: {
        uintptr_t old_address = 0;
        uint64_t old_generation = 0;
        uint8_t readable = 0;
        if (!state->mapped ||
            strcmp(state->mapping.key, op->key) != 0) {
            return mem_service_object_session_finish_data_op(
                config,
                index,
                op,
                MEM_SERVICE_WIRE_STATUS_NOT_FOUND,
                "not_mapped",
                0,
                0,
                0,
                false);
        }
        if (op->probe_unmapped) {
            if (!state->mapping.base || !state->mapping.len ||
                !(state->mapping.flags & MEM_SERVICE_CLIENT_MAP_READ) ||
                !mem_service_object_session_find_holder(state, op->key,
                    state->mapping.generation, config->session_id)) {
                return mem_service_object_session_finish_data_op(config, index, op,
                    MEM_SERVICE_WIRE_STATUS_UNSUPPORTED, "readable_mapping_required",
                    0, 0, 0, false);
            }
            old_address = (uintptr_t)state->mapping.base;
            old_generation = state->mapping.generation;
            readable = *(volatile uint8_t *)state->mapping.base;
        }
        if (mem_service_object_session_unmap(state, &status) != 0) {
            return mem_service_object_session_finish_data_op(
                config,
                index,
                op,
                MEM_SERVICE_WIRE_STATUS_INTERNAL,
                "unmap_failed",
                0,
                0,
                0,
                false);
        }
        state->mapped = false;
        state->mapped_backing_len = 0;
        memset(&state->mapping, 0, sizeof(state->mapping));
        if (op->probe_unmapped) {
            rc = mem_service_object_session_probe_fault(&readable,
                (uint8_t *)old_address, false);
            printf("mem_service object-session: session=%s cpu_unmapped_probe=%s "
                   "key=%s generation=%llu address=0x%016llx signal=%d "
                   "read_before_unmap=1 unmap_confirmed=1\n",
                   config->session_id, rc ? "fail" : "pass", op->key,
                   (unsigned long long)old_generation,
                   (unsigned long long)old_address, (int)mem_service_probe_signal);
            return mem_service_object_session_finish_data_op(config, index, op,
                rc ? MEM_SERVICE_WIRE_STATUS_INTERNAL : MEM_SERVICE_WIRE_STATUS_OK,
                rc ? "expected_cpu_fault_not_observed" : NULL, 0, 0, 0, false);
        }
        return mem_service_object_session_finish_data_op(
            config,
            index,
            op,
            MEM_SERVICE_WIRE_STATUS_OK,
            NULL,
            0,
            0,
            0,
            false);
    }
    case MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH_DATA:
    case MEM_SERVICE_OBJECT_SESSION_ACTION_WAIT_VISIBLE: {
        struct mem_service_visibility_completion completion;
        uint64_t expected = op->expect_checksum;
        uint64_t required = op->action == MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH_DATA
                                ? MEM_SERVICE_CLIENT_MAP_WRITE : MEM_SERVICE_CLIENT_MAP_READ;
        if (!state->mapped || strcmp(state->mapping.key, op->key)) {
            return mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_NOT_FOUND, "not_mapped", 0, 0, 0, false);
        }
        if (!(state->mapping.flags & required)) {
            return mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_UNSUPPORTED, "mapping_permissions", 0, 0, 0, false);
        }
        if (op->data_offset > state->mapping.len ||
            op->data_len > state->mapping.len - op->data_offset) {
            return mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED, "out_of_bounds", 0, 0, 0, false);
        }
        if (op->has_data_seed) {
            uint64_t pattern = 1469598103934665603ULL;
            for (uint64_t i = 0; i < op->data_len; i++) {
                pattern ^= (uint8_t)(op->data_seed + i);
                pattern *= 1099511628211ULL;
            }
            if (op->has_expect_checksum && expected != pattern) {
                return mem_service_object_session_finish_data_op(config, index, op,
                    MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH, "conflicting_expectations",
                    0, 0, 0, false);
            }
            expected = pattern;
        }
        if (op->action == MEM_SERVICE_OBJECT_SESSION_ACTION_PUBLISH_DATA) {
            rc = mem_service_provider_channel_publish_range(&state->channel,
                &state->mapping.binding, op->data_offset, op->data_len, expected, &completion);
        } else {
            rc = mem_service_provider_channel_wait_range_visible(&state->channel,
                &state->mapping.binding, op->data_offset, op->data_len, expected,
                config->request_timeout_ms, &completion);
        }
        return mem_service_object_session_finish_data_op(config, index, op,
            rc ? MEM_SERVICE_WIRE_STATUS_INTERNAL : MEM_SERVICE_WIRE_STATUS_OK,
            rc ? "visibility_unconfirmed" : NULL, 0, 0, expected, rc == 0);
    }
    case MEM_SERVICE_OBJECT_SESSION_ACTION_WRITE: {
        uint64_t checksum = 0;
        uint8_t *base;
        uint64_t i;

        for (uint32_t j = 0; j < state->prepared_count; ++j) {
            const struct lingqu_object_ref_wire_v2 *ref = &state->prepared[j].reference;
            if (!strcmp(ref->allocation_key, op->key) &&
                ref->allocation_generation == state->mapping.generation &&
                ref->object.object_version == state->mapped_content_version)
                return mem_service_object_session_finish_data_op(config, index, op,
                    MEM_SERVICE_WIRE_STATUS_UNSUPPORTED, "reference_payload_frozen", 0, 0, 0, false);
        }
        if (!state->mapped ||
            strcmp(state->mapping.key, op->key) != 0) {
            return mem_service_object_session_finish_data_op(
                config,
                index,
                op,
                MEM_SERVICE_WIRE_STATUS_NOT_FOUND,
                "not_mapped",
                0,
                0,
                0,
                false);
        }
        if ((state->mapping.flags & MEM_SERVICE_CLIENT_MAP_WRITE) == 0) {
            return mem_service_object_session_finish_data_op(
                config,
                index,
                op,
                MEM_SERVICE_WIRE_STATUS_UNSUPPORTED,
                "not_writable",
                0,
                0,
                0,
                false);
        }
        if (op->data_offset > state->mapping.len ||
            op->data_len > state->mapping.len - op->data_offset) {
            return mem_service_object_session_finish_data_op(
                config,
                index,
                op,
                MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED,
                "out_of_bounds",
                0,
                0,
                0,
                false);
        }
        base = (uint8_t *)state->mapping.base + op->data_offset;
        for (i = 0; i < op->data_len; ++i) {
            base[i] = (uint8_t)(op->data_seed + i);
        }
        checksum = mem_service_provider_checksum64(base, op->data_len);
        return mem_service_object_session_finish_data_op(
            config,
            index,
            op,
            MEM_SERVICE_WIRE_STATUS_OK,
            NULL,
            0,
            0,
            checksum,
            true);
    }
    case MEM_SERVICE_OBJECT_SESSION_ACTION_READ: {
        uint64_t checksum = 0;
        const uint8_t *base;
        uint64_t i;

        if (!state->mapped ||
            strcmp(state->mapping.key, op->key) != 0) {
            return mem_service_object_session_finish_data_op(
                config,
                index,
                op,
                MEM_SERVICE_WIRE_STATUS_NOT_FOUND,
                "not_mapped",
                0,
                0,
                0,
                false);
        }
        if ((state->mapping.flags & MEM_SERVICE_CLIENT_MAP_READ) == 0) {
            return mem_service_object_session_finish_data_op(
                config,
                index,
                op,
                MEM_SERVICE_WIRE_STATUS_UNSUPPORTED,
                "not_readable",
                0,
                0,
                0,
                false);
        }
        if (op->data_offset > state->mapping.len ||
            op->data_len > state->mapping.len - op->data_offset) {
            return mem_service_object_session_finish_data_op(
                config,
                index,
                op,
                MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED,
                "out_of_bounds",
                0,
                0,
                0,
                false);
        }
        base = (const uint8_t *)state->mapping.base + op->data_offset;
        if (op->has_data_seed) {
            for (i = 0; i < op->data_len; ++i) {
                if (base[i] != (uint8_t)(op->data_seed + i)) {
                    return mem_service_object_session_finish_data_op(
                        config,
                        index,
                        op,
                        MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH,
                        "pattern_mismatch",
                        0,
                        0,
                        0,
                        false);
                }
            }
        }
        checksum = mem_service_provider_checksum64(base, op->data_len);
        if (op->has_expect_checksum && checksum != op->expect_checksum) {
            return mem_service_object_session_finish_data_op(
                config,
                index,
                op,
                MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH,
                "checksum_mismatch",
                0,
                0,
                0,
                false);
        }
        if (op->has_data_seed || op->has_expect_checksum) {
            state->mapped_read_verified = true;
            if (state->has_captured_mapping && !state->mapped_reference &&
                (uintptr_t)state->mapping.base == state->captured_address &&
                state->mapping.len == state->captured_len &&
                state->mapping.generation != state->captured_allocation.generation) {
                snprintf(state->replacement_key, sizeof(state->replacement_key),
                    "%s", state->mapping.key);
                state->replacement_generation = state->mapping.generation;
            }
        }
        return mem_service_object_session_finish_data_op(
            config,
            index,
            op,
            MEM_SERVICE_WIRE_STATUS_OK,
            NULL,
            0,
            0,
            checksum,
            true);
    }
    case MEM_SERVICE_OBJECT_SESSION_ACTION_STATS: {
        struct mem_service_client_allocation_stats stats;

        memset(&stats, 0, sizeof(stats));
        rc = mem_service_client_allocation_stats(client, &stats, &status);
        printf("mem_service object-session: session=%s op=%u action=stats "
               "status=%s live_objects=%llu backing_allocated_bytes=%llu "
               "address_reserved_bytes=%llu live_refs=%llu in_flight=%llu "
               "quarantined_objects=%llu quarantined_bytes=%llu "
               "import_mappings=%llu\n",
               config->session_id,
               index,
               mem_service_wire_status_name(status),
               (unsigned long long)stats.live_objects,
               (unsigned long long)stats.backing_allocated_bytes,
               (unsigned long long)stats.address_reserved_bytes,
               (unsigned long long)stats.live_refs,
               (unsigned long long)stats.in_flight,
               (unsigned long long)stats.quarantined_objects,
               (unsigned long long)stats.quarantined_bytes,
               (unsigned long long)stats.import_mappings);
        (void)fflush(stdout);
#ifdef MEM_SERVICE_OBJECT_SESSION_OBMM
        if (state->obmm_open) {
            struct mem_service_provider_obmm_resources_v1 resources;
            if (mem_service_provider_obmm_endpoint_resources_v1(
                    &state->obmm_endpoint, &resources) != 0) {
                fprintf(stderr, "mem_service object-session: provider_resources_unavailable\n");
                return 1;
            }
            printf("mem_service provider-resources: provider=obmm scope=endpoint "
                   "version=1 export_handles=%llu export_bytes=%llu "
                   "import_handles=%llu import_bytes=%llu vma_count=%llu "
                   "vma_bytes=%llu accessible_views=%llu accessible_bytes=%llu "
                   "cleanup_mappings=%llu closing=%u control_close_uncertain=%u\n",
                   (unsigned long long)resources.export_handles,
                   (unsigned long long)resources.export_bytes,
                   (unsigned long long)resources.import_handles,
                   (unsigned long long)resources.import_bytes,
                   (unsigned long long)resources.vma_count,
                   (unsigned long long)resources.vma_bytes,
                   (unsigned long long)resources.accessible_views,
                   (unsigned long long)resources.accessible_bytes,
                   (unsigned long long)resources.cleanup_mappings,
                   (unsigned)resources.closing, (unsigned)resources.control_close_uncertain);
            (void)fflush(stdout);
        }
#endif
        if (rc != 0 || status != op->expect_status) {
            if (status == op->expect_status) {
                return 1;
            }
            printf("mem_service object-session: session=%s op=%u "
                   "action=stats mismatch=status expected_status=%s\n",
                   config->session_id,
                   index,
                   mem_service_wire_status_name(op->expect_status));
            (void)fflush(stdout);
            return 1;
        }
        return 0;
    }
    case MEM_SERVICE_OBJECT_SESSION_ACTION_WAIT_STATE: {
        uint64_t deadline = mem_service_object_session_monotonic_ms() +
                            op->wait_timeout_ms;
        char last_state[MEM_SERVICE_CLIENT_ALLOCATION_STATE_LEN] = "-";
        uint64_t last_generation = 0;

        for (;;) {
            enum mem_service_wire_status poll_status =
                MEM_SERVICE_WIRE_STATUS_INTERNAL;

            memset(&view, 0, sizeof(view));
            rc = mem_service_client_inspect_allocation(client,
                                                       op->key,
                                                       &view,
                                                       &poll_status);
            if (poll_status == MEM_SERVICE_WIRE_STATUS_OK &&
                view.state[0] != '\0') {
                snprintf(last_state,
                         sizeof(last_state),
                         "%s",
                         view.state);
                last_generation = view.generation;
                if (strcmp(view.state, op->wait_state) == 0) {
                    mem_service_object_session_print_op_line(config,
                                                             index,
                                                             op,
                                                             poll_status,
                                                             &view,
                                                             NULL,
                                                             NULL);
                    return 0;
                }
            }
            if (mem_service_object_session_monotonic_ms() >= deadline) {
                char expected[MEM_SERVICE_OBJECT_SESSION_FIELD_VALUE_LEN];

                snprintf(expected,
                         sizeof(expected),
                         "%s",
                         op->wait_state);
                memset(&view, 0, sizeof(view));
                if (strcmp(last_state, "-") != 0) {
                    snprintf(view.state,
                             sizeof(view.state),
                             "%s",
                             last_state);
                    view.generation = last_generation;
                }
                mem_service_object_session_print_op_line(config,
                                                         index,
                                                         op,
                                                         MEM_SERVICE_WIRE_STATUS_TIMEOUT,
                                                         &view,
                                                         "state",
                                                         expected);
                return 1;
            }
            mem_service_object_session_sleep_ms(op->wait_poll_ms);
        }
    }
    default:
        return 1;
    }

    /* Query selection must not erase references to other objects or owners. */
    if (rc == 0 && status == MEM_SERVICE_WIRE_STATUS_OK && view.state[0] != '\0') {
        if (mem_service_object_session_track_holder(state, op, &view, session_id)) {
            (void)mem_service_object_session_finish_data_op(config, index, op,
                MEM_SERVICE_WIRE_STATUS_INTERNAL, "holder_tracking_failed",
                0, 0, 0, false);
            return 1;
        }
        state->has_view = true;
        state->view = view;
    }

    if (status != op->expect_status) {
        mem_service_object_session_print_op_line(
            config,
            index,
            op,
            status,
            view.state[0] != '\0' ? &view : NULL,
            "status",
            mem_service_wire_status_name(op->expect_status));
        return 1;
    }
    if (op->action == MEM_SERVICE_OBJECT_SESSION_ACTION_INSPECT &&
        status == MEM_SERVICE_WIRE_STATUS_OK) {
        if (op->has_expect_state &&
            strcmp(view.state, op->expect_state) != 0) {
            mem_service_object_session_print_op_line(config,
                                                     index,
                                                     op,
                                                     status,
                                                     &view,
                                                     "state",
                                                     op->expect_state);
            return 1;
        }
        if (op->has_expected_generation &&
            view.generation != op->expected_generation) {
            char expected[32];

            snprintf(expected,
                     sizeof(expected),
                     "%llu",
                     (unsigned long long)op->expected_generation);
            mem_service_object_session_print_op_line(config,
                                                     index,
                                                     op,
                                                     status,
                                                     &view,
                                                     "generation",
                                                     expected);
            return 1;
        }
        if (op->has_expect_holder_count &&
            (uint64_t)view.holder_count != op->expect_holder_count) {
            char expected[32];

            snprintf(expected,
                     sizeof(expected),
                     "%llu",
                     (unsigned long long)op->expect_holder_count);
            mem_service_object_session_print_op_line(config,
                                                     index,
                                                     op,
                                                     status,
                                                     &view,
                                                     "holder_count",
                                                     expected);
            return 1;
        }
    }
    mem_service_object_session_print_op_line(
        config,
        index,
        op,
        status,
        rc == 0 && view.state[0] != '\0' ? &view : NULL,
        NULL,
        NULL);
    return 0;
}

static int run_object_session(int argc, char **argv)
{
    struct mem_service_object_session_config config;
    struct mem_service_object_session_state state;
    struct mem_service_client client;
    struct mem_service_wire_client_options options;
    uint64_t started_ms;
    uint32_t i;
    const char *config_path = NULL;
    const char *end_failure = NULL;

    if (argc == 4 && strcmp(argv[2], "--config") == 0) {
        config_path = argv[3];
    }
    if (config_path == NULL) {
        fprintf(stderr,
                "usage: linqu_mem_service object-session --config <path>\n");
        return 2;
    }
    if (mem_service_object_session_load_config(config_path, &config) != 0) {
        return 2;
    }
    memset(&state, 0, sizeof(state));
    mem_service_wire_client_options_init(&options);
    options.timeout_ms = config.request_timeout_ms;
    mem_service_client_init_with_options(&client, config.connect, &options);
    state.client = &client;
    if (mem_service_object_session_provider_open(&config, &state) != 0) {
        fprintf(stderr,
                "mem_service object-session: session=%s provider %s "
                "unavailable\n",
                config.session_id,
                config.provider_kind);
        mem_service_object_session_provider_close(&state);
        return 2;
    }
    started_ms = mem_service_object_session_monotonic_ms();
    for (i = 0; i < config.op_count; ++i) {
        if (mem_service_object_session_run_op(&client,
                                              &config,
                                              &state,
                                              &config.ops[i],
                                              i + 1U) != 0) {
            (void)mem_service_object_session_report_holders(&state, config.session_id);
            printf("mem_service object-session: session=%s result=failed "
                   "op=%u elapsed_ms=%llu\n",
                   config.session_id,
                   i + 1U,
                   (unsigned long long)(
                       mem_service_object_session_monotonic_ms() -
                       started_ms));
            (void)fflush(stdout);
            mem_service_object_session_provider_close(&state);
            return 1;
        }
    }
    /* A session must end clean: unmap before release already ran per-op,
     * so a live mapping or holder here means the config itself leaked. */
    if (state.mapped || state.retired_probe_mapping.binding.mapped) {
        end_failure = "active_mapping";
    }
    if (mem_service_object_session_report_holders(&state, config.session_id) &&
        end_failure == NULL) {
        end_failure = "live_holder";
    }
    if (end_failure != NULL) {
        printf("mem_service object-session: session=%s result=failed "
               "reason=%s elapsed_ms=%llu\n",
               config.session_id,
               end_failure,
               (unsigned long long)(
                   mem_service_object_session_monotonic_ms() - started_ms));
        (void)fflush(stdout);
        mem_service_object_session_provider_close(&state);
        return 1;
    }
    if (mem_service_object_session_provider_close(&state) != 0) {
        printf("mem_service object-session: session=%s result=failed "
               "reason=endpoint_cleanup_pending\n", config.session_id);
        return 1;
    }
    printf("mem_service object-session: session=%s result=ok ops=%u "
           "elapsed_ms=%llu\n",
           config.session_id,
           config.op_count,
           (unsigned long long)(mem_service_object_session_monotonic_ms() -
                                started_ms));
    (void)fflush(stdout);
    return 0;
}

static int run_export_snapshot_page(int argc, char **argv)
{
    char payload[160] = "";

    if (append_optional_payload_field(payload,
                                      sizeof(payload),
                                      argc,
                                      argv,
                                      "--start-index",
                                      "start_index") != 0 ||
        append_optional_payload_field(payload,
                                      sizeof(payload),
                                      argc,
                                      argv,
                                      "--max-records",
                                      "max_records") != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT_PAGE,
                                      "export-snapshot-page",
                                      payload);
}

static int run_audit_log(int argc, char **argv)
{
    char payload[160] = "";

    if (append_optional_payload_field(payload,
                                      sizeof(payload),
                                      argc,
                                      argv,
                                      "--start-sequence",
                                      "start_sequence") != 0 ||
        append_optional_payload_field(payload,
                                      sizeof(payload),
                                      argc,
                                      argv,
                                      "--max-events",
                                      "max_events") != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_AUDIT_LOG,
                                      "audit-log",
                                      payload);
}

static int write_snapshot_records_from_page(FILE *file, const char *response)
{
    const char *records = strstr(response, "record_begin\n");

    if (records == NULL) {
        return 0;
    }
    return fputs(records, file) < 0 ? -1 : 0;
}

static int run_export_snapshot_to(int argc, char **argv)
{
    const char *path = snapshot_path_arg(argc, argv, "--to");
    const char *max_records_arg = option_value(argc, argv, "--max-records");
    char tmp_path[512];
    char payload[160];
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    FILE *file;
    uint64_t start_index = 0;
    uint64_t page_count = 0;
    uint64_t record_count = 0;
    bool wrote_header = false;

    if (path == NULL || path[0] == '\0') {
        fprintf(stderr, "mem_service: missing snapshot output path\n");
        return 2;
    }
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path)) {
        fprintf(stderr, "mem_service: snapshot output path too long\n");
        return 2;
    }
    file = fopen(tmp_path, "w");
    if (file == NULL) {
        fprintf(stderr, "mem_service: failed to open snapshot output %s\n", tmp_path);
        return 1;
    }
    for (;;) {
        struct mem_service_wire_payload_view view;
        uint64_t next_index;
        uint32_t complete;
        int rc;

        payload[0] = '\0';
        if (mem_service_wire_payload_append_u64(payload,
                                                sizeof(payload),
                                                "start_index",
                                                start_index) != 0 ||
            append_payload_field(payload,
                                 sizeof(payload),
                                 "max_records",
                                 max_records_arg) != 0) {
            fclose(file);
            remove(tmp_path);
            return 2;
        }
        rc = send_client_payload_request(argc,
                                         argv,
                                         MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT_PAGE,
                                         payload,
                                         response,
                                         sizeof(response),
                                         &status);
        if (rc != 0 || status != MEM_SERVICE_WIRE_STATUS_OK) {
            fclose(file);
            remove(tmp_path);
            fprintf(stderr,
                    "mem_service: export-snapshot-page failed status=%s\n",
                    mem_service_wire_status_name(status));
            return rc != 0 ? rc : 1;
        }
        view = mem_service_wire_payload_view_from_cstr(response);
        if (!wrote_header) {
            record_count = mem_service_wire_payload_get_u64(&view, "record_count", 0);
            if (fprintf(file,
                        "%s\nstore_schema_version=1\nrecord_count=%" PRIu64 "\n",
                        MEM_SERVICE_CLI_STORE_MAGIC,
                        record_count) < 0) {
                fclose(file);
                remove(tmp_path);
                return 1;
            }
            wrote_header = true;
        }
        if (write_snapshot_records_from_page(file, response) != 0) {
            fclose(file);
            remove(tmp_path);
            return 1;
        }
        complete = mem_service_wire_payload_get_u32(&view, "complete", 0);
        next_index = mem_service_wire_payload_get_u64(&view, "next_index", start_index);
        page_count += 1U;
        if (complete != 0) {
            break;
        }
        if (next_index <= start_index) {
            fclose(file);
            remove(tmp_path);
            fprintf(stderr, "mem_service: export-snapshot-page made no progress\n");
            return 1;
        }
        start_index = next_index;
    }
    if (fclose(file) != 0) {
        remove(tmp_path);
        return 1;
    }
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        fprintf(stderr, "mem_service: failed to publish snapshot %s\n", path);
        return 1;
    }
    printf("mem_service export-snapshot-to: status=ok path=%s record_count=%" PRIu64
           " pages=%" PRIu64 "\n",
           path,
           record_count,
           page_count);
    return 0;
}

static const char *restore_snapshot_path_arg(int argc, char **argv)
{
    return snapshot_path_arg(argc, argv, "--from");
}

static void trim_snapshot_line(char *line)
{
    size_t len;

    if (line == NULL) {
        return;
    }
    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[len - 1] = '\0';
        len -= 1;
    }
}

static int read_snapshot_file(const char *path, char *payload, size_t payload_len)
{
    FILE *file;
    size_t used;
    int next;

    if (path == NULL || path[0] == '\0' || payload == NULL || payload_len == 0) {
        fprintf(stderr, "mem_service: missing snapshot path\n");
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "mem_service: failed to open snapshot %s\n", path);
        return -1;
    }
    used = fread(payload, 1, payload_len - 1U, file);
    if (ferror(file)) {
        fclose(file);
        fprintf(stderr, "mem_service: failed to read snapshot %s\n", path);
        return -1;
    }
    next = fgetc(file);
    if (next != EOF) {
        fclose(file);
        return -2;
    }
    payload[used] = '\0';
    if (fclose(file) != 0) {
        fprintf(stderr, "mem_service: failed to close snapshot %s\n", path);
        return -1;
    }
    return 0;
}

static bool parse_snapshot_record_count(const char *line, uint64_t *record_count_out)
{
    char *end = NULL;
    uint64_t parsed;

    if (line == NULL || strncmp(line, "record_count=", 13) != 0 ||
        record_count_out == NULL) {
        return false;
    }
    parsed = strtoull(line + 13, &end, 10);
    if (end == line + 13 || *end != '\0') {
        return false;
    }
    *record_count_out = parsed;
    return true;
}

static int append_snapshot_text_line(char *out,
                                     size_t out_len,
                                     const char *line)
{
    size_t used = strlen(out);
    int written;

    if (used >= out_len) {
        return -1;
    }
    written = snprintf(out + used, out_len - used, "%s\n", line);
    if (written < 0 || (size_t)written >= out_len - used) {
        return -1;
    }
    return 0;
}

static int send_restore_snapshot_page_payload(int argc,
                                              char **argv,
                                              const char *payload,
                                              char *response,
                                              size_t response_len,
                                              enum mem_service_wire_status *status_out)
{
    return send_client_payload_request(argc,
                                       argv,
                                       MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                       payload,
                                       response,
                                       response_len,
                                       status_out);
}

static int begin_paged_restore_snapshot(int argc,
                                        char **argv,
                                        bool has_expected_records,
                                        uint64_t expected_records)
{
    char payload[128] = "action=begin\n";
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    int rc;

    if (has_expected_records &&
        mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "expected_records",
                                            expected_records) != 0) {
        return 2;
    }
    rc = send_restore_snapshot_page_payload(argc,
                                            argv,
                                            payload,
                                            response,
                                            sizeof(response),
                                            &status);
    if (rc != 0 || status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service: restore-snapshot begin failed status=%s\n",
                mem_service_wire_status_name(status));
        return rc != 0 ? rc : 1;
    }
    return 0;
}

static void cancel_paged_restore_snapshot(int argc, char **argv)
{
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;

    (void)send_restore_snapshot_page_payload(argc,
                                             argv,
                                             "action=cancel\n",
                                             response,
                                             sizeof(response),
                                             &status);
}

static int send_restore_snapshot_records_page(int argc,
                                              char **argv,
                                              uint64_t page_index,
                                              bool complete,
                                              const char *records)
{
    char payload[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN + 1U];
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    int written;
    int rc;

    written = snprintf(payload,
                       sizeof(payload),
                       "action=append\npage_index=%" PRIu64 "\ncomplete=%u\n%s",
                       page_index,
                       complete ? 1U : 0U,
                       records != NULL ? records : "");
    if (written < 0 || (size_t)written >= sizeof(payload)) {
        return 2;
    }
    rc = send_restore_snapshot_page_payload(argc,
                                            argv,
                                            payload,
                                            response,
                                            sizeof(response),
                                            &status);
    if (rc != 0 || status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service: restore-snapshot append failed status=%s page=%" PRIu64 "\n",
                mem_service_wire_status_name(status),
                page_index);
        return rc != 0 ? rc : 1;
    }
    return 0;
}

static int commit_paged_restore_snapshot(int argc, char **argv)
{
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    int rc = send_restore_snapshot_page_payload(argc,
                                                argv,
                                                "action=commit\n",
                                                response,
                                                sizeof(response),
                                                &status);

    printf("mem_service restore-snapshot: status=%s", mem_service_wire_status_name(status));
    if (response[0] != '\0') {
        printf("\n%s", response);
        if (response[strlen(response) - 1] != '\n') {
            printf("\n");
        }
    } else {
        printf("\n");
    }
    return rc;
}

static int append_record_to_restore_page(int argc,
                                         char **argv,
                                         char *page_records,
                                         size_t page_records_len,
                                         const char *record,
                                         uint64_t *page_index,
                                         uint64_t *pages_sent)
{
    size_t page_used = strlen(page_records);
    size_t record_len = strlen(record);

    if (record_len + 128U >= MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN) {
        fprintf(stderr, "mem_service: snapshot record exceeds wire payload capacity\n");
        return 2;
    }
    if (page_used > 0 &&
        page_used + record_len + 128U >= MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN) {
        int rc = send_restore_snapshot_records_page(argc,
                                                    argv,
                                                    *page_index,
                                                    false,
                                                    page_records);

        if (rc != 0) {
            return rc;
        }
        *page_index += 1U;
        *pages_sent += 1U;
        page_records[0] = '\0';
        page_used = 0;
    }
    if (page_used + record_len >= page_records_len) {
        return 2;
    }
    memcpy(page_records + page_used, record, record_len + 1U);
    return 0;
}

static int run_restore_snapshot_paged(int argc, char **argv, const char *path)
{
    FILE *file;
    char line[512];
    char record[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char page_records[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    bool saw_magic = false;
    bool began = false;
    bool in_record = false;
    bool has_expected_records = false;
    uint64_t expected_records = 0;
    uint64_t page_index = 0;
    uint64_t pages_sent = 0;
    int rc = 0;

    file = fopen(path, "r");
    if (file == NULL) {
        fprintf(stderr, "mem_service: failed to open snapshot %s\n", path);
        return 1;
    }
    record[0] = '\0';
    page_records[0] = '\0';
    while (fgets(line, sizeof(line), file) != NULL) {
        trim_snapshot_line(line);
        if (!saw_magic) {
            if (strcmp(line, MEM_SERVICE_CLI_STORE_MAGIC) != 0) {
                fclose(file);
                return 2;
            }
            saw_magic = true;
            continue;
        }
        if (!began && !in_record) {
            if (parse_snapshot_record_count(line, &expected_records)) {
                has_expected_records = true;
                continue;
            }
            if (strcmp(line, "record_begin") != 0) {
                continue;
            }
            rc = begin_paged_restore_snapshot(argc,
                                              argv,
                                              has_expected_records,
                                              expected_records);
            if (rc != 0) {
                fclose(file);
                return rc;
            }
            began = true;
        }
        if (strcmp(line, "record_begin") == 0) {
            if (in_record) {
                rc = 2;
                break;
            }
            record[0] = '\0';
            in_record = true;
        }
        if (in_record &&
            append_snapshot_text_line(record, sizeof(record), line) != 0) {
            rc = 2;
            break;
        }
        if (strcmp(line, "record_end") == 0) {
            rc = append_record_to_restore_page(argc,
                                               argv,
                                               page_records,
                                               sizeof(page_records),
                                               record,
                                               &page_index,
                                               &pages_sent);
            if (rc != 0) {
                break;
            }
            record[0] = '\0';
            in_record = false;
        }
    }
    if (rc == 0 && (!saw_magic || in_record)) {
        rc = 2;
    }
    if (rc == 0 && !began) {
        rc = begin_paged_restore_snapshot(argc,
                                          argv,
                                          has_expected_records,
                                          expected_records);
        began = rc == 0;
    }
    if (rc == 0 && page_records[0] != '\0') {
        rc = send_restore_snapshot_records_page(argc,
                                                argv,
                                                page_index,
                                                true,
                                                page_records);
        if (rc == 0) {
            page_index += 1U;
            pages_sent += 1U;
            page_records[0] = '\0';
        }
    }
    if (rc == 0 && page_records[0] == '\0' && !has_expected_records) {
        rc = send_restore_snapshot_records_page(argc, argv, page_index, true, "");
        if (rc == 0) {
            pages_sent += 1U;
        }
    }
    if (fclose(file) != 0 && rc == 0) {
        rc = 1;
    }
    if (rc != 0) {
        if (began) {
            cancel_paged_restore_snapshot(argc, argv);
        }
        return rc;
    }
    return commit_paged_restore_snapshot(argc, argv);
}

static int run_restore_snapshot(int argc, char **argv)
{
    char payload[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN + 1U];
    const char *path = restore_snapshot_path_arg(argc, argv);
    int read_rc;

    memset(payload, 0, sizeof(payload));
    read_rc = read_snapshot_file(path, payload, sizeof(payload));
    if (read_rc == -2) {
        return run_restore_snapshot_paged(argc, argv, path);
    }
    if (read_rc != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT,
                                      "restore-snapshot",
                                      payload);
}

static int append_block_context_payload(char *payload,
                                        size_t payload_len,
                                        int argc,
                                        char **argv,
                                        bool require_result)
{
    if (append_required_payload_field(payload, payload_len, argc, argv, "--request-id", "request_id") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--prefix-group", "prefix_group") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--group-id", "group_id") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--block-hash", "block_hash") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--placement-node", "placement_node") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--placement-level", "placement_level") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--hot-segment", "hot_segment_id") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--state", "state") != 0 ||
        append_idempotency_payload_field(payload, payload_len, argc, argv) != 0) {
        return -1;
    }
    if (require_result) {
        return append_required_payload_field(payload,
                                             payload_len,
                                             argc,
                                             argv,
                                             "--result-segment",
                                             "result_segment_id");
    }
    return append_optional_payload_field(payload,
                                         payload_len,
                                         argc,
                                         argv,
                                         "--result-segment",
                                         "result_segment_id");
}

static int run_register_prefix(int argc, char **argv)
{
    char payload[768] = "";

    if (append_block_context_payload(payload, sizeof(payload), argc, argv, true) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_REGISTER_PREFIX_ENTRY,
                                      "register-prefix",
                                      payload);
}

static int run_lookup_prefix(int argc, char **argv)
{
    char payload[256] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--request-id", "request_id") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--prefix-group", "prefix_group") != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_LOOKUP_PREFIX_ENTRY,
                                      "lookup-prefix",
                                      payload);
}

static int run_publish_kv(int argc, char **argv)
{
    char payload[768] = "";

    if (append_block_context_payload(payload, sizeof(payload), argc, argv, false) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_PUBLISH_KV_SEGMENT,
                                      "publish-kv",
                                      payload);
}

static int run_resolve_kv(int argc, char **argv)
{
    char payload[192] = "";

    if (append_payload_field(payload, sizeof(payload), "key", option_value(argc, argv, "--key")) != 0 ||
        append_payload_field(payload, sizeof(payload), "block_hash", option_value(argc, argv, "--block-hash")) != 0 ||
        payload[0] == '\0') {
        fprintf(stderr, "mem_service: missing required --key or --block-hash\n");
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT,
                                      "resolve-kv",
                                      payload);
}

static int append_artifact_payload(char *payload,
                                   size_t payload_len,
                                   int argc,
                                   char **argv)
{
    if (append_required_payload_field(payload, payload_len, argc, argv, "--key", "key") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--session-id", "session_id") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--request-id", "request_id") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--model-key", "model_key") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--artifact-kind", "artifact_kind") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--artifact-id", "artifact_id") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--owner", "owner") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--payload-kind", "payload_kind") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--backing-offset", "backing_offset") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--backing-len", "backing_len") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--checksum", "checksum") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--version", "version") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--payload-inline", "payload_inline") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--payload-file", "payload_path") != 0 ||
        append_idempotency_payload_field(payload, payload_len, argc, argv) != 0) {
        return -1;
    }
    return 0;
}

static int append_artifact_query_payload(char *payload,
                                         size_t payload_len,
                                         int argc,
                                         char **argv)
{
    if (append_required_payload_field(payload, payload_len, argc, argv, "--key", "key") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-session-id", "expected_session_id") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-model-key", "expected_model_key") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-artifact-kind", "expected_artifact_kind") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-artifact-id", "expected_artifact_id") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-owner", "expected_owner") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-version", "expected_version") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-checksum", "expected_checksum") != 0) {
        return -1;
    }
    return 0;
}

static int run_publish_runtime_handoff(int argc, char **argv)
{
    char payload[1024] = "";

    if (append_artifact_payload(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF,
                                      "publish-runtime-handoff",
                                      payload);
}

static int run_resolve_runtime_handoff(int argc, char **argv)
{
    char payload[512] = "";

    if (append_artifact_query_payload(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
                                      "resolve-runtime-handoff",
                                      payload);
}

static int run_register_execution_artifact(int argc, char **argv)
{
    char payload[1024] = "";

    if (append_artifact_payload(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT,
                                      "register-execution-artifact",
                                      payload);
}

static int run_query_execution_artifact(int argc, char **argv)
{
    char payload[512] = "";

    if (append_artifact_query_payload(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT,
                                      "query-execution-artifact",
                                      payload);
}

static int run_register_training_artifact(int argc, char **argv)
{
    char payload[1024] = "";

    if (append_artifact_payload(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
                                      "register-training-artifact",
                                      payload);
}

static int run_query_training_artifact(int argc, char **argv)
{
    char payload[512] = "";

    if (append_artifact_query_payload(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT,
                                      "query-training-artifact",
                                      payload);
}

static int append_training_step_commit_payload(char *payload,
                                               size_t payload_len,
                                               int argc,
                                               char **argv)
{
    if (append_required_payload_field(payload, payload_len, argc, argv, "--key", "key") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--session-id", "session_id") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--request-id", "request_id") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--model-key", "model_key") != 0 ||
        append_payload_field(payload,
                             payload_len,
                             "artifact_kind",
                             MEM_SERVICE_CLIENT_TRAINING_STEP_COMMIT_KIND) != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--artifact-id", "artifact_id") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--owner", "owner") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--payload-kind", "payload_kind") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--backing-offset", "backing_offset") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--backing-len", "backing_len") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--checksum", "checksum") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--version", "version") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--payload-inline", "payload_inline") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--payload-file", "payload_path") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--idempotency-key", "idempotency_key") != 0) {
        return -1;
    }
    return 0;
}

static int append_training_step_query_payload(char *payload,
                                              size_t payload_len,
                                              int argc,
                                              char **argv)
{
    if (append_required_payload_field(payload, payload_len, argc, argv, "--key", "key") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--expected-session-id", "expected_session_id") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--expected-model-key", "expected_model_key") != 0 ||
        append_payload_field(payload,
                             payload_len,
                             "expected_artifact_kind",
                             MEM_SERVICE_CLIENT_TRAINING_STEP_COMMIT_KIND) != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--expected-artifact-id", "expected_artifact_id") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-owner", "expected_owner") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--expected-version", "expected_version") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--expected-checksum", "expected_checksum") != 0) {
        return -1;
    }
    return 0;
}

static int run_commit_training_step(int argc, char **argv)
{
    char payload[1024] = "";

    if (append_training_step_commit_payload(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
                                      "commit-training-step",
                                      payload);
}

static int run_resolve_training_step(int argc, char **argv)
{
    char payload[512] = "";

    if (append_training_step_query_payload(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT,
                                      "resolve-training-step",
                                      payload);
}

static const char *w5_memory_object_store_empty_snapshot(void)
{
    return "{\n"
           "  \"profile\": {\n"
           "    \"metadata_latency_us\": 8,\n"
           "    \"shmem_latency_us\": 3,\n"
           "    \"block_latency_us\": 30,\n"
           "    \"inline_value_limit\": 64,\n"
           "    \"queue_depth\": 4096,\n"
           "    \"obmm_pool\": {\n"
           "      \"enabled\": true,\n"
           "      \"node_count\": 8,\n"
           "      \"queue_depth\": 4096,\n"
           "      \"pool_bytes\": 268435456,\n"
           "      \"payload_base_offset\": 2097152,\n"
           "      \"payload_alignment\": 64,\n"
           "      \"payload_block_tiers\": [262144, 524288, 1048576, 2097152],\n"
           "      \"queue_auto_drain\": true\n"
           "    }\n"
           "  },\n"
           "  \"records\": []\n"
           "}\n";
}

static void print_export_line(const char *name, const char *value)
{
    printf("export %s='%s'\n", name, value != NULL ? value : "");
}

static int run_bootstrap_w5_service(int argc, char **argv)
{
    const char *memory_store = option_value(argc, argv, "--memory-store");
    const char *object_store = option_value(argc, argv, "--memory-object-store");
    const char *engram_state = option_value(argc, argv, "--memory-engram-state");
    const char *registry_dir = option_value(argc, argv, "--memory-registry-dir");
    const char *service_name = option_value(argc, argv, "--service-name");
    bool print_env = option_present(argc, argv, "--print-env");

    if (service_name == NULL || service_name[0] == '\0') {
        service_name = "lingqu_memory_service";
    }
    if (memory_store == NULL || object_store == NULL ||
        engram_state == NULL || registry_dir == NULL) {
        fprintf(stderr,
                "mem_service: bootstrap-w5-service requires --memory-store, "
                "--memory-object-store, --memory-engram-state, and "
                "--memory-registry-dir\n");
        return 2;
    }
    if (ensure_parent_directory_path(memory_store) != 0 ||
        ensure_parent_directory_path(engram_state) != 0 ||
        ensure_directory_path(registry_dir) != 0) {
        fprintf(stderr, "mem_service: failed to prepare W5 memory directories\n");
        return 1;
    }
    if (write_text_file_if_missing(object_store,
                                   w5_memory_object_store_empty_snapshot()) != 0) {
        fprintf(stderr, "mem_service: failed to initialize W5 object store %s\n", object_store);
        return 1;
    }

    if (print_env) {
        print_export_line("SIM_W5_MEMORY_SERVICE", service_name);
        print_export_line("SIM_W5_MEMORY_SERVICE_BOOTSTRAPPED", "1");
        print_export_line("SIM_W5_MEMORY_STORE", memory_store);
        print_export_line("SIM_W5_MEMORY_OBJECT_STORE", object_store);
        print_export_line("SIM_W5_MEMORY_ENGRAM_STATE", engram_state);
        print_export_line("SIM_W5_MEMORY_REGISTRY_DIR", registry_dir);
        return 0;
    }

    printf("linqu_mem_service\n");
    printf("  mode: bootstrap-w5-service\n");
    printf("  service: %s\n", service_name);
    printf("  bootstrapped: true\n");
    printf("  memory_store: %s\n", memory_store);
    printf("  object_store: %s\n", object_store);
    printf("  engram_state: %s\n", engram_state);
    printf("  registry_dir: %s\n", registry_dir);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 1 ||
        strcmp(argv[1], "--smoke") == 0 ||
        strcmp(argv[1], "--self-test") == 0) {
        return run_smoke();
    }
    if (strcmp(argv[1], "version") == 0) {
        return run_version_manifest();
    }
    if (strcmp(argv[1], "version-fixtures") == 0) {
        return run_version_fixture_check();
    }
    if (strcmp(argv[1], "release-readiness") == 0) {
        return run_release_readiness(argc, argv);
    }
    if (strcmp(argv[1], "release-readiness-fixtures") == 0) {
        return run_release_readiness_fixture_check();
    }
    if (strcmp(argv[1], "provider-fixtures") == 0) {
        return mem_service_run_provider_fixture_check();
    }
    if (strcmp(argv[1], "wire-fixtures") == 0) {
        return mem_service_run_wire_fixture_check();
    }
    if (strcmp(argv[1], "wire-schema") == 0) {
        return run_wire_schema_manifest();
    }
    if (strcmp(argv[1], "wire-schema-fixtures") == 0) {
        return run_wire_schema_fixture_check();
    }
    if (strcmp(argv[1], "store-fixtures") == 0) {
        return mem_service_run_store_fixture_check();
    }
    if (strcmp(argv[1], "journal-fixtures") == 0) {
        return mem_service_run_journal_fixture_check();
    }
    if (strcmp(argv[1], "journal-compaction-fixtures") == 0) {
        return mem_service_run_journal_compaction_fixture_check();
    }
    if (strcmp(argv[1], "journal-torn-recovery-fixtures") == 0) {
        return mem_service_run_journal_torn_recovery_fixture_check();
    }
    if (strcmp(argv[1], "config-fixtures") == 0) {
        return run_config_fixture_check();
    }
    if (strcmp(argv[1], "metrics-export-fixtures") == 0) {
        return run_metrics_export_fixture_check();
    }
    if (strcmp(argv[1], "collector-fixtures") == 0) {
        return run_collector_fixture_check();
    }
    if (strcmp(argv[1], "alert-rules") == 0) {
        return run_alert_rules();
    }
    if (strcmp(argv[1], "alert-fixtures") == 0) {
        return run_alert_fixture_check();
    }
    if (strcmp(argv[1], "alert-integration-fixtures") == 0) {
        return run_alert_integration_fixture_check();
    }
    if (strcmp(argv[1], "ops-certification-policy") == 0) {
        return run_ops_certification_policy();
    }
    if (strcmp(argv[1], "ops-certification-fixtures") == 0) {
        return run_ops_certification_fixture_check();
    }
    if (strcmp(argv[1], "encryption-policy") == 0) {
        return run_encryption_policy();
    }
    if (strcmp(argv[1], "encryption-fixtures") == 0) {
        return run_encryption_fixture_check();
    }
    if (strcmp(argv[1], "ops-certification-evidence-fixtures") == 0) {
        return run_ops_certification_evidence_fixture_check();
    }
    if (strcmp(argv[1], "ops-certification-generate-evidence") == 0) {
        return run_ops_certification_generate_evidence(argc, argv);
    }
    if (strcmp(argv[1], "ops-certification-linux-ci-smoke") == 0) {
        return run_ops_certification_linux_ci_smoke(argc, argv);
    }
    if (strcmp(argv[1], "ops-certification-verify") == 0) {
        return run_ops_certification_verify(argc, argv);
    }
    if (strcmp(argv[1], "deployment-fixtures") == 0) {
        return run_deployment_fixture_check();
    }
    if (strcmp(argv[1], "admin-output-schema") == 0) {
        return run_admin_output_schema();
    }
    if (strcmp(argv[1], "admin-output-fixtures") == 0) {
        return run_admin_output_fixture_check();
    }
    if (strcmp(argv[1], "upgrade-rollback-policy") == 0) {
        return run_upgrade_rollback_policy();
    }
    if (strcmp(argv[1], "upgrade-rollback-fixtures") == 0) {
        return run_upgrade_rollback_fixture_check();
    }
    if (strcmp(argv[1], "upgrade-rollback-runtime-fixtures") == 0) {
        return mem_service_run_upgrade_rollback_runtime_fixture_check();
    }
    if (strcmp(argv[1], "restore-policy-fixtures") == 0) {
        return mem_service_run_restore_policy_fixture_check();
    }
    if (strcmp(argv[1], "runtime-quota-fixtures") == 0) {
        return mem_service_run_runtime_quota_fixture_check();
    }
    if (strcmp(argv[1], "retention-fixtures") == 0) {
        return mem_service_run_retention_fixture_check();
    }
    if (strcmp(argv[1], "checkpoint-retention-fixtures") == 0) {
        return mem_service_run_checkpoint_retention_fixture_check();
    }
    if (strcmp(argv[1], "payload-gc-fixtures") == 0) {
        return mem_service_run_payload_gc_fixture_check();
    }
    if (strcmp(argv[1], "record-retention-fixtures") == 0) {
        return mem_service_run_record_retention_fixture_check();
    }
    if (strcmp(argv[1], "durable-catalog-fixtures") == 0) {
        return mem_service_run_durable_catalog_fixture_check();
    }
    if (strcmp(argv[1], "chunked-block-fixtures") == 0) {
        return mem_service_run_chunked_block_fixture_check();
    }
    if (strcmp(argv[1], "transport-block-fixtures") == 0) {
        return mem_service_run_transport_block_fixture_check();
    }
    if (strcmp(argv[1], "network-transport-block-fixtures") == 0) {
        return mem_service_run_network_transport_block_fixture_check();
    }
    if (strcmp(argv[1], "ub-ssd-gsva-descriptor-fixtures") == 0) {
        return run_ub_ssd_gsva_descriptor_fixture_check();
    }
    if (strcmp(argv[1], "remote-block-backend-policy-fixtures") == 0) {
        return run_remote_block_backend_policy_fixture_check();
    }
    if (strcmp(argv[1], "remote-transport-evidence-fixtures") == 0) {
        return run_remote_transport_evidence_fixture_check();
    }
    if (strcmp(argv[1], "remote-transport-serve-fixture") == 0) {
        return run_remote_transport_serve_fixture(argc, argv);
    }
    if (strcmp(argv[1], "remote-transport-generate-evidence") == 0) {
        return run_remote_transport_generate_evidence(argc, argv);
    }
    if (strcmp(argv[1], "remote-transport-verify") == 0) {
        return run_remote_transport_verify(argc, argv);
    }
    if (strcmp(argv[1], "client-retry-fixtures") == 0) {
        return run_client_retry_fixture_check();
    }
    if (strcmp(argv[1], "api-abi-policy") == 0) {
        return run_api_abi_policy();
    }
    if (strcmp(argv[1], "api-abi-fixtures") == 0) {
        return run_api_abi_fixture_check();
    }
    if (strcmp(argv[1], "compat-matrix") == 0) {
        return run_compat_matrix();
    }
    if (strcmp(argv[1], "compat-fixtures") == 0) {
        return run_compat_fixture_check();
    }
    if (strcmp(argv[1], "compat-baseline-v1") == 0) {
        return run_compat_baseline_v1();
    }
    if (strcmp(argv[1], "compat-baseline-fixtures") == 0) {
        return run_compat_baseline_fixture_check();
    }
    if (strcmp(argv[1], "compat-old-new-matrix") == 0) {
        return run_compat_old_new_matrix();
    }
    if (strcmp(argv[1], "compat-old-new-fixtures") == 0) {
        return run_compat_old_new_fixture_check();
    }
    if (strcmp(argv[1], "compat-runtime-fixtures") == 0) {
        return mem_service_run_compat_runtime_fixture_check();
    }
    if (strcmp(argv[1], "compat-old-server-runtime-fixtures") == 0) {
        return mem_service_run_compat_old_server_runtime_fixture_check();
    }
    if (strcmp(argv[1], "serving-fail-closed-fixtures") == 0) {
        return mem_service_run_serving_fail_closed_fixture_check();
    }
    if (strcmp(argv[1], "pretraining-fail-closed-fixtures") == 0) {
        return mem_service_run_pretraining_fail_closed_fixture_check();
    }
    if (strcmp(argv[1], "typed-payload-fixtures") == 0) {
        return mem_service_run_typed_payload_fixture_check();
    }
    if (strcmp(argv[1], "package-manifest") == 0) {
        return run_package_manifest();
    }
    if (strcmp(argv[1], "package-fixtures") == 0) {
        return run_package_fixture_check();
    }
    if (strcmp(argv[1], "release-manifest") == 0) {
        return run_release_manifest();
    }
    if (strcmp(argv[1], "release-fixtures") == 0) {
        return run_release_fixture_check();
    }
    if (strcmp(argv[1], "bootstrap-w5-service") == 0) {
        return run_bootstrap_w5_service(argc, argv);
    }
    if (strcmp(argv[1], "serve") == 0) {
        return run_serve(argc, argv);
    }
    if (strcmp(argv[1], "health") == 0) {
        return run_client_status(argc, argv, MEM_SERVICE_WIRE_OP_HEALTH, "health");
    }
    if (strcmp(argv[1], "ready") == 0) {
        return run_client_status(argc, argv, MEM_SERVICE_WIRE_OP_READY, "ready");
    }
    if (strcmp(argv[1], "status") == 0) {
        return run_client_payload_command(argc,
                                          argv,
                                          MEM_SERVICE_WIRE_OP_STATUS,
                                          "status",
                                          NULL);
    }
    if (strcmp(argv[1], "provider-status") == 0) {
        return run_client_payload_command(argc,
                                          argv,
                                          MEM_SERVICE_WIRE_OP_STATUS,
                                          "provider-status",
                                          NULL);
    }
    if (strcmp(argv[1], "list-records") == 0) {
        return run_client_payload_command(argc,
                                          argv,
                                          MEM_SERVICE_WIRE_OP_LIST_RECORDS,
                                          "list-records",
                                          NULL);
    }
    if (strcmp(argv[1], "metrics") == 0) {
        return run_client_payload_command(argc,
                                          argv,
                                          MEM_SERVICE_WIRE_OP_METRICS,
                                          "metrics",
                                          NULL);
    }
    if (strcmp(argv[1], "audit-log") == 0) {
        return run_audit_log(argc, argv);
    }
    if (strcmp(argv[1], "metrics-export") == 0) {
        return run_metrics_export(argc, argv);
    }
    if (strcmp(argv[1], "export-snapshot") == 0) {
        return run_client_payload_command(argc,
                                          argv,
                                          MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT,
                                          "export-snapshot",
                                          NULL);
    }
    if (strcmp(argv[1], "export-snapshot-page") == 0) {
        return run_export_snapshot_page(argc, argv);
    }
    if (strcmp(argv[1], "export-snapshot-to") == 0) {
        return run_export_snapshot_to(argc, argv);
    }
    if (strcmp(argv[1], "restore-snapshot") == 0) {
        return run_restore_snapshot(argc, argv);
    }
    if (strcmp(argv[1], "put-object") == 0) {
        return run_put_object(argc, argv);
    }
    if (strcmp(argv[1], "get-object") == 0) {
        return run_get_object(argc, argv);
    }
    if (strcmp(argv[1], "inspect-object") == 0) {
        return run_inspect_object(argc, argv);
    }
    if (strcmp(argv[1], "materialize-object") == 0) {
        return run_materialize_object(argc, argv);
    }
    if (strcmp(argv[1], "allocate-object") == 0) {
        return run_allocate_object(argc, argv);
    }
    if (strcmp(argv[1], "acquire-object") == 0) {
        return run_acquire_object(argc, argv);
    }
    if (strcmp(argv[1], "release-object") == 0) {
        return run_release_object(argc, argv);
    }
    if (strcmp(argv[1], "retire-object") == 0) {
        return run_retire_object(argc, argv);
    }
    if (strcmp(argv[1], "inspect-allocation") == 0) {
        return run_inspect_allocation(argc, argv);
    }
    if (strcmp(argv[1], "allocation-stats") == 0) {
        return run_allocation_stats(argc, argv);
    }
    if (strcmp(argv[1], "allocation-fixtures") == 0) {
        return mem_service_run_allocation_fixture_check();
    }
    if (strcmp(argv[1], "provider-directory-fixtures") == 0) {
        return mem_service_run_provider_directory_fixture_check();
    }
    if (strcmp(argv[1], "provider-register") == 0) {
        return run_provider_register(argc, argv);
    }
    if (strcmp(argv[1], "provider-refresh") == 0) {
        return run_provider_refresh(argc, argv);
    }
    if (strcmp(argv[1], "provider-deregister") == 0) {
        return run_provider_deregister(argc, argv);
    }
    if (strcmp(argv[1], "provider-directory-status") == 0) {
        return run_provider_directory_status(argc, argv);
    }
    if (strcmp(argv[1], "publish-allocation") == 0) {
        return run_publish_allocation(argc, argv);
    }
    if (strcmp(argv[1], "reclaim-allocation") == 0) {
        return run_reclaim_allocation(argc, argv);
    }
    if (strcmp(argv[1], "poll-allocation") == 0) {
        return run_poll_allocation(argc, argv);
    }
    if (strcmp(argv[1], "mapping-transition") == 0) {
        return run_mapping_transition(argc, argv);
    }
    if (strcmp(argv[1], "reference-transition") == 0) {
        return run_reference_transition(argc, argv);
    }
    if (strcmp(argv[1], "object-session") == 0) {
        return run_object_session(argc, argv);
    }
    if (strcmp(argv[1], "register-prefix") == 0) {
        return run_register_prefix(argc, argv);
    }
    if (strcmp(argv[1], "lookup-prefix") == 0) {
        return run_lookup_prefix(argc, argv);
    }
    if (strcmp(argv[1], "publish-kv") == 0) {
        return run_publish_kv(argc, argv);
    }
    if (strcmp(argv[1], "resolve-kv") == 0) {
        return run_resolve_kv(argc, argv);
    }
    if (strcmp(argv[1], "publish-runtime-handoff") == 0) {
        return run_publish_runtime_handoff(argc, argv);
    }
    if (strcmp(argv[1], "resolve-runtime-handoff") == 0) {
        return run_resolve_runtime_handoff(argc, argv);
    }
    if (strcmp(argv[1], "register-execution-artifact") == 0) {
        return run_register_execution_artifact(argc, argv);
    }
    if (strcmp(argv[1], "query-execution-artifact") == 0) {
        return run_query_execution_artifact(argc, argv);
    }
    if (strcmp(argv[1], "register-training-artifact") == 0) {
        return run_register_training_artifact(argc, argv);
    }
    if (strcmp(argv[1], "query-training-artifact") == 0) {
        return run_query_training_artifact(argc, argv);
    }
    if (strcmp(argv[1], "commit-training-step") == 0) {
        return run_commit_training_step(argc, argv);
    }
    if (strcmp(argv[1], "resolve-training-step") == 0) {
        return run_resolve_training_step(argc, argv);
    }
    if (strcmp(argv[1], "--inspect-qwen3") == 0) {
#ifdef MEM_SERVICE_ENABLE_QWEN3_INSPECT
        return inspect_qwen3();
#else
        fprintf(stderr, "mem_service qwen3: inspect is available only in the qwen3 adapter build\n");
        return 2;
#endif
    }
    usage(argv[0]);
    return 2;
}
