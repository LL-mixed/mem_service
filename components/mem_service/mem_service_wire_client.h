#ifndef MEM_SERVICE_WIRE_CLIENT_H
#define MEM_SERVICE_WIRE_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "mem_service_wire.h"

#define MEM_SERVICE_DEFAULT_UNIX_SOCKET "/tmp/linqu_mem_service.sock"
#define MEM_SERVICE_WIRE_CLIENT_DEFAULT_MAX_ATTEMPTS 1U
#define MEM_SERVICE_WIRE_CLIENT_MAX_ATTEMPTS 8U

struct mem_service_wire_client_options {
    uint64_t timeout_ms;
    uint32_t max_attempts;
    uint64_t retry_backoff_ms;
    uint32_t retry_on_timeout;
};

const char *mem_service_wire_status_name(enum mem_service_wire_status status);
const char *mem_service_default_unix_socket_spec(void);
void mem_service_wire_client_options_init(
    struct mem_service_wire_client_options *options);

int mem_service_send_unix_request_with_options(
    const char *connect_spec,
    const struct mem_service_wire_client_options *options,
    enum mem_service_wire_operation operation,
    const char *payload_in,
    char *payload_out,
    size_t payload_out_len,
    enum mem_service_wire_status *status_out);

int mem_service_send_unix_request(const char *connect_spec,
                                  enum mem_service_wire_operation operation,
                                  const char *payload_in,
                                  char *payload_out,
                                  size_t payload_out_len,
                                  enum mem_service_wire_status *status_out);

/*
 * Transport-neutral entry points. The connect spec selects the transport:
 * NULL or "unix:<path>" (a bare path is treated as a legacy unix path)
 * keeps the existing local behavior, "tcp:<ipv4>:<port>" connects to a
 * daemon serving the wire protocol over TCP. Any other explicit scheme is
 * rejected with a usage error; endpoints never fall back to another
 * transport on failure.
 */
int mem_service_send_request_with_options(
    const char *connect_spec,
    const struct mem_service_wire_client_options *options,
    enum mem_service_wire_operation operation,
    const char *payload_in,
    char *payload_out,
    size_t payload_out_len,
    enum mem_service_wire_status *status_out);

int mem_service_send_request(const char *connect_spec,
                             enum mem_service_wire_operation operation,
                             const char *payload_in,
                             char *payload_out,
                             size_t payload_out_len,
                             enum mem_service_wire_status *status_out);

#endif
