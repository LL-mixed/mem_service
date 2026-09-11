#include "mem_service_client.h"

#include <string.h>

#include "mem_service_wire_client.h"
#include "mem_service_wire_payload.h"

static void mem_service_client_set_status(enum mem_service_wire_status *status_out,
                                          enum mem_service_wire_status status)
{
    if (status_out != NULL) {
        *status_out = status;
    }
}

static const char *mem_service_client_connect_spec(
    const struct mem_service_client *client)
{
    if (client == NULL || client->connect_spec == NULL ||
        client->connect_spec[0] == '\0') {
        return NULL;
    }
    return client->connect_spec;
}

static bool mem_service_client_has_value(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static int mem_service_client_invalid(enum mem_service_wire_status *status_out)
{
    mem_service_client_set_status(status_out, MEM_SERVICE_WIRE_STATUS_INVALID_SESSION);
    return 2;
}

static int mem_service_client_append_required_string(char *payload,
                                                     size_t payload_len,
                                                     const char *name,
                                                     const char *value)
{
    if (!mem_service_client_has_value(value)) {
        return -1;
    }
    return mem_service_wire_payload_append_field(payload, payload_len, name, value);
}

static int mem_service_client_append_optional_string(char *payload,
                                                     size_t payload_len,
                                                     const char *name,
                                                     const char *value)
{
    if (!mem_service_client_has_value(value)) {
        return 0;
    }
    return mem_service_wire_payload_append_field(payload, payload_len, name, value);
}

static int mem_service_client_append_optional_u32(char *payload,
                                                  size_t payload_len,
                                                  const char *name,
                                                  bool present,
                                                  uint32_t value)
{
    if (!present) {
        return 0;
    }
    return mem_service_wire_payload_append_u64(payload,
                                               payload_len,
                                               name,
                                               (uint64_t)value);
}

static int mem_service_client_append_optional_u64(char *payload,
                                                  size_t payload_len,
                                                  const char *name,
                                                  bool present,
                                                  uint64_t value)
{
    if (!present) {
        return 0;
    }
    return mem_service_wire_payload_append_u64(payload, payload_len, name, value);
}

static void mem_service_client_payload_copy(
    const struct mem_service_wire_payload_view *view,
    const char *name,
    char *out,
    size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    if (!mem_service_wire_payload_get_string(view, name, out, out_len)) {
        out[0] = '\0';
    }
}

static int mem_service_client_parse_record(const char *payload,
                                           struct mem_service_client_record *record_out)
{
    struct mem_service_wire_payload_view view;

    if (record_out == NULL) {
        return 0;
    }
    if (payload == NULL || payload[0] == '\0') {
        return -1;
    }
    memset(record_out, 0, sizeof(*record_out));
    view = mem_service_wire_payload_view_from_cstr(payload);
    mem_service_client_payload_copy(&view,
                                    "key",
                                    record_out->key,
                                    sizeof(record_out->key));
    mem_service_client_payload_copy(&view,
                                    "request_id",
                                    record_out->request_id,
                                    sizeof(record_out->request_id));
    mem_service_client_payload_copy(&view,
                                    "prefix_group",
                                    record_out->prefix_group,
                                    sizeof(record_out->prefix_group));
    mem_service_client_payload_copy(&view,
                                    "group_id",
                                    record_out->group_id,
                                    sizeof(record_out->group_id));
    mem_service_client_payload_copy(&view,
                                    "session_id",
                                    record_out->session_id,
                                    sizeof(record_out->session_id));
    mem_service_client_payload_copy(&view,
                                    "model_key",
                                    record_out->model_key,
                                    sizeof(record_out->model_key));
    mem_service_client_payload_copy(&view,
                                    "artifact_kind",
                                    record_out->artifact_kind,
                                    sizeof(record_out->artifact_kind));
    mem_service_client_payload_copy(&view,
                                    "artifact_id",
                                    record_out->artifact_id,
                                    sizeof(record_out->artifact_id));
    mem_service_client_payload_copy(&view,
                                    "block_hash",
                                    record_out->block_hash,
                                    sizeof(record_out->block_hash));
    mem_service_client_payload_copy(&view,
                                    "state",
                                    record_out->state,
                                    sizeof(record_out->state));
    record_out->kind = mem_service_wire_payload_get_u32(&view, "kind", 0);
    record_out->placement_node =
        mem_service_wire_payload_get_u32(&view, "placement_node", 0);
    record_out->placement_level =
        mem_service_wire_payload_get_u32(&view, "placement_level", 0);
    record_out->hot_segment_id =
        mem_service_wire_payload_get_u64(&view, "hot_segment_id", 0);
    record_out->version = mem_service_wire_payload_get_u64(&view, "version", 0);
    record_out->last_result_segment =
        mem_service_wire_payload_get_u64(&view, "last_result_segment", 0);
    record_out->object_owner_node =
        mem_service_wire_payload_get_u32(&view, "object_owner_node", 0);
    record_out->object_payload_kind =
        mem_service_wire_payload_get_u32(&view, "object_payload_kind", 0);
    record_out->object_backing_offset =
        mem_service_wire_payload_get_u64(&view, "object_backing_offset", 0);
    record_out->object_backing_len =
        mem_service_wire_payload_get_u64(&view, "object_backing_len", 0);
    record_out->object_payload_checksum =
        mem_service_wire_payload_get_u64(&view, "object_payload_checksum", 0);
    if (record_out->object_payload_checksum == 0) {
        record_out->object_payload_checksum =
            mem_service_wire_payload_get_u64(&view, "checksum", 0);
    }
    record_out->object_backend_kind =
        mem_service_wire_payload_get_u32(&view, "object_backend_kind", 0);
    record_out->object_backend_node =
        mem_service_wire_payload_get_u32(&view, "object_backend_node", 0);
    record_out->object_backend_device_cna =
        mem_service_wire_payload_get_u32(&view, "object_backend_device_cna", 0);
    record_out->object_backend_flags =
        mem_service_wire_payload_get_u32(&view, "object_backend_flags", 0);
    record_out->object_backend_block_hi =
        mem_service_wire_payload_get_u64(&view, "object_backend_block_hi", 0);
    record_out->object_backend_block_lo =
        mem_service_wire_payload_get_u64(&view, "object_backend_block_lo", 0);
    record_out->object_backend_block_version =
        mem_service_wire_payload_get_u64(&view, "object_backend_block_version", 0);
    record_out->object_backend_block_offset =
        mem_service_wire_payload_get_u64(&view, "object_backend_block_offset", 0);
    record_out->object_backend_block_bytes =
        mem_service_wire_payload_get_u64(&view, "object_backend_block_bytes", 0);
    record_out->object_backend_block_checksum =
        mem_service_wire_payload_get_u64(&view, "object_backend_block_checksum", 0);
    return record_out->key[0] == '\0' ? -1 : 0;
}

static int mem_service_client_send(
    const struct mem_service_client *client,
    enum mem_service_wire_operation operation,
    const char *payload,
    char *response,
    size_t response_len,
    enum mem_service_wire_status *status_out)
{
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    const struct mem_service_wire_client_options *options =
        client != NULL ? &client->wire_options : NULL;
    int rc = mem_service_send_request_with_options(
        mem_service_client_connect_spec(client),
        options,
        operation,
        payload,
        response,
        response_len,
        &status);

    mem_service_client_set_status(status_out, status);
    return rc;
}

static int mem_service_client_send_record(
    const struct mem_service_client *client,
    enum mem_service_wire_operation operation,
    const char *payload,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    int rc;

    memset(response, 0, sizeof(response));
    rc = mem_service_client_send(client,
                                 operation,
                                 payload,
                                 response,
                                 sizeof(response),
                                 &status);
    if (status_out != NULL) {
        *status_out = status;
    }
    if (rc != 0) {
        return rc;
    }
    if (mem_service_client_parse_record(response, record_out) != 0) {
        mem_service_client_set_status(status_out, MEM_SERVICE_WIRE_STATUS_INTERNAL);
        return 1;
    }
    return 0;
}

static int mem_service_client_append_object_payload(
    char *payload,
    size_t payload_len,
    const struct mem_service_client_object *object)
{
    if (object == NULL ||
        mem_service_client_append_required_string(payload,
                                                  payload_len,
                                                  "key",
                                                  object->key) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "idempotency_key",
                                                  object->idempotency_key) != 0 ||
        mem_service_client_append_optional_u32(payload,
                                               payload_len,
                                               "owner",
                                               object->has_owner,
                                               object->owner) != 0 ||
        mem_service_client_append_optional_u32(payload,
                                               payload_len,
                                               "payload_kind",
                                               object->has_payload_kind,
                                               object->payload_kind) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "backing_offset",
                                               object->has_backing_offset,
                                               object->backing_offset) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "backing_len",
                                               object->has_backing_len,
                                               object->backing_len) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "checksum",
                                               object->has_checksum,
                                               object->checksum) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "version",
                                               object->has_version,
                                               object->version) != 0 ||
        mem_service_client_append_optional_u32(payload,
                                               payload_len,
                                               "backend_kind",
                                               object->has_backend_kind,
                                               object->backend_kind) != 0 ||
        mem_service_client_append_optional_u32(payload,
                                               payload_len,
                                               "backend_node",
                                               object->has_backend_node,
                                               object->backend_node) != 0 ||
        mem_service_client_append_optional_u32(payload,
                                               payload_len,
                                               "backend_device_cna",
                                               object->has_backend_device_cna,
                                               object->backend_device_cna) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "backend_block_hi",
                                               object->has_backend_block_hi,
                                               object->backend_block_hi) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "backend_block_lo",
                                               object->has_backend_block_lo,
                                               object->backend_block_lo) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "backend_block_version",
                                               object->has_backend_block_version,
                                               object->backend_block_version) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "backend_block_offset",
                                               object->has_backend_block_offset,
                                               object->backend_block_offset) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "backend_block_bytes",
                                               object->has_backend_block_bytes,
                                               object->backend_block_bytes) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "backend_block_checksum",
                                               object->has_backend_block_checksum,
                                               object->backend_block_checksum) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "payload_inline",
                                                  object->payload_inline) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "payload_path",
                                                  object->payload_path) != 0) {
        return -1;
    }
    return 0;
}

