#ifndef MEM_SERVICE_MODEL_RUNTIME_H
#define MEM_SERVICE_MODEL_RUNTIME_H

#include "mem_service_guest_runtime.h"

#include <stdint.h>

uint64_t mem_service_model_payload_checksum(const uint8_t *bytes,
                                            uint64_t len);
int mem_service_model_kv_state_alloc(struct mem_service_cluster_runtime *rt,
                                     uint64_t payload_len,
                                     uint64_t *offset_out,
                                     uint64_t *block_bytes_out,
                                     uint64_t *block_count_out,
                                     uint64_t *reserved_bytes_out);
/* Validate a trusted in-process reservation without allocating or reading it. */
int mem_service_model_kv_state_validate_reserved(
    const struct mem_service_cluster_runtime *rt,
    const uint8_t *payload,
    uint64_t payload_len,
    uint64_t offset,
    uint64_t *block_bytes_out,
    uint64_t *block_count_out,
    uint64_t *reserved_bytes_out);
void mem_service_report_obmm_pool_layout_once(
    struct mem_service_cluster_runtime *rt);
void mem_service_report_obmm_pool_usage(struct mem_service_cluster_runtime *rt,
                                        uint32_t local_node,
                                        uint64_t decode_step);

#endif
