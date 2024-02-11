#include "gpu_driver_specific.h"

#include "common/io/io.h"
#include "3rdparty/nvml/nvml.h"
#include "util/mallocHelper.h"
#include "common/io/io.h"

#include <dev/pci/pcireg.h>
#include <sys/pciio.h>
#include <fcntl.h>
#include <paths.h>

static bool loadPciIds(FFstrbuf* pciids)
{
    // https://github.com/freebsd/freebsd-src/blob/main/usr.sbin/pciconf/pathnames.h

    ffReadFileBuffer(_PATH_LOCALBASE "/share/pciids/pci.ids", pciids);
    if (pciids->length > 0) return true;

    ffReadFileBuffer("/usr/share/pciids/pci.ids", pciids);
    if (pciids->length > 0) return true;

    return false;
}

const char* ffDetectGPUImpl(const FFGPUOptions* options, FFlist* gpus)
{
    FF_AUTO_CLOSE_FD int fd = open("/dev/pci", O_RDONLY, 0);
    struct pci_conf confs[128];
    struct pci_match_conf match = {
        .pc_class = PCIC_DISPLAY,
        .flags = PCI_GETCONF_MATCH_CLASS,
    };
    struct pci_conf_io pcio = {
        .pat_buf_len = sizeof(match),
        .num_patterns = 1,
        .patterns = &match,
        .match_buf_len = sizeof(confs),
        .matches = confs,
    };

    if (ioctl(fd, PCIOCGETCONF, &pcio) < 0)
        return "ioctl(fd, PCIOCGETCONF, &pc) failed";

    if (pcio.status == PCI_GETCONF_ERROR)
        return "ioctl(fd, PCIOCGETCONF, &pc) returned error";

    FF_STRBUF_AUTO_DESTROY pciids = ffStrbufCreate();
    loadPciIds(&pciids);

    for (uint32_t i = 0; i < pcio.num_matches; ++i)
    {
        struct pci_conf* pc = &confs[i];

        FFGPUResult* gpu = (FFGPUResult*)ffListAdd(gpus);
        ffStrbufInitStatic(&gpu->vendor, ffGetGPUVendorString(pc->pc_vendor));
        ffStrbufInit(&gpu->name);
        ffStrbufInitS(&gpu->driver, pc->pd_name);
        ffStrbufInit(&gpu->platformApi);
        gpu->temperature = FF_GPU_TEMP_UNSET;
        gpu->coreCount = FF_GPU_CORE_COUNT_UNSET;
        gpu->type = FF_GPU_TYPE_UNKNOWN;
        gpu->dedicated.total = gpu->dedicated.used = gpu->shared.total = gpu->shared.used = FF_GPU_VMEM_SIZE_UNSET;
        gpu->deviceId = ((uint64_t) pc->pc_sel.pc_domain << 6) | ((uint64_t) pc->pc_sel.pc_bus << 4) | ((uint64_t) pc->pc_sel.pc_dev << 2) | pc->pc_sel.pc_func;
        gpu->frequency = FF_GPU_FREQUENCY_UNSET;

        ffGPUParsePciIds(&pciids, pc->pc_subclass, pc->pc_vendor, pc->pc_device, pc->pc_subvendor, pc->pc_subdevice, gpu);

        #ifdef FF_USE_PROPRIETARY_GPU_DRIVER_API
        if (gpu->vendor.chars == FF_GPU_VENDOR_NAME_NVIDIA && (options->temp || options->driverSpecific))
        {
            ffDetectNvidiaGpuInfo(&(FFGpuDriverCondition) {
                .type = FF_GPU_DRIVER_CONDITION_TYPE_BUS_ID,
                .pciBusId = {
                    .domain = (uint32_t) pc->pc_sel.pc_domain,
                    .bus = pc->pc_sel.pc_bus,
                    .device = pc->pc_sel.pc_dev,
                    .func = pc->pc_sel.pc_func,
                },
            }, (FFGpuDriverResult) {
                .temp = options->temp ? &gpu->temperature : NULL,
                .memory = options->driverSpecific ? &gpu->dedicated : NULL,
                .coreCount = options->driverSpecific ? (uint32_t*) &gpu->coreCount : NULL,
                .type = &gpu->type,
                .frequency = &gpu->frequency,
            }, "libnvidia-ml.so");

            if (gpu->dedicated.total != FF_GPU_VMEM_SIZE_UNSET)
                gpu->type = gpu->dedicated.total > (uint64_t)1024 * 1024 * 1024 ? FF_GPU_TYPE_DISCRETE : FF_GPU_TYPE_INTEGRATED;
        }
        #endif // FF_USE_PROPRIETARY_GPU_DRIVER_API
    }

    return NULL;
}
