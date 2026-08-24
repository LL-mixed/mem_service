#ifndef MEM_SERVICE_QWEN3_RUNTIME_H
#define MEM_SERVICE_QWEN3_RUNTIME_H

#include "mem_service.h"
#include "mem_service_guest_runtime.h"
#include "mem_service_model_runtime.h"
#include "mem_service_qwen3_placement.h"

#include <stdbool.h>
#include <stdint.h>

int mem_service_qwen3_decode_entry_node(uint32_t cluster_node_count,
                                        uint32_t *node_out);
int mem_service_publish_qwen3_layer_range_placements(
    struct mem_service *svc,
    uint32_t node_count);
bool mem_service_read_qwen3_layer_range_placement(
    struct mem_service *svc,
    uint32_t owner_node,
    struct mem_service_qwen3_layer_range_placement *placement_out);
bool mem_service_find_qwen3_layer_range_predecessor(
    struct mem_service *svc,
    uint32_t owner_node,
    struct mem_service_qwen3_layer_range_placement *placement_out);
int mem_service_qwen3_engram_owner_index(uint32_t cluster_node_count);
void mem_service_qwen3_engram_history_key(char *out, size_t out_len);
void mem_service_qwen3_engram_candidates_key(uint64_t decode_step,
                                             char *out,
                                             size_t out_len);
void mem_service_qwen3_engram_selected_key(uint64_t decode_step,
                                           char *out,
                                           size_t out_len);
void mem_service_qwen3_engram_state_key(uint64_t decode_step,
                                        char *out,
                                        size_t out_len);

#endif