static int mem_service_client_append_block_payload(
    char *payload,
    size_t payload_len,
    const struct mem_service_client_block_entry *entry,
    bool require_result)
{
    if (entry == NULL ||
        mem_service_client_append_required_string(payload,
                                                  payload_len,
                                                  "request_id",
                                                  entry->request_id) != 0 ||
        mem_service_client_append_required_string(payload,
                                                  payload_len,
                                                  "prefix_group",
                                                  entry->prefix_group) != 0 ||
        mem_service_client_append_required_string(payload,
                                                  payload_len,
                                                  "group_id",
                                                  entry->group_id) != 0 ||
        mem_service_client_append_required_string(payload,
                                                  payload_len,
                                                  "block_hash",
                                                  entry->block_hash) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "idempotency_key",
                                                  entry->idempotency_key) != 0 ||
        mem_service_client_append_optional_u32(payload,
                                               payload_len,
                                               "placement_node",
                                               entry->has_placement_node,
                                               entry->placement_node) != 0 ||
        mem_service_client_append_optional_u32(payload,
                                               payload_len,
                                               "placement_level",
                                               entry->has_placement_level,
                                               entry->placement_level) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "hot_segment_id",
                                               entry->has_hot_segment_id,
                                               entry->hot_segment_id) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "state",
                                                  entry->state) != 0) {
        return -1;
    }
    if (require_result &&
        (!entry->has_result_segment_id || entry->result_segment_id == 0)) {
        return -1;
    }
    return mem_service_client_append_optional_u64(payload,
                                                  payload_len,
                                                  "result_segment_id",
                                                  entry->has_result_segment_id,
                                                  entry->result_segment_id);
}

static int mem_service_client_append_artifact_payload(
    char *payload,
    size_t payload_len,
    const struct mem_service_client_artifact *artifact)
{
    if (artifact == NULL ||
        mem_service_client_append_required_string(payload,
                                                  payload_len,
                                                  "key",
                                                  artifact->key) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "idempotency_key",
                                                  artifact->idempotency_key) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "session_id",
                                                  artifact->session_id) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "request_id",
                                                  artifact->request_id) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "model_key",
                                                  artifact->model_key) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "artifact_kind",
                                                  artifact->artifact_kind) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "artifact_id",
                                                  artifact->artifact_id) != 0 ||
        mem_service_client_append_optional_u32(payload,
                                               payload_len,
                                               "owner",
                                               artifact->has_owner,
                                               artifact->owner) != 0 ||
        mem_service_client_append_optional_u32(payload,
                                               payload_len,
                                               "payload_kind",
                                               artifact->has_payload_kind,
                                               artifact->payload_kind) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "backing_offset",
                                               artifact->has_backing_offset,
                                               artifact->backing_offset) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "backing_len",
                                               artifact->has_backing_len,
                                               artifact->backing_len) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "checksum",
                                               artifact->has_checksum,
                                               artifact->checksum) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "version",
                                               artifact->has_version,
                                               artifact->version) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "payload_inline",
                                                  artifact->payload_inline) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "payload_path",
                                                  artifact->payload_path) != 0) {
        return -1;
    }
    return 0;
}

