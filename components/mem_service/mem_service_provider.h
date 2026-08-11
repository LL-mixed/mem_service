#ifndef MEM_SERVICE_PROVIDER_H
#define MEM_SERVICE_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MEM_SERVICE_PROVIDER_NAME_LEN 32U
#define MEM_SERVICE_PROVIDER_INSTANCE_LEN 64U
#define MEM_SERVICE_PROVIDER_DESCRIPTOR_LEN 128U
#define MEM_SERVICE_MAX_PROVIDERS 8U
#define MEM_SERVICE_PROVIDER_API_VERSION 2U
#define MEM_SERVICE_PROVIDER_MAPPING_CONTRACT_VERSION 1U
#define MEM_SERVICE_PROVIDER_REGION_WIRE_VERSION 1U
#define MEM_SERVICE_PROVIDER_REGION_WIRE_MAX_LEN \
    (32U + MEM_SERVICE_PROVIDER_NAME_LEN + \
     MEM_SERVICE_PROVIDER_DESCRIPTOR_LEN)

enum mem_service_provider_capability {
    MEM_SERVICE_PROVIDER_CAP_REGION_REGISTRATION = 1ULL << 0,
    MEM_SERVICE_PROVIDER_CAP_LOCAL_TRANSFER = 1ULL << 1,
    MEM_SERVICE_PROVIDER_CAP_PEER_TRANSFER = 1ULL << 2,
    MEM_SERVICE_PROVIDER_CAP_DURABLE_STORAGE = 1ULL << 3,
    MEM_SERVICE_PROVIDER_CAP_ACCELERATOR_MEMORY = 1ULL << 4,
    MEM_SERVICE_PROVIDER_CAP_RECEIVE_FENCE = 1ULL << 5,
    MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING = 1ULL << 6,
};

#define MEM_SERVICE_PROVIDER_CAP_TRANSFER_MASK \
    (MEM_SERVICE_PROVIDER_CAP_LOCAL_TRANSFER | \
     MEM_SERVICE_PROVIDER_CAP_PEER_TRANSFER | \
     MEM_SERVICE_PROVIDER_CAP_DURABLE_STORAGE)

#define MEM_SERVICE_PROVIDER_CAP_DATA_PLANE_MASK \
    (MEM_SERVICE_PROVIDER_CAP_TRANSFER_MASK | \
     MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING)

enum mem_service_region_flag {
    MEM_SERVICE_REGION_FLAG_PROVIDER_ALLOCATED = 1ULL << 0,
};

enum mem_service_mapping_flag {
    MEM_SERVICE_MAPPING_FLAG_READ = 1ULL << 0,
    MEM_SERVICE_MAPPING_FLAG_WRITE = 1ULL << 1,
    MEM_SERVICE_MAPPING_FLAG_FIXED_ADDRESS = 1ULL << 2,
};

#define MEM_SERVICE_MAPPING_FLAG_VALID_MASK \
    (MEM_SERVICE_MAPPING_FLAG_READ | \
     MEM_SERVICE_MAPPING_FLAG_WRITE | \
     MEM_SERVICE_MAPPING_FLAG_FIXED_ADDRESS)

enum mem_service_provider_state {
    MEM_SERVICE_PROVIDER_STATE_UNKNOWN = 0,
    MEM_SERVICE_PROVIDER_STATE_READY = 1,
    MEM_SERVICE_PROVIDER_STATE_DEGRADED = 2,
    MEM_SERVICE_PROVIDER_STATE_UNAVAILABLE = 3,
};

enum mem_service_memory_kind {
    MEM_SERVICE_MEMORY_HOST = 1,
    MEM_SERVICE_MEMORY_ACCELERATOR = 2,
    MEM_SERVICE_MEMORY_PERSISTENT = 3,
};

struct mem_service_provider_descriptor {
    uint32_t len;
    uint8_t bytes[MEM_SERVICE_PROVIDER_DESCRIPTOR_LEN];
};

struct mem_service_region_request {
    void *base;
    uint64_t len;
    enum mem_service_memory_kind memory_kind;
    uint64_t flags;
};

struct mem_service_region {
    uint64_t handle;
    uint64_t len;
    enum mem_service_memory_kind memory_kind;
    struct mem_service_provider_descriptor descriptor;
};

