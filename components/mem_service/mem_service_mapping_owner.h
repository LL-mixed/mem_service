#ifndef MEM_SERVICE_MAPPING_OWNER_H
#define MEM_SERVICE_MAPPING_OWNER_H

#include "mem_service_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEM_SERVICE_MAPPING_OWNER_MAX_PINS 64U

struct mem_service_access_domain;
struct mem_service_mapping_owner;
struct mem_service_mapping_lease;

enum mem_service_mapping_access_kind {
    MEM_SERVICE_MAPPING_ACCESS_CPU = 1,
    MEM_SERVICE_MAPPING_ACCESS_COMPUTE = 2,
};

struct mem_service_mapping_owner_stats {
    uint32_t cpu_pins;
    uint32_t compute_pins;
    uint32_t in_flight_pins;
    uint32_t pin_limit;
    bool closing;
    bool mapping_pending;
    bool holder_pending;
};

struct mem_service_mapping_access_view {
    void *base;
    uint64_t offset;
    uint64_t len;
    uint64_t flags;
    uint64_t generation;
};

/* One domain serializes every owner and provider callback on this channel.
 * Keep its registry/provider alive and immutable until domain destruction.
 * No raw provider/endpoint operation may run concurrently outside the domain.
 * Client options and connect_spec are copied. Destruction requires callers
 * to have joined all threads and destroyed every owner; it never closes the
 * externally created endpoint. All other domain/owner operations are locked.
 */
int mem_service_access_domain_create(
    const struct mem_service_client *client,
    const struct mem_service_provider_channel *channel,
    struct mem_service_access_domain **domain_out);
int mem_service_access_domain_destroy(struct mem_service_access_domain *domain);

/* Move one active mapping/lifecycle AND its existing holder into an owner.
 * A fresh service INSPECT must confirm the transaction. No map/acquire is
 * issued. Success zeros both source structs; failure preserves them exactly.
 * The caller relinquishes its right to release this holder on success.
 * release_operation_id must be globally unique to this holder release.
 * pin_limit is 1..MAX_PINS. Adoption requires exclusive access to the inputs.
 */
int mem_service_mapping_owner_adopt(
    struct mem_service_access_domain *domain,
    struct mem_service_client_object_mapping *mapping,
    struct mem_service_client_mapping_lifecycle *lifecycle,
    const char *release_operation_id, uint32_t pin_limit,
    struct mem_service_mapping_owner **owner_out,
    enum mem_service_wire_status *status_out);

/* Each returned lease belongs to one caller until release; transfer it to
 * another thread only with ordinary synchronization. It pins lifetime, not
 * data ordering: overlapping payload access still needs publish/acquire.
 */
int mem_service_mapping_owner_acquire(
    struct mem_service_mapping_owner *owner,
    enum mem_service_mapping_access_kind kind,
    uint64_t offset, uint64_t len, uint64_t flags,
    struct mem_service_mapping_lease **lease_out,
    struct mem_service_mapping_access_view *view_out);
int mem_service_mapping_lease_release(struct mem_service_mapping_lease *lease);

/* Platform attachment/visibility callbacks run under the shared domain lock.
 * They may inspect the original mapping, but cannot mutate it or retain its
 * address after releasing the lease. They must not reenter domain APIs or
 * wait for work that needs the same domain. A compute lease with an original
 * operation still in flight rejects callbacks with -EBUSY. Submit and wait
 * outside these callbacks; complete only after the original completion.
 * CPU payload access may instead use the bounded view while the lease lives.
 */
typedef int (*mem_service_mapping_access_fn)(
    void *context,
    const struct mem_service_provider_channel *channel,
    const struct mem_service_client_object_mapping *mapping,
    const struct mem_service_client_mapping_lifecycle *lifecycle,
    const struct mem_service_mapping_access_view *view);
int mem_service_mapping_lease_apply(
    struct mem_service_mapping_lease *lease,
    mem_service_mapping_access_fn callback, void *context);

/* Compute adapters pair one begin with a confirmed completion of the same
 * operation. Timeout/cancel requests cannot authorize complete or release.
 * These count in-flight pins, not distinct operations across several leases.
 */
int mem_service_mapping_lease_begin(
    struct mem_service_mapping_lease *lease, uint64_t operation_id);
int mem_service_mapping_lease_complete(
    struct mem_service_mapping_lease *lease, uint64_t operation_id);
int mem_service_mapping_owner_inspect(
    struct mem_service_mapping_owner *owner,
    struct mem_service_mapping_owner_stats *stats_out);

/* Close seals new admission. Busy pins leave the mapping/holder untouched.
 * Retry after draining; uncertain unmap/release retains cleanup ownership.
 * Success leaves an inspectable closed handle. Destroy only after all caller
 * threads have joined; destroy refuses any remaining mapping/holder/pin.
 */
/* Stop new admission and unmap after all pins drain, but retain the holder.
 * A V2 writer can then SEAL its staged references before calling close.
 * Success grants neither new access nor publication. Retry uncertain cleanup
 * on this same owner; destroy still refuses the retained holder. */
int mem_service_mapping_owner_unmap(
    struct mem_service_mapping_owner *owner,
    enum mem_service_wire_status *status_out);

int mem_service_mapping_owner_close(
    struct mem_service_mapping_owner *owner,
    enum mem_service_wire_status *status_out);
int mem_service_mapping_owner_destroy(struct mem_service_mapping_owner *owner);

#ifdef __cplusplus
}
#endif

#endif