static int mem_service_client_append_artifact_query_payload(
    char *payload,
    size_t payload_len,
    const struct mem_service_client_artifact_query *query)
{
    if (query == NULL ||
        mem_service_client_append_required_string(payload,
                                                  payload_len,
                                                  "key",
                                                  query->key) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "expected_session_id",
                                                  query->expected_session_id) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "expected_model_key",
                                                  query->expected_model_key) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "expected_artifact_kind",
                                                  query->expected_artifact_kind) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "expected_artifact_id",
                                                  query->expected_artifact_id) != 0 ||
        mem_service_client_append_optional_u32(payload,
                                               payload_len,
                                               "expected_owner",
                                               query->has_expected_owner,
                                               query->expected_owner) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "expected_version",
                                               query->has_expected_version,
                                               query->expected_version) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "expected_checksum",
                                               query->has_expected_checksum,
                                               query->expected_checksum) != 0) {
        return -1;
    }
    return 0;
}

static int mem_service_client_publish_artifact(
    const struct mem_service_client *client,
    enum mem_service_wire_operation operation,
    const struct mem_service_client_artifact *artifact,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    char payload[1024] = "";

    if (mem_service_client_append_artifact_payload(payload,
                                                   sizeof(payload),
                                                   artifact) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          operation,
                                          payload,
                                          record_out,
                                          status_out);
}

static int mem_service_client_query_artifact(
    const struct mem_service_client *client,
    enum mem_service_wire_operation operation,
    const struct mem_service_client_artifact_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    char payload[1024] = "";

    if (mem_service_client_append_artifact_query_payload(payload,
                                                         sizeof(payload),
                                                         query) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          operation,
                                          payload,
                                          record_out,
                                          status_out);
}

static int mem_service_client_publish_training_ref(
    const struct mem_service_client *client,
    const char *artifact_kind,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    struct mem_service_client_artifact artifact;

    if (ref == NULL || !mem_service_client_has_value(artifact_kind)) {
        return mem_service_client_invalid(status_out);
    }
    memset(&artifact, 0, sizeof(artifact));
    artifact.key = ref->key;
    artifact.idempotency_key = ref->idempotency_key;
    artifact.session_id = ref->session_id;
    artifact.request_id = ref->request_id;
    artifact.model_key = ref->model_key;
    artifact.artifact_kind = artifact_kind;
    artifact.artifact_id = ref->artifact_id;
    artifact.has_owner = ref->has_owner;
    artifact.owner = ref->owner;
    artifact.has_payload_kind = ref->has_payload_kind;
    artifact.payload_kind = ref->payload_kind;
    artifact.has_backing_offset = ref->has_backing_offset;
    artifact.backing_offset = ref->backing_offset;
    artifact.has_backing_len = ref->has_backing_len;
    artifact.backing_len = ref->backing_len;
    artifact.has_checksum = ref->has_checksum;
    artifact.checksum = ref->checksum;
    artifact.has_version = ref->has_version;
    artifact.version = ref->version;
    artifact.payload_inline = ref->payload_inline;
    artifact.payload_path = ref->payload_path;
    return mem_service_client_publish_artifact(
        client,
        MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
        &artifact,
        record_out,
        status_out);
}

static int mem_service_client_resolve_training_ref(
    const struct mem_service_client *client,
    const char *artifact_kind,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    struct mem_service_client_artifact_query artifact_query;

    if (query == NULL || !mem_service_client_has_value(artifact_kind)) {
        return mem_service_client_invalid(status_out);
    }
    memset(&artifact_query, 0, sizeof(artifact_query));
    artifact_query.key = query->key;
    artifact_query.expected_session_id = query->expected_session_id;
    artifact_query.expected_model_key = query->expected_model_key;
    artifact_query.expected_artifact_kind = artifact_kind;
    artifact_query.expected_artifact_id = query->expected_artifact_id;
    artifact_query.has_expected_owner = query->has_expected_owner;
    artifact_query.expected_owner = query->expected_owner;
    artifact_query.has_expected_version = query->has_expected_version;
    artifact_query.expected_version = query->expected_version;
    artifact_query.has_expected_checksum = query->has_expected_checksum;
    artifact_query.expected_checksum = query->expected_checksum;
    return mem_service_client_query_artifact(
        client,
        MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT,
        &artifact_query,
        record_out,
        status_out);
}

void mem_service_client_init(struct mem_service_client *client,
                             const char *connect_spec)
{
    mem_service_client_init_with_options(client, connect_spec, NULL);
}

void mem_service_client_init_with_options(
    struct mem_service_client *client,
    const char *connect_spec,
    const struct mem_service_wire_client_options *options)
{
    if (client != NULL) {
        memset(client, 0, sizeof(*client));
        client->connect_spec = connect_spec;
        mem_service_wire_client_options_init(&client->wire_options);
        if (options != NULL) {
            client->wire_options = *options;
        }
    }
}

int mem_service_client_health(const struct mem_service_client *client,
                              enum mem_service_wire_status *status_out)
{
    return mem_service_client_send(client,
                                   MEM_SERVICE_WIRE_OP_HEALTH,
                                   NULL,
                                   NULL,
                                   0,
                                   status_out);
}

int mem_service_client_ready(const struct mem_service_client *client,
                             enum mem_service_wire_status *status_out)
{
    return mem_service_client_send(client,
                                   MEM_SERVICE_WIRE_OP_READY,
                                   NULL,
                                   NULL,
                                   0,
                                   status_out);
}

int mem_service_client_status(const struct mem_service_client *client,
                              char *payload_out,
                              size_t payload_out_len,
                              enum mem_service_wire_status *status_out)
{
    return mem_service_client_send(client,
                                   MEM_SERVICE_WIRE_OP_STATUS,
                                   NULL,
                                   payload_out,
                                   payload_out_len,
                                   status_out);
}

int mem_service_client_list_records(const struct mem_service_client *client,
                                    char *payload_out,
                                    size_t payload_out_len,
                                    enum mem_service_wire_status *status_out)
{
    return mem_service_client_send(client,
                                   MEM_SERVICE_WIRE_OP_LIST_RECORDS,
                                   NULL,
                                   payload_out,
                                   payload_out_len,
                                   status_out);
}

int mem_service_client_export_snapshot(const struct mem_service_client *client,
                                       char *payload_out,
                                       size_t payload_out_len,
                                       enum mem_service_wire_status *status_out)
{
    return mem_service_client_send(client,
                                   MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT,
                                   NULL,
                                   payload_out,
                                   payload_out_len,
                                   status_out);
}

