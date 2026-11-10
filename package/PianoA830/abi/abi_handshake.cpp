/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The NativeAdreno ABI v2 handshake as Turnip's darwin KMD makes it (tu_knl_darwin.cc dw_open_connection and device creation), over
 * IOKit: on the host against Adreno830's host simulator (tests/hostsim: host_gpu_service + host_iokit_shim, HOSTSIM_GPU=a830), built
 * and run by tooling/k27/check-a830-abi-host.py. Open the accelerator with type 0x4147, find an admitted selector with the tunnel
 * probe, then through the tunnel: Query (abi_version 2), GetDeviceInfo (the A830's facts), CreateQueue, BoAlloc/BoMap at va_start,
 * BoCpuMap + IOConnectMapMemory64, one Submit of a small IB, WaitFence, GetTimestamp, GetStatus, unmap/free/close. Then the v1 records
 * a Turnip built against the VM's ABI would send (a 416-byte Submit) must be refused. Prints one PASS line or the first failure. */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "NativeAdrenoABI.h"

static io_connect_t conn;
static uint32_t admitted = ~0u;
enum { TunnelMagic = 0x4e544e4c };

static kern_return_t call(uint32_t method, const void *in, size_t in_size, void *out, size_t out_size)
{
  uint8_t buffer[8 + sizeof(struct NativeAdrenoAbiSubmit) + 64];
  if (in_size + 8 > sizeof buffer) return kIOReturnBadArgument;
  ((uint32_t *)buffer)[0] = TunnelMagic;
  ((uint32_t *)buffer)[1] = method;
  if (in_size) memcpy(buffer + 8, in, in_size);
  size_t got = out_size;
  return IOConnectCallStructMethod(conn, admitted, buffer, in_size + 8, out, out ? &got : NULL);
}

#define REQUIRE(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); return 1; } } while (0)

int main(void)
{
  const char *name = getenv("TU_DARWIN_SERVICE") ? getenv("TU_DARWIN_SERVICE") : "IOAcceleratorES";
  io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(name));
  REQUIRE(service, "no %s service", name);
  kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0x4147, &conn);
  IOObjectRelease(service);
  REQUIRE(kr == KERN_SUCCESS, "IOServiceOpen(%s, 0x4147) = %#x", name, kr);
  for (uint32_t selector = 0; selector < 32 && admitted == ~0u; ++selector) {
    uint32_t probe[2] = {TunnelMagic, 0xffffffffu};
    if (IOConnectCallStructMethod(conn, selector, probe, sizeof probe, NULL, NULL) == KERN_SUCCESS) admitted = selector;
  }
  REQUIRE(admitted != ~0u, "no external-method selector admitted for the tunnel");

  struct NativeAdrenoAbiParams params = {};
  REQUIRE(call(NativeAdrenoMethodQuery, NULL, 0, &params, sizeof params) == KERN_SUCCESS, "Query");
  REQUIRE(params.abi_version == NativeAdrenoAbiVersion && params.abi_version == 2, "abi_version %u, want 2", params.abi_version);
  REQUIRE(!(params.flags & NativeAdrenoParamsOwnedCoherent), "OwnedCoherent reported (PianoA830 never sets it)");
  struct NativeAdrenoAbiDeviceInfo info = {};
  REQUIRE(call(NativeAdrenoMethodGetDeviceInfo, NULL, 0, &info, sizeof info) == KERN_SUCCESS, "GetDeviceInfo");
  REQUIRE(info.chip_id == 0x44050001u, "chip_id %#x, want the A830's 0x44050001", info.chip_id);
  REQUIRE(info.flags & NativeAdrenoDeviceClientIova, "DeviceInfo without ClientIova");

  struct NativeAdrenoAbiQueue create = {};
  struct NativeAdrenoAbiQueueReply queue = {};
  REQUIRE(call(NativeAdrenoMethodCreateQueue, &create, sizeof create, &queue, sizeof queue) == KERN_SUCCESS && queue.queue, "CreateQueue");
  struct NativeAdrenoAbiBoAlloc alloc = {0x10000, 12, 0};
  struct NativeAdrenoAbiBoAllocReply bo = {};
  REQUIRE(call(NativeAdrenoMethodBoAlloc, &alloc, sizeof alloc, &bo, sizeof bo) == KERN_SUCCESS && bo.handle, "BoAlloc 64 KiB");
  struct NativeAdrenoAbiBoMap map = {bo.handle, info.va_start, 3, 0, 3, 0};
  struct NativeAdrenoAbiBoMapReply mapped = {};
  REQUIRE(call(NativeAdrenoMethodBoMap, &map, sizeof map, &mapped, sizeof mapped) == KERN_SUCCESS && mapped.iova == info.va_start,
          "BoMap at va_start %#llx", (unsigned long long)info.va_start);
  struct NativeAdrenoAbiBoCpuMap cpu = {bo.handle};
  struct NativeAdrenoAbiBoCpuMapReply lease = {};
  REQUIRE(call(NativeAdrenoMethodBoCpuMap, &cpu, sizeof cpu, &lease, sizeof lease) == KERN_SUCCESS && lease.bytes >= alloc.bytes, "BoCpuMap");
  mach_vm_address_t address = 0;
  mach_vm_size_t bytes = 0;
  REQUIRE(IOConnectMapMemory64(conn, lease.token, mach_task_self(), &address, &bytes, kIOMapAnywhere) == KERN_SUCCESS && address,
          "IOConnectMapMemory64(token %u)", lease.token);
  uint32_t *ib = (uint32_t *)(uintptr_t)address;
  for (unsigned i = 0; i < 16; ++i) ib[i] = 0x70108000u;   /* CP_NOP type-7 headers; the host's software CP executes nothing */

  struct NativeAdrenoAbiSubmit submit;
  memset(&submit, 0, sizeof submit);
  submit.buffer_count = 1;
  submit.queue = queue.queue;
  submit.buffers[0].handle = bo.handle;
  submit.buffers[0].iova = mapped.iova;
  submit.buffers[0].bytes = 64;
  struct NativeAdrenoAbiSubmitReply fence = {};
  REQUIRE(call(NativeAdrenoMethodSubmit, &submit, sizeof submit, &fence, sizeof fence) == KERN_SUCCESS && fence.fence, "Submit (%zu bytes)",
          sizeof submit);
  struct NativeAdrenoAbiWaitFence wait = {fence.fence, 1000000000ull};
  REQUIRE(call(NativeAdrenoMethodWaitFence, &wait, sizeof wait, NULL, 0) == KERN_SUCCESS, "WaitFence %llu", (unsigned long long)fence.fence);
  struct NativeAdrenoAbiTimestamp stamp = {};
  REQUIRE(call(NativeAdrenoMethodGetTimestamp, NULL, 0, &stamp, sizeof stamp) == KERN_SUCCESS, "GetTimestamp");
  struct NativeAdrenoAbiStatus status = {};
  kr = call(NativeAdrenoMethodGetStatus, NULL, 0, &status, sizeof status);
  /* As darwin_device_check_status: kIOReturnUnsupported means no counters are attached (the host simulator binds none). */
  REQUIRE((kr == KERN_SUCCESS && !status.faults) || kr == kIOReturnUnsupported, "GetStatus = %#x (faults %llu, suspends %llu)", kr, (unsigned long long)status.faults,
          (unsigned long long)status.suspends);

  /* What a v1 Turnip sends: a 416-byte Submit (16 buffers) must be refused, never read as v2. */
  REQUIRE(call(NativeAdrenoMethodSubmit, &submit, 32 + 24 * 16, &fence, sizeof fence) != KERN_SUCCESS, "a v1 (416-byte) Submit was accepted");

  IOConnectUnmapMemory64(conn, lease.token, mach_task_self(), address);
  struct NativeAdrenoAbiBoCpuUnmap unmap = {bo.handle, lease.token, 0};
  REQUIRE(call(NativeAdrenoMethodBoCpuUnmap, &unmap, sizeof unmap, NULL, 0) == KERN_SUCCESS, "BoCpuUnmap");
  struct NativeAdrenoAbiBoFree freeing = {bo.handle};
  REQUIRE(call(NativeAdrenoMethodBoFree, &freeing, sizeof freeing, NULL, 0) == KERN_SUCCESS, "BoFree");
  REQUIRE(call(NativeAdrenoMethodCloseQueue, &queue, sizeof queue, NULL, 0) == KERN_SUCCESS, "CloseQueue");
  IOServiceClose(conn);
  printf("PASS: NativeAdreno ABI v2 handshake over IOKit (%s, open 0x4147, tunnel selector %u): Query v%u (in %u / out %u bits, max BO %llu MiB), "
         "DeviceInfo chip %#x GMEM %u KiB VA %#llx+%#llx HBB %u UBWC %u flags %#x, queue, 64 KiB BO at %#llx, CPU lease, Submit %zu bytes -> "
         "fence %llu retired, timestamp, status; a v1 416-byte Submit refused\n",
         name, admitted, params.abi_version, params.input_bits, params.output_bits, (unsigned long long)(params.max_bo_bytes >> 20), info.chip_id,
         info.gmem_bytes >> 10, (unsigned long long)info.va_start, (unsigned long long)info.va_size, info.highest_bank_bit, info.ubwc_version,
         info.flags, (unsigned long long)mapped.iova, sizeof submit, (unsigned long long)fence.fence);
  return 0;
}
