// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeSoftGpu: the registry accelerator for macOS 27's Metal discovery on a board without a GPU driver. It only advertises a Metal
// plugin (MetalPluginName NativeSoftMetal, package/NativeSoftMetal): no hardware, no user client, no memory.
//
// Why: WindowServer will not run without a Metal device. Without one it waits 30 s and aborts ("No suitable Metal devices present after
// 30 seconds"), then launchd relaunches it, again and again. Metal.framework only makes a device for a registry IOAccelerator, by loading
// <MetalPluginName>.bundle from /Library/GPUBundles (IOGPU's +[IOGPUMetalDevice registerAcceleratorService:]). This service is that
// accelerator. The plugin's device answers acceleratorPort 0, and SkyLight then composites the display with its software compositor
// (CompositorSW). The device has no command queue either, so Core Animation's WindowServer display renders in software. The pixels go
// out through NativeGopDisplay's CPU copy, as before.
//
// The personality matches IOResources in its own match category, so nothing else on IOResources is held up. Boot argument
// native-soft-gpu=0 leaves the accelerator out (WindowServer's behaviour is then as without this kext). Progress goes to the kernel log.
// An IOAccelerator subclass without a GPU behind it (provenance: package/NativeSoftGpu/UPSTREAM).
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/graphics/IOAccelerator.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>

extern "C" void proc_selfname(char *, int);

class NativeSoftAccelerator : public IOAccelerator {
    OSDeclareDefaultStructors(NativeSoftAccelerator)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) override;
    OSObject *copyProperty(const OSSymbol *key) const override;
    OSObject *copyProperty(const OSString *key) const override;
    OSObject *copyProperty(const char *key) const override;
};

OSDefineMetaClassAndStructors(NativeSoftAccelerator, IOAccelerator)

IOService *NativeSoftAccelerator::probe(IOService *provider, SInt32 *score)
{
    unsigned on = 1;
    PE_parse_boot_argn("native-soft-gpu", &on, sizeof(on));
    if (!on) {
        IOLog("NativeSoftGpu: off (boot-arg native-soft-gpu=0): no Metal accelerator\n");
        return nullptr;
    }
    return IOAccelerator::probe(provider, score);
}

bool NativeSoftAccelerator::start(IOService *provider)
{
    if (!IOAccelerator::start(provider)) {
        IOLog("NativeSoftGpu: IOAccelerator::start failed\n");
        return false;
    }
    OSObject *plugin = getProperty("MetalPluginName");
    OSString *name = OSDynamicCast(OSString, plugin);
    registerService();
    IOLog("NativeSoftGpu: Metal accelerator registered on %s (MetalPluginName %s, no GPU, no user client)\n", provider->getName(),
        name ? name->getCStringNoCopy() : "missing");
    return true;
}

// Nothing to open: the plugin's device never asks (acceleratorPort 0). A client that tries anyway is named in the log and refused.
IOReturn NativeSoftAccelerator::newUserClient(task_t, void *, UInt32 type, OSDictionary *, IOUserClient **handler)
{
    if (handler) *handler = nullptr;
    static unsigned logged;
    if (logged < 16) {
        logged++;
        char name[20] = {};
        proc_selfname(name, sizeof(name));
        IOLog("NativeSoftGpu: %s opens the accelerator, type %#x: refused (no GPU)\n", name, unsigned(type));
    }
    return kIOReturnUnsupported;
}

// Who looks for the plugin (Metal's discovery reads MetalPluginName in every process that asks for a device): the first readers.
static void notePropertyRead(const char *key)
{
    static unsigned logged;
    if (!key || strcmp(key, "MetalPluginName") || logged >= 24) return;
    logged++;
    char name[20] = {};
    proc_selfname(name, sizeof(name));
    IOLog("NativeSoftGpu: %s reads MetalPluginName\n", name);
}

OSObject *NativeSoftAccelerator::copyProperty(const OSSymbol *key) const
{
    notePropertyRead(key ? key->getCStringNoCopy() : nullptr);
    return IORegistryEntry::copyProperty(key);
}

OSObject *NativeSoftAccelerator::copyProperty(const OSString *key) const
{
    notePropertyRead(key ? key->getCStringNoCopy() : nullptr);
    return IORegistryEntry::copyProperty(key);
}

OSObject *NativeSoftAccelerator::copyProperty(const char *key) const
{
    notePropertyRead(key);
    return IORegistryEntry::copyProperty(key);
}