int mem_service_client_export_snapshot_page(const struct mem_service_client *client,
                                            uint64_t start_index,
                                            uint64_t max_records,
                                            char *payload_out,
                                            size_t payload_out_len,
                                            enum mem_service_wire_status *status_out)
{
    char payload[160] = "";

    if (mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "start_index",
                                            start_index) != 0 ||
        (max_records != 0 &&
         mem_service_wire_payload_append_u64(payload,
                                             sizeof(payload),
                                             "max_records",
                                             max_records) != 0)) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send(client,
                                   MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT_PAGE,
                                   payload,
                                   payload_out,
                                   payload_out_len,
                                   status_out);
}

int mem_service_client_restore_snapshot(const struct mem_service_client *client,
                                        const char *snapshot_payload,
                                        char *payload_out,
                                        size_t payload_out_len,
                                        enum mem_service_wire_status *status_out)
{
    if (!mem_service_client_has_value(snapshot_payload)) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send(client,
                                   MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT,
                                   snapshot_payload,
                                   payload_out,
                                   payload_out_len,
                                   status_out);
}

int mem_service_client_restore_snapshot_page(const struct mem_service_client *client,
                                             const char *page_payload,
                                             char *payload_out,
                                             size_t payload_out_len,
                                             enum mem_service_wire_status *status_out)
{
    if (!mem_service_client_has_value(page_payload)) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send(client,
                                   MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                   page_payload,
                                   payload_out,
                                   payload_out_len,
                                   status_out);
}

int mem_service_client_put_object(const struct mem_service_client *client,
                                  const struct mem_service_client_object *object,
                                  struct mem_service_client_record *record_out,
                                  enum mem_service_wire_status *status_out)
{
    char payload[512] = "";

    if (mem_service_client_append_object_payload(payload, sizeof(payload), object) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                          payload,
                                          record_out,
                                          status_out);
}

int mem_service_client_get_object(const struct mem_service_client *client,
                                  const char *key,
                                  struct mem_service_client_record *record_out,
                                  enum mem_service_wire_status *status_out)
{
    char payload[160] = "";

    if (mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "key",
                                                  key) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          MEM_SERVICE_WIRE_OP_GET_OBJECT,
                                          payload,
                                          record_out,
                                          status_out);
}

int mem_service_client_inspect_object(const struct mem_service_client *client,
                                      const char *key,
                                      struct mem_service_client_record *record_out,
                                      enum mem_service_wire_status *status_out)
{
    char payload[160] = "";

    if (mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "key",
                                                  key) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          MEM_SERVICE_WIRE_OP_INSPECT_OBJECT,
                                          payload,
                                          record_out,
                                          status_out);
}

int mem_service_client_materialize_object(
    const struct mem_service_client *client,
    const char *key,
    const char *destination_path,
    bool has_expected_version,
    uint64_t expected_version,
    bool has_expected_checksum,
    uint64_t expected_checksum,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    char payload[768] = "";

    if (mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "key",
                                                  key) != 0 ||
        mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "destination_path",
                                                  destination_path) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               sizeof(payload),
                                               "expected_version",
                                               has_expected_version,
                                               expected_version) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               sizeof(payload),
                                               "expected_checksum",
                                               has_expected_checksum,
                                               expected_checksum) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          MEM_SERVICE_WIRE_OP_MATERIALIZE_OBJECT,
                                          payload,
                                          record_out,
                                          status_out);
}

int mem_service_client_register_prefix_entry(
    const struct mem_service_client *client,
    const struct mem_service_client_block_entry *entry,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    char payload[768] = "";

    if (mem_service_client_append_block_payload(payload,
                                                sizeof(payload),
                                                entry,
                                                true) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          MEM_SERVICE_WIRE_OP_REGISTER_PREFIX_ENTRY,
                                          payload,
                                          record_out,
                                          status_out);
}

int mem_service_client_lookup_prefix_entry(
    const struct mem_service_client *client,
    const char *request_id,
    const char *prefix_group,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    char payload[256] = "";

    if (mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "request_id",
                                                  request_id) != 0 ||
        mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "prefix_group",
                                                  prefix_group) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          MEM_SERVICE_WIRE_OP_LOOKUP_PREFIX_ENTRY,
                                          payload,
                                          record_out,
                                          status_out);
}

int mem_service_client_publish_kv_segment(
    const struct mem_service_client *client,
    const struct mem_service_client_block_entry *entry,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    char payload[768] = "";

    if (mem_service_client_append_block_payload(payload,
                                                sizeof(payload),
                                                entry,
                                                false) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          MEM_SERVICE_WIRE_OP_PUBLISH_KV_SEGMENT,
                                          payload,
                                          record_out,
                                          status_out);
}

int mem_service_client_resolve_kv_segment(
    const struct mem_service_client *client,
    const struct mem_service_client_kv_selector *selector,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    char payload[192] = "";

    if (selector == NULL ||
        (mem_service_client_append_optional_string(payload,
                                                   sizeof(payload),
                                                   "key",
                                                   selector->key) != 0 ||
         mem_service_client_append_optional_string(payload,
                                                   sizeof(payload),
                                                   "block_hash",
                                                   selector->block_hash) != 0) ||
        payload[0] == '\0') {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT,
                                          payload,
                                          record_out,
                                          status_out);
}

int mem_service_client_publish_runtime_handoff(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact *artifact,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_publish_artifact(
        client,
        MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF,
        artifact,
        record_out,
        status_out);
}

int mem_service_client_resolve_runtime_handoff(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_query_artifact(
        client,
        MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
        query,
        record_out,
        status_out);
}

int mem_service_client_register_execution_artifact(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact *artifact,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_publish_artifact(
        client,
        MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT,
        artifact,
        record_out,
        status_out);
}

int mem_service_client_query_execution_artifact(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_query_artifact(
        client,
        MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT,
        query,
        record_out,
        status_out);
}

int mem_service_client_register_training_artifact(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact *artifact,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_publish_artifact(
        client,
        MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
        artifact,
        record_out,
        status_out);
}

int mem_service_client_query_training_artifact(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_query_artifact(
        client,
        MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT,
        query,
        record_out,
        status_out);
}