struct mem_service_provider_slice {
    uint64_t region_handle;
    uint64_t offset;
    uint64_t len;
    struct mem_service_provider_descriptor descriptor;
};

struct mem_service_transfer_request {
    struct mem_service_provider_slice source;
    struct mem_service_provider_slice destination;
    uint64_t expected_checksum;
    uint64_t flags;
};

struct mem_service_receive_request {
    struct mem_service_provider_slice destination;
    uint64_t expected_checksum;
    uint64_t flags;
};

struct mem_service_transfer_completion {
    uint64_t id;
    int32_t status;
    uint64_t transferred_bytes;
    uint64_t checksum;
};

struct mem_service_mapping_request {
    struct mem_service_provider_descriptor remote_descriptor;
    uint64_t remote_region_len;
    uint64_t offset;
    uint64_t len;
    void *requested_address;
    enum mem_service_memory_kind memory_kind;
    uint64_t flags;
};

struct mem_service_mapping {
    uint64_t handle;
    void *base;
    uint64_t len;
    enum mem_service_memory_kind memory_kind;
};

struct mem_service_mapping_range_request {
    uint64_t mapping_handle;
    uint64_t offset;
    uint64_t len;
    uint64_t expected_checksum;
    uint64_t timeout_ms;
    uint64_t flags;
};

struct mem_service_visibility_completion {
    int32_t status;
    uint64_t visible_bytes;
    uint64_t checksum;
};

struct mem_service_provider_ops {
    int (*probe)(void *context, enum mem_service_provider_state *state_out);
    int (*register_region)(void *context,
                           const struct mem_service_region_request *request,
                           struct mem_service_region *region_out);
    int (*deregister_region)(void *context, uint64_t region_handle);
    int (*submit_transfer)(void *context,
                           const struct mem_service_transfer_request *request,
                           uint64_t *completion_id_out);
    int (*poll_completion)(void *context,
                           uint64_t completion_id,
                           struct mem_service_transfer_completion *completion_out);
    int (*wait_receive)(void *context,
                        const struct mem_service_receive_request *request,
                        struct mem_service_transfer_completion *completion_out);
    int (*map_remote_region)(void *context,
                             const struct mem_service_mapping_request *request,
                             struct mem_service_mapping *mapping_out);
    int (*unmap_remote_region)(void *context, uint64_t mapping_handle);
    int (*publish_range)(
        void *context,
        const struct mem_service_mapping_range_request *request,
        struct mem_service_visibility_completion *completion_out);
    int (*invalidate_range)(
        void *context,
        const struct mem_service_mapping_range_request *request,
        struct mem_service_visibility_completion *completion_out);
    int (*wait_range_visible)(
        void *context,
        const struct mem_service_mapping_range_request *request,
        struct mem_service_visibility_completion *completion_out);
};

struct mem_service_provider_registration {
    const char *name;
    const char *instance;
    uint64_t capabilities;
    const struct mem_service_provider_ops *ops;
    void *context;
};

struct mem_service_provider {
    char name[MEM_SERVICE_PROVIDER_NAME_LEN];
    char instance[MEM_SERVICE_PROVIDER_INSTANCE_LEN];
    uint64_t capabilities;
    enum mem_service_provider_state state;
    const struct mem_service_provider_ops *ops;
    void *context;
};

struct mem_service_provider_registry {
    bool initialized;
    size_t count;
    struct mem_service_provider providers[MEM_SERVICE_MAX_PROVIDERS];
};

struct mem_service_provider_channel {
    const struct mem_service_provider *provider;
    uint64_t required_capabilities;
};

struct mem_service_provider_region_binding {
    struct mem_service_region region;
    const struct mem_service_provider *owner;
    bool registered;
};

struct mem_service_provider_mapping_binding {
    struct mem_service_mapping mapping;
    const struct mem_service_provider *owner;
    bool mapped;
};

struct mem_service_provider_remote_region {
    char provider_name[MEM_SERVICE_PROVIDER_NAME_LEN];
    uint64_t len;
    enum mem_service_memory_kind memory_kind;
    struct mem_service_provider_descriptor descriptor;
};

const char *mem_service_provider_state_name(enum mem_service_provider_state state);
int mem_service_provider_registry_init(
    struct mem_service_provider_registry *registry);
int mem_service_provider_registry_register(
    struct mem_service_provider_registry *registry,
    const struct mem_service_provider_registration *registration);
