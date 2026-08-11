#include "mem_service_provider_obmm.h"

#include <stdio.h>
#include <string.h>

static void mem_service_obmm_cli_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s status [--device <path>] [--cna-path <path>]\n"
            "       %s protocol-fixtures\n",
            program,
            program);
}

static int mem_service_obmm_cli_status(int argc, char **argv)
{
    const char *device = NULL;
    const char *cna_path = NULL;
    char detail[256];
    int i;

    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strcmp(argv[i], "--cna-path") == 0 && i + 1 < argc) {
            cna_path = argv[++i];
        } else {
            return 2;
        }
    }
    if (mem_service_provider_obmm_probe_device(
            device, cna_path, detail, sizeof(detail)) != 0) {
        fprintf(stderr,
                "mem_service obmm-provider: status=unavailable "
                "data_plane_ready=0 %s\n",
                detail);
        return 1;
    }
    printf("mem_service obmm-provider: status=degraded "
           "data_plane_ready=0 reason=peer-mapping-not-verified %s\n",
           detail);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        mem_service_obmm_cli_usage(argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "status") == 0) {
        int rc = mem_service_obmm_cli_status(argc, argv);

        if (rc == 2) {
            mem_service_obmm_cli_usage(argv[0]);
        }
        return rc;
    }
    if (strcmp(argv[1], "protocol-fixtures") == 0 && argc == 2) {
        return mem_service_provider_obmm_run_protocol_fixture();
    }
    mem_service_obmm_cli_usage(argv[0]);
    return 2;
}