int mem_service_client_publish_dataset_shard(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_publish_training_ref(client,
                                                   "dataset-shard",
                                                   ref,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_resolve_dataset_shard(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_resolve_training_ref(client,
                                                   "dataset-shard",
                                                   query,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_publish_sample_batch(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_publish_training_ref(client,
                                                   "sample-batch",
                                                   ref,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_resolve_sample_batch(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_resolve_training_ref(client,
                                                   "sample-batch",
                                                   query,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_publish_checkpoint(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_publish_training_ref(client,
                                                   "checkpoint",
                                                   ref,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_resolve_checkpoint(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_resolve_training_ref(client,
                                                   "checkpoint",
                                                   query,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_publish_gradient_bucket(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_publish_training_ref(client,
                                                   "gradient-bucket",
                                                   ref,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_resolve_gradient_bucket(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_resolve_training_ref(client,
                                                   "gradient-bucket",
                                                   query,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_publish_optimizer_state(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_publish_training_ref(client,
                                                   "optimizer-state",
                                                   ref,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_resolve_optimizer_state(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_resolve_training_ref(client,
                                                   "optimizer-state",
                                                   query,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_commit_training_step(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_publish_training_ref(
        client,
        MEM_SERVICE_CLIENT_TRAINING_STEP_COMMIT_KIND,
        ref,
        record_out,
        status_out);
}

int mem_service_client_resolve_training_step(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_resolve_training_ref(
        client,
        MEM_SERVICE_CLIENT_TRAINING_STEP_COMMIT_KIND,
        query,
        record_out,
        status_out);
}

/*
 * Managed object allocation control (M1.1). The allocation view is parsed
 * into its own struct; the 808-byte record layout above is untouched.
 */

static int mem_service_client_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void mem_service_client_hex_encode(const uint8_t *data,
                                          size_t len,
                                          char *out,
                                          size_t out_len)
{
    static const char digits[] = "0123456789abcdef";
    size_t i = 0;

    if (out_len == 0) {
        return;
    }
    for (i = 0; i < len && (size_t)(2U * i + 2U) < out_len; ++i) {
        out[2U * i] = digits[(data[i] >> 4U) & 0xfU];
        out[2U * i + 1U] = digits[data[i] & 0xfU];
    }
    out[(size_t)(2U * i) < out_len ? (size_t)(2U * i) : out_len - 1U] = '\0';
}

static int mem_service_client_hex_decode(const char *hex,
                                         uint8_t *out,
                                         size_t out_cap,
                                         uint32_t *out_len)
{
    size_t len;
    size_t i;

    if (hex == NULL || out == NULL || out_len == NULL) {
        return -1;
    }
    len = strlen(hex);
    if ((len % 2U) != 0 || len / 2U > out_cap) {
        return -1;
    }
    for (i = 0; i < len / 2U; ++i) {
        int hi = mem_service_client_hex_nibble(hex[2U * i]);
        int lo = mem_service_client_hex_nibble(hex[2U * i + 1U]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = (uint32_t)(len / 2U);
    return 0;
}

static int mem_service_client_parse_allocation(
    const char *payload,
    struct mem_service_client_allocation *allocation_out)
{
    struct mem_service_wire_payload_view view;
    uint32_t i;
    uint32_t holder_count = 0;

    if (allocation_out == NULL) {
        return 0;
    }
    if (payload == NULL || payload[0] == '\0') {
        return -1;
    }
    memset(allocation_out, 0, sizeof(*allocation_out));
    view = mem_service_wire_payload_view_from_cstr(payload);
    mem_service_client_payload_copy(&view,
                                    "key",
                                    allocation_out->key,
                                    sizeof(allocation_out->key));
    mem_service_client_payload_copy(&view,
                                    "state",
                                    allocation_out->state,
                                    sizeof(allocation_out->state));
    mem_service_client_payload_copy(&view,
                                    "owner_session",
                                    allocation_out->owner_session,
                                    sizeof(allocation_out->owner_session));
    allocation_out->generation =
        mem_service_wire_payload_get_u64(&view, "generation", 0);
    allocation_out->version =
        mem_service_wire_payload_get_u64(&view, "version", 0);
    allocation_out->size_bytes =
        mem_service_wire_payload_get_u64(&view, "size_bytes", 0);
    allocation_out->alignment_bytes =
        mem_service_wire_payload_get_u64(&view, "alignment_bytes", 0);
    allocation_out->capabilities =
        mem_service_wire_payload_get_u64(&view, "capabilities", 0);
    allocation_out->live_refs =
        mem_service_wire_payload_get_u32(&view, "live_refs", 0);
    allocation_out->provider_incarnation =
        mem_service_wire_payload_get_u64(&view, "provider_incarnation", 0);
    allocation_out->descriptor_len =
        mem_service_wire_payload_get_u32(&view, "descriptor_len", 0);
    mem_service_client_payload_copy(&view,
                                    "home_node",
                                    allocation_out->home_node,
                                    sizeof(allocation_out->home_node));
    if (strcmp(allocation_out->home_node, "-") == 0) {
        allocation_out->home_node[0] = '\0';
    }
    allocation_out->provider_backed =
        mem_service_wire_payload_get_u32(&view, "provider_backed", 0) != 0;
    allocation_out->address =
        mem_service_wire_payload_get_u64(&view, "address", 0);
    allocation_out->address_len =
        mem_service_wire_payload_get_u64(&view, "address_len", 0);
    {
        char descriptor_hex[2U * MEM_SERVICE_CLIENT_ALLOCATION_DESCRIPTOR_MAX_LEN + 1U];
        uint32_t decoded_len = 0;

        memset(descriptor_hex, 0, sizeof(descriptor_hex));
        if (mem_service_wire_payload_get_string(&view,
                                                "descriptor_hex",
                                                descriptor_hex,
                                                sizeof(descriptor_hex)) &&
            descriptor_hex[0] != '\0') {
            if (mem_service_client_hex_decode(descriptor_hex,
                                              allocation_out->descriptor,
                                              sizeof(allocation_out->descriptor),
                                              &decoded_len) != 0) {
                return -1;
            }
            allocation_out->descriptor_len = decoded_len;
        }
    }
    for (i = 0; i < MEM_SERVICE_CLIENT_ALLOCATION_MAX_HOLDERS; ++i) {
        char field[48];

        snprintf(field, sizeof(field), "holder.%u.session_id", i);
        if (!mem_service_wire_payload_get_string(
                &view,
                field,
                allocation_out->holders[holder_count].session_id,
                sizeof(allocation_out->holders[holder_count].session_id))) {
            break;
        }
        snprintf(field, sizeof(field), "holder.%u.generation", i);
        allocation_out->holders[holder_count].generation =
            mem_service_wire_payload_get_u64(&view, field, 0);
        holder_count += 1U;
    }
    allocation_out->holder_count = holder_count;
    return allocation_out->key[0] == '\0' ? -1 : 0;
}

static int mem_service_client_send_allocation(
    const struct mem_service_client *client,
    enum mem_service_wire_operation operation,
    const char *payload,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out)
{
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    int rc;

    memset(response, 0, sizeof(response));
    rc = mem_service_client_send(client,
                                 operation,
                                 payload,
                                 response,
                                 sizeof(response),
                                 &status);
    if (status_out != NULL) {
        *status_out = status;
    }
    if (rc != 0) {
        return rc;
    }
    if (mem_service_client_parse_allocation(response, allocation_out) != 0) {
        mem_service_client_set_status(status_out,
                                      MEM_SERVICE_WIRE_STATUS_INTERNAL);
        return 1;
    }
    return 0;
}

static int mem_service_client_send_holder_op(
    const struct mem_service_client *client,
    enum mem_service_wire_operation operation,
    const char *key,
    const char *idempotency_key,
    const char *session_id,
    bool has_expected_generation,
    uint64_t expected_generation,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out)
{
    char payload[512] = "";

    if (mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "key",
                                                  key) != 0 ||
        mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "idempotency_key",
                                                  idempotency_key) != 0 ||
        (session_id != NULL &&
         mem_service_client_append_required_string(payload,
                                                   sizeof(payload),
                                                   "session_id",
                                                   session_id) != 0) ||
        mem_service_client_append_optional_u64(payload,
                                               sizeof(payload),
                                               "expected_generation",
                                               has_expected_generation,
                                               expected_generation) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_allocation(client,
                                              operation,
                                              payload,
                                              allocation_out,
                                              status_out);
}

int mem_service_client_allocate_object(
    const struct mem_service_client *client,
    const struct mem_service_client_allocate *request,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out)
{
    char payload[512] = "";

    if (request == NULL || request->size_bytes == 0 ||
        request->capabilities == 0 ||
        mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "key",
                                                  request->key) != 0 ||
        mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "idempotency_key",
                                                  request->idempotency_key) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  sizeof(payload),
                                                  "session_id",
                                                  request->session_id) != 0 ||
        mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "size_bytes",
                                            request->size_bytes) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               sizeof(payload),
                                               "alignment_bytes",
                                               request->alignment_bytes != 0,
                                               request->alignment_bytes) != 0 ||
        mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "capabilities",
                                            request->capabilities) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_allocation(client,
                                              MEM_SERVICE_WIRE_OP_ALLOCATE_OBJECT,
                                              payload,
                                              allocation_out,
                                              status_out);
}

int mem_service_client_acquire_object(
    const struct mem_service_client *client,
    const char *key,
    const char *idempotency_key,
    const char *session_id,
    bool has_expected_generation,
    uint64_t expected_generation,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_send_holder_op(client,
                                             MEM_SERVICE_WIRE_OP_ACQUIRE_OBJECT,
                                             key,
                                             idempotency_key,
                                             session_id,
                                             has_expected_generation,
                                             expected_generation,
                                             allocation_out,
                                             status_out);
}

int mem_service_client_release_object(
    const struct mem_service_client *client,
    const char *key,
    const char *idempotency_key,
    const char *session_id,
    bool has_expected_generation,
    uint64_t expected_generation,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_send_holder_op(client,
                                             MEM_SERVICE_WIRE_OP_RELEASE_OBJECT,
                                             key,
                                             idempotency_key,
                                             session_id,
                                             has_expected_generation,
                                             expected_generation,
                                             allocation_out,
                                             status_out);
}

int mem_service_client_retire_object(
    const struct mem_service_client *client,
    const char *key,
    const char *idempotency_key,
    bool has_expected_generation,
    uint64_t expected_generation,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_send_holder_op(client,
                                             MEM_SERVICE_WIRE_OP_RETIRE_OBJECT,
                                             key,
                                             idempotency_key,
                                             NULL,
                                             has_expected_generation,
                                             expected_generation,
                                             allocation_out,
                                             status_out);
}

int mem_service_client_poll_allocation(
    const struct mem_service_client *client,
    const char *node_id,
    uint64_t incarnation,
    uint64_t after_generation,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out)
{
    char payload[256] = "";

    if (incarnation == 0 ||
        mem_service_client_append_required_string(payload, sizeof(payload),
                                                  "node_id", node_id) != 0 ||
        mem_service_wire_payload_append_u64(payload, sizeof(payload),
                                            "incarnation", incarnation) != 0 ||
        mem_service_wire_payload_append_u64(payload, sizeof(payload),
                                            "after_generation", after_generation) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_allocation(client,
                                              MEM_SERVICE_WIRE_OP_POLL_ALLOCATION,
                                              payload, allocation_out, status_out);
}

int mem_service_client_inspect_allocation(
    const struct mem_service_client *client,
    const char *key,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out)
{
    char payload[160] = "";

    if (mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "key",
                                                  key) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_allocation(client,
                                              MEM_SERVICE_WIRE_OP_INSPECT_ALLOCATION,
                                              payload,
                                              allocation_out,
                                              status_out);
}

int mem_service_client_allocation_stats(
    const struct mem_service_client *client,
    struct mem_service_client_allocation_stats *stats_out,
    enum mem_service_wire_status *status_out)
{
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    struct mem_service_wire_payload_view view;
    int rc;

    memset(response, 0, sizeof(response));
    rc = mem_service_client_send(client,
                                 MEM_SERVICE_WIRE_OP_ALLOCATION_STATS,
                                 "",
                                 response,
                                 sizeof(response),
                                 &status);
    if (status_out != NULL) {
        *status_out = status;
    }
    if (rc != 0 || stats_out == NULL) {
        return rc;
    }
    view = mem_service_wire_payload_view_from_cstr(response);
    memset(stats_out, 0, sizeof(*stats_out));
    stats_out->backing_registered =
        mem_service_wire_payload_get_u64(&view, "backing_registered", 0);
    stats_out->live_objects =
        mem_service_wire_payload_get_u64(&view, "live_objects", 0);
    stats_out->backing_allocated_bytes =
        mem_service_wire_payload_get_u64(&view, "backing_allocated_bytes", 0);
    stats_out->address_reserved_bytes =
        mem_service_wire_payload_get_u64(&view, "address_reserved_bytes", 0);
    stats_out->export_mappings =
        mem_service_wire_payload_get_u64(&view, "export_mappings", 0);
    stats_out->import_mappings =
        mem_service_wire_payload_get_u64(&view, "import_mappings", 0);
    stats_out->live_refs =
        mem_service_wire_payload_get_u64(&view, "live_refs", 0);
    stats_out->in_flight =
        mem_service_wire_payload_get_u64(&view, "in_flight", 0);
    stats_out->quarantined_objects =
        mem_service_wire_payload_get_u64(&view, "quarantined_objects", 0);
    stats_out->quarantined_bytes =
        mem_service_wire_payload_get_u64(&view, "quarantined_bytes", 0);
    stats_out->allocate_ok_count =
        mem_service_wire_payload_get_u64(&view, "allocate_ok_count", 0);
    stats_out->acquire_ok_count =
        mem_service_wire_payload_get_u64(&view, "acquire_ok_count", 0);
    stats_out->release_ok_count =
        mem_service_wire_payload_get_u64(&view, "release_ok_count", 0);
    stats_out->retire_ok_count =
        mem_service_wire_payload_get_u64(&view, "retire_ok_count", 0);
    stats_out->allocate_rejected_count =
        mem_service_wire_payload_get_u64(&view, "allocate_rejected_count", 0);
    stats_out->acquire_rejected_count =
        mem_service_wire_payload_get_u64(&view, "acquire_rejected_count", 0);
    stats_out->release_rejected_count =
        mem_service_wire_payload_get_u64(&view, "release_rejected_count", 0);
    stats_out->retire_rejected_count =
        mem_service_wire_payload_get_u64(&view, "retire_rejected_count", 0);
    stats_out->publish_ok_count =
        mem_service_wire_payload_get_u64(&view, "publish_ok_count", 0);
    stats_out->publish_rejected_count =
        mem_service_wire_payload_get_u64(&view, "publish_rejected_count", 0);
    stats_out->reclaim_ok_count =
        mem_service_wire_payload_get_u64(&view, "reclaim_ok_count", 0);
    stats_out->reclaim_rejected_count =
        mem_service_wire_payload_get_u64(&view, "reclaim_rejected_count", 0);
    stats_out->quarantine_events =
        mem_service_wire_payload_get_u64(&view, "quarantine_events", 0);
    return 0;
}

int mem_service_client_publish_allocation(
    const struct mem_service_client *client,
    const char *key,
    const char *node_id,
    uint64_t incarnation,
    uint64_t generation,
    const uint8_t *descriptor,
    uint32_t descriptor_len,
    uint64_t address,
    uint64_t address_len,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out)
{
    char payload[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN] = "";
    char descriptor_hex[2U * MEM_SERVICE_CLIENT_ALLOCATION_DESCRIPTOR_MAX_LEN + 1U];

    if (descriptor == NULL || descriptor_len == 0 ||
        descriptor_len > MEM_SERVICE_CLIENT_ALLOCATION_DESCRIPTOR_MAX_LEN ||
        incarnation == 0 || address_len == 0) {
        return mem_service_client_invalid(status_out);
    }
    mem_service_client_hex_encode(descriptor,
                                  descriptor_len,
                                  descriptor_hex,
                                  sizeof(descriptor_hex));
    if (mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "key",
                                                  key) != 0 ||
        mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "node_id",
                                                  node_id) != 0 ||
        mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "incarnation",
                                            incarnation) != 0 ||
        mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "generation",
                                            generation) != 0 ||
        mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "descriptor_hex",
                                                  descriptor_hex) != 0 ||
        mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "address",
                                            address) != 0 ||
        mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "address_len",
                                            address_len) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_allocation(client,
                                              MEM_SERVICE_WIRE_OP_PUBLISH_ALLOCATION,
                                              payload,
                                              allocation_out,
                                              status_out);
}

int mem_service_client_reclaim_allocation(
    const struct mem_service_client *client,
    const char *key,
    const char *node_id,
    uint64_t incarnation,
    uint64_t generation,
    bool confirmed,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out)
{
    char payload[512] = "";

    if (incarnation == 0 ||
        mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "key",
                                                  key) != 0 ||
        mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "node_id",
                                                  node_id) != 0 ||
        mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "incarnation",
                                            incarnation) != 0 ||
        mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "generation",
                                            generation) != 0 ||
        mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "confirmed",
                                            confirmed ? 1U : 0U) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_allocation(client,
                                              MEM_SERVICE_WIRE_OP_RECLAIM_ALLOCATION,
                                              payload,
                                              allocation_out,
                                              status_out);
}

/*
 * Client-side object mapping. No control RPC is issued here: the caller
 * supplies the allocation view from a successful acquire/inspect and a
 * ready provider channel, and the mapping is created locally through the
 * provider contract. Returns 0 on success, -1 on any validation or
 * mapping failure (fail-closed; no fallback VA is ever substituted).
 */
int mem_service_client_map_allocation(
    const struct mem_service_provider_channel *channel,
    const struct mem_service_client_allocation *allocation,
    uint64_t flags,
    struct mem_service_client_object_mapping *mapping_out)
{
    struct mem_service_provider_remote_region remote;
    struct mem_service_client_object_mapping mapping;
    size_t key_len;

    if (channel == NULL || channel->provider == NULL ||
        allocation == NULL || mapping_out == NULL ||
        (flags & ~(MEM_SERVICE_CLIENT_MAP_READ |
                   MEM_SERVICE_CLIENT_MAP_WRITE)) != 0 ||
        (flags & (MEM_SERVICE_CLIENT_MAP_READ |
                  MEM_SERVICE_CLIENT_MAP_WRITE)) == 0 ||
        strcmp(allocation->state, "active") != 0 ||
        !allocation->provider_backed ||
        (allocation->capabilities & MEM_SERVICE_CLIENT_MANAGED_CAP_MAP) == 0 ||
        allocation->descriptor_len == 0 ||
        allocation->descriptor_len >
            MEM_SERVICE_CLIENT_ALLOCATION_DESCRIPTOR_MAX_LEN ||
        allocation->descriptor_len > MEM_SERVICE_PROVIDER_DESCRIPTOR_LEN ||
        allocation->address == 0 || allocation->size_bytes == 0 ||
        allocation->address_len == 0 ||
        allocation->address_len < allocation->size_bytes) {
        return -1;
    }
    key_len = strlen(allocation->key);
    if (key_len == 0 || key_len >= MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN) {
        return -1;
    }
    memset(&remote, 0, sizeof(remote));
    snprintf(remote.provider_name,
             sizeof(remote.provider_name),
             "%s",
             channel->provider->name);
    remote.len = allocation->address_len;
    remote.memory_kind = MEM_SERVICE_MEMORY_HOST;
    remote.descriptor.len = allocation->descriptor_len;
    memcpy(remote.descriptor.bytes,
           allocation->descriptor,
           allocation->descriptor_len);

    memset(&mapping, 0, sizeof(mapping));
    if (mem_service_provider_channel_map_remote_region(
            channel,
            &remote,
            0,
            allocation->size_bytes,
            (void *)(uintptr_t)allocation->address,
            MEM_SERVICE_MAPPING_FLAG_FIXED_ADDRESS |
                ((flags & MEM_SERVICE_CLIENT_MAP_READ) != 0
                     ? MEM_SERVICE_MAPPING_FLAG_READ
                     : 0) |
                ((flags & MEM_SERVICE_CLIENT_MAP_WRITE) != 0
                     ? MEM_SERVICE_MAPPING_FLAG_WRITE
                     : 0),
            &mapping.binding) != 0) {
        return -1;
    }
    /* Strict same-VA: the provider must deliver exactly the requested
     * UBA; anything else is torn down and reported as a failure. */
    if (mapping.binding.mapping.base !=
            (void *)(uintptr_t)allocation->address ||
        mapping.binding.mapping.len != allocation->size_bytes) {
        (void)mem_service_provider_channel_unmap_remote_region(channel,
                                                               &mapping.binding);
        return -1;
    }
    memcpy(mapping.key, allocation->key, key_len + 1);
    mapping.generation = allocation->generation;
    mapping.base = mapping.binding.mapping.base;
    /* The provider owns the full aligned backing; clients own only the
     * requested logical bytes, including when the last backing page is padded. */
    mapping.len = allocation->size_bytes;
    mapping.flags = flags;
    *mapping_out = mapping;
    return 0;
}

int mem_service_client_unmap_allocation(
    const struct mem_service_provider_channel *channel,
    struct mem_service_client_object_mapping *mapping)
{
    if (channel == NULL || mapping == NULL || mapping->base == NULL ||
        !mapping->binding.mapped) {
        return -1;
    }
    if (mem_service_provider_channel_unmap_remote_region(channel,
                                                         &mapping->binding) !=
        0) {
        return -1;
    }
    mapping->base = NULL;
    mapping->len = 0;
    mapping->binding.mapped = false;
    return 0;
}

static void mem_service_client_parse_provider_directory(
    const char *response,
    struct mem_service_client_provider_directory *view_out)
{
    struct mem_service_wire_payload_view view;

    if (view_out == NULL) {
        return;
    }
    view = mem_service_wire_payload_view_from_cstr(response);
    memset(view_out, 0, sizeof(*view_out));
    view_out->directory_epoch =
        mem_service_wire_payload_get_u64(&view, "directory_epoch", 0);
    view_out->lease_ms =
        mem_service_wire_payload_get_u64(&view, "lease_ms", 0);
    view_out->required_count =
        mem_service_wire_payload_get_u64(&view, "provider_required_count", 0);
    view_out->active_count =
        mem_service_wire_payload_get_u64(&view, "provider_active_count", 0);
    view_out->directory_ready =
        mem_service_wire_payload_get_u64(&view, "provider_directory_ready", 0) !=
        0;
    view_out->data_plane_ready =
        mem_service_wire_payload_get_u64(&view, "data_plane_ready", 0) != 0;
    view_out->replaced =
        mem_service_wire_payload_get_u64(&view, "replaced", 0) != 0;
    view_out->register_ok_count =
        mem_service_wire_payload_get_u64(&view, "register_ok_count", 0);
    view_out->register_replace_count =
        mem_service_wire_payload_get_u64(&view, "register_replace_count", 0);
    view_out->refresh_ok_count =
        mem_service_wire_payload_get_u64(&view, "refresh_ok_count", 0);
    view_out->deregister_ok_count =
        mem_service_wire_payload_get_u64(&view, "deregister_ok_count", 0);
    view_out->register_rejected_count =
        mem_service_wire_payload_get_u64(&view, "register_rejected_count", 0);
    view_out->refresh_rejected_count =
        mem_service_wire_payload_get_u64(&view, "refresh_rejected_count", 0);
    view_out->deregister_rejected_count =
        mem_service_wire_payload_get_u64(&view, "deregister_rejected_count", 0);
    view_out->incarnation_conflict_count =
        mem_service_wire_payload_get_u64(&view, "incarnation_conflict_count", 0);
    view_out->expired_count =
        mem_service_wire_payload_get_u64(&view, "expired_count", 0);
}

static int mem_service_client_send_provider_op(
    const struct mem_service_client *client,
    enum mem_service_wire_operation operation,
    const char *payload,
    struct mem_service_client_provider_directory *view_out,
    enum mem_service_wire_status *status_out)
{
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    int rc;

    memset(response, 0, sizeof(response));
    rc = mem_service_client_send(client,
                                 operation,
                                 payload,
                                 response,
                                 sizeof(response),
                                 &status);
    if (status_out != NULL) {
        *status_out = status;
    }
    if (rc != 0) {
        return rc;
    }
    mem_service_client_parse_provider_directory(response, view_out);
    return 0;
}

int mem_service_client_provider_register(
    const struct mem_service_client *client,
    const char *node_id,
    uint64_t incarnation,
    uint64_t readiness_generation,
    uint64_t capabilities,
    struct mem_service_client_provider_directory *view_out,
    enum mem_service_wire_status *status_out)
{
    char payload[512] = "";

    if (incarnation == 0 || capabilities == 0 ||
        mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "node_id",
                                                  node_id) != 0 ||
        mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "incarnation",
                                            incarnation) != 0 ||
        mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "readiness_generation",
                                            readiness_generation) != 0 ||
        mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "capabilities",
                                            capabilities) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_provider_op(client,
                                               MEM_SERVICE_WIRE_OP_PROVIDER_REGISTER,
                                               payload,
                                               view_out,
                                               status_out);
}

int mem_service_client_provider_refresh(
    const struct mem_service_client *client,
    const char *node_id,
    uint64_t incarnation,
    uint64_t readiness_generation,
    struct mem_service_client_provider_directory *view_out,
    enum mem_service_wire_status *status_out)
{
    char payload[384] = "";

    if (incarnation == 0 ||
        mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "node_id",
                                                  node_id) != 0 ||
        mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "incarnation",
                                            incarnation) != 0 ||
        mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "readiness_generation",
                                            readiness_generation) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_provider_op(client,
                                               MEM_SERVICE_WIRE_OP_PROVIDER_REFRESH,
                                               payload,
                                               view_out,
                                               status_out);
}

int mem_service_client_provider_deregister(
    const struct mem_service_client *client,
    const char *node_id,
    uint64_t incarnation,
    struct mem_service_client_provider_directory *view_out,
    enum mem_service_wire_status *status_out)
{
    char payload[256] = "";

    if (incarnation == 0 ||
        mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "node_id",
                                                  node_id) != 0 ||
        mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "incarnation",
                                            incarnation) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_provider_op(client,
                                               MEM_SERVICE_WIRE_OP_PROVIDER_DEREGISTER,
                                               payload,
                                               view_out,
                                               status_out);
}

int mem_service_client_provider_status(
    const struct mem_service_client *client,
    struct mem_service_client_provider_directory *view_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_send_provider_op(client,
                                               MEM_SERVICE_WIRE_OP_PROVIDER_STATUS,
                                               "",
                                               view_out,
                                               status_out);
}