int mem_service_provider_registry_refresh(
    struct mem_service_provider_registry *registry);
const struct mem_service_provider *mem_service_provider_registry_find(
    const struct mem_service_provider_registry *registry,
    const char *name,
    const char *instance);
size_t mem_service_provider_registry_ready_count(
    const struct mem_service_provider_registry *registry);
bool mem_service_provider_registry_data_plane_ready(
    const struct mem_service_provider_registry *registry);
uint64_t mem_service_provider_checksum64(const void *data, uint64_t len);
int mem_service_provider_channel_bind(
    const struct mem_service_provider_registry *registry,
    const char *name,
    const char *instance,
    uint64_t required_capabilities,
    struct mem_service_provider_channel *channel_out);
int mem_service_provider_channel_register_region(
    const struct mem_service_provider_channel *channel,
    const struct mem_service_region_request *request,
    struct mem_service_provider_region_binding *binding_out);
int mem_service_provider_channel_export_region(
    const struct mem_service_provider_channel *channel,
    const struct mem_service_provider_region_binding *binding,
    struct mem_service_provider_remote_region *remote_out);
int mem_service_provider_remote_region_encode(
    const struct mem_service_provider_remote_region *remote,
    void *wire_out,
    size_t wire_capacity,
    size_t *wire_len_out);
int mem_service_provider_remote_region_decode(
    const void *wire,
    size_t wire_len,
    struct mem_service_provider_remote_region *remote_out);
int mem_service_provider_channel_transfer(
    const struct mem_service_provider_channel *channel,
    const struct mem_service_provider_region_binding *source,
    uint64_t source_offset,
    const struct mem_service_provider_remote_region *destination,
    uint64_t destination_offset,
    uint64_t len,
    uint64_t expected_checksum,
    struct mem_service_transfer_completion *completion_out);
int mem_service_provider_channel_submit_transfer(
    const struct mem_service_provider_channel *channel,
    const struct mem_service_provider_region_binding *source,
    uint64_t source_offset,
    const struct mem_service_provider_remote_region *destination,
    uint64_t destination_offset,
    uint64_t len,
    uint64_t expected_checksum,
    uint64_t *completion_id_out);
int mem_service_provider_channel_poll_transfer(
    const struct mem_service_provider_channel *channel,
    uint64_t completion_id,
    uint64_t len,
    uint64_t expected_checksum,
    struct mem_service_transfer_completion *completion_out);
int mem_service_provider_channel_wait_receive(
    const struct mem_service_provider_channel *channel,
    const struct mem_service_provider_region_binding *destination,
    uint64_t destination_offset,
    uint64_t len,
    uint64_t expected_checksum,
    struct mem_service_transfer_completion *completion_out);
int mem_service_provider_channel_map_remote_region(
    const struct mem_service_provider_channel *channel,
    const struct mem_service_provider_remote_region *remote,
    uint64_t offset,
    uint64_t len,
    void *requested_address,
    uint64_t flags,
    struct mem_service_provider_mapping_binding *binding_out);
int mem_service_provider_channel_publish_range(
    const struct mem_service_provider_channel *channel,
    const struct mem_service_provider_mapping_binding *binding,
    uint64_t offset,
    uint64_t len,
    uint64_t expected_checksum,
    struct mem_service_visibility_completion *completion_out);
int mem_service_provider_channel_invalidate_range(
    const struct mem_service_provider_channel *channel,
    const struct mem_service_provider_mapping_binding *binding,
    uint64_t offset,
    uint64_t len,
    uint64_t expected_checksum,
    struct mem_service_visibility_completion *completion_out);
int mem_service_provider_channel_wait_range_visible(
    const struct mem_service_provider_channel *channel,
    const struct mem_service_provider_mapping_binding *binding,
    uint64_t offset,
    uint64_t len,
    uint64_t expected_checksum,
    uint64_t timeout_ms,
    struct mem_service_visibility_completion *completion_out);
int mem_service_provider_channel_unmap_remote_region(
    const struct mem_service_provider_channel *channel,
    struct mem_service_provider_mapping_binding *binding);
int mem_service_provider_channel_deregister_region(
    const struct mem_service_provider_channel *channel,
    struct mem_service_provider_region_binding *binding);
int mem_service_run_provider_fixture_check(void);

#endif
