// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the apple80211 requests (WifiApple80211.h, docs/drivers/wifi-menu.md section 5): the layouts' sizes and offsets,
// a model of user space (a buffer at a fake user address, copyin/copyout by memcpy, a fault for an address outside it) asking
// what airportd's bind and the menu ask, and the answers read back field by field: the identity, the power off/on and what it
// stops, the channel list, a scan and its results until the end, the cache clear, the not-associated answers, the unknown
// request, the scan list's address parse and the rebuilt elements, and the log's one-line-per-kind table.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. apple80211_host.cpp -o /tmp/apple80211_host && /tmp/apple80211_host
#include "WifiApple80211.h"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pianowifi;
using namespace pianowifi::a80211;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// User space: one buffer at a made-up address.
struct User {
  static constexpr uint64_t kBase = 0x16f000000ull;
  std::vector<uint8_t> mem = std::vector<uint8_t>(8192, 0xa5);
  bool inside(uint64_t at, uint32_t n) const { return at >= kBase && at + n <= kBase + mem.size(); }
};
static int userIn(void *ctx, uint64_t from, void *to, uint32_t n)
{
  User &u = *static_cast<User *>(ctx);
  if (!u.inside(from, n)) return 14;
  std::memcpy(to, u.mem.data() + (from - User::kBase), n);
  return 0;
}
static int userOut(void *ctx, const void *from, uint64_t to, uint32_t n)
{
  User &u = *static_cast<User *>(ctx);
  if (!u.inside(to, n)) return 14;
  std::memcpy(u.mem.data() + (to - User::kBase), from, n);
  return 0;
}

static const Identity kId = {"PianoWifi 1.0 (apple80211 M1)", "WCN7860", "PianoWifi"};

struct Rig {
  User user;
  UserIo io{&user, userIn, userOut};
  State st;
  std::vector<uint8_t> scratch = std::vector<uint8_t>(kScratchBytes);
  Req rq{};
  Answer ask(bool set, int32_t type, uint32_t length, bool buffer = true)
  {
    std::memset(&rq, 0, sizeof rq);
    std::strcpy(rq.name, "en2");
    rq.type = type;
    rq.length = length;
    rq.data = buffer ? User::kBase : 0;
    return handle(st, kId, set, rq, io, scratch.data());
  }
  uint32_t u32(uint32_t at) const
  {
    uint32_t v;
    std::memcpy(&v, user.mem.data() + at, 4);
    return v;
  }
  const uint8_t *at(uint32_t off) const { return user.mem.data() + off; }
  void put32(uint32_t off, uint32_t v) { std::memcpy(user.mem.data() + off, &v, 4); }
};

static Bss network(uint8_t last, const char *name, uint32_t freq, int32_t rssi, bool rsn)
{
  Bss b;
  const uint8_t addr[6] = {0x3c, 0x52, 0xa1, 0x10, 0x20, last};
  std::memcpy(b.bssid, addr, 6);
  b.ssidLength = uint8_t(std::strlen(name));
  std::memcpy(b.ssid, name, b.ssidLength);
  b.freq = freq;
  b.channel = uint16_t(channelOfFreq(freq));
  b.rssi = rssi;
  const uint8_t rates[] = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c};
  std::memcpy(b.rates, rates, sizeof rates);
  b.rateCount = sizeof rates;
  if (rsn) {   // RSN: version 1, group CCMP, one pairwise CCMP, one AKM PSK, capabilities 0
    const uint8_t e[] = {48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0};
    std::memcpy(b.rsn, e, sizeof e);
    b.rsnLength = sizeof e;
  }
  return b;
}

int main()
{
  // ---- the layouts: the request and the answers' fixed sizes and offsets
  CHECK(sizeof(Req) == 40);
  CHECK(sizeof(ScanResult) == 1164 && offsetof(ScanResult, elements) == 0x8c && offsetof(ScanResult, elementsLength) == 0x8a);
  CHECK(offsetof(ScanResult, channel) == 4 && offsetof(ScanResult, ssid) == 0x61 && offsetof(ScanResult, capability) == 0x1a);
  CHECK(sizeof(Power) == 24 && sizeof(Rssi) == 52 && sizeof(LinkEvent) == 32 && sizeof(Channel) == 12);
  CHECK(kSiocSA80211 == 0x802869c8UL && kSiocGA80211 == 0xc02869c9UL);
  // _IOW('i', 200, 40 bytes) and _IOWR('i', 201, 40 bytes)
  CHECK(((kSiocSA80211 >> 16) & 0x1fff) == 40 && (kSiocSA80211 & 0xff) == 200 && ((kSiocSA80211 >> 8) & 0xff) == 'i');
  CHECK(((kSiocGA80211 >> 16) & 0x1fff) == 40 && (kSiocGA80211 & 0xff) == 201 && (kSiocGA80211 >> 30) == 3);
  CHECK(std::strcmp(requestName(kHeCapability), "HE_CAPABILITY") == 0 && kHeCapability == 570);
  CHECK(std::strcmp(requestName(9999), "?") == 0);

  // ---- the bind's three: DRIVER_VERSION with a 0x180-byte buffer, CARD_CAPABILITIES, STATUS_DEV_NAME
  {
    Rig r;
    // 27.3: a 384-byte buffer, a NUL-terminated string from offset 0, zeros after it
    Answer a = r.ask(false, kDriverVersion, 0x180);
    CHECK(a.error == 0 && a.wrote == 0x180 && std::strcmp(reinterpret_cast<const char *>(r.at(0)), kId.driverVersion) == 0);
    CHECK(r.at(0x17f)[0] == 0 && r.at(0x180)[0] == 0xa5);   // nothing beyond the buffer
    a = r.ask(false, kHardwareVersion, 0x180);
    CHECK(a.error == 0 && std::strcmp(reinterpret_cast<const char *>(r.at(0)), "WCN7860") == 0);
    // the bind's only sanity request on the ioctl path: CARD_CAPABILITIES with 23 bytes, a raw bitmap from offset 0
    std::memset(r.user.mem.data(), 0xa5, 64);
    a = r.ask(false, kCardCapabilities, 23);
    CHECK(a.error == 0 && a.wrote == 23 && r.rq.length == 23 && r.at(23)[0] == 0xa5);
    for (uint8_t b : kCapBits) CHECK(r.at(0)[b / 8] & (1u << (b % 8)));
    CHECK((r.at(0)[0] & 0x1) == 0);                          // no WEP
    CHECK((r.at(0)[46 / 8] & (1u << (46 % 8))) == 0);        // no wait for a scan-done event
    CHECK((r.at(0)[73 / 8] & (1u << (73 % 8))) == 0);        // directed scans as SCAN_REQ
    a = r.ask(false, kStatusDevName, 1028);
    CHECK(a.error == 0 && std::strcmp(reinterpret_cast<const char *>(r.at(0)), "PianoWifi") == 0);
    // a get without a buffer is refused (the bind always gives one)
    a = r.ask(false, kDriverVersion, 0x180, false);
    CHECK(a.error == kErrInval);
    // a buffer outside user space: the copy's fault
    r.rq = {};
    r.rq.type = kDriverVersion;
    r.rq.length = 0x180;
    r.rq.data = 0x1000;
    a = handle(r.st, kId, false, r.rq, r.io, r.scratch.data());
    CHECK(a.error == kErrFault);
  }

  // ---- power: on by default; off answers EPWROFF to scans, on again restores them; each set asks for the event
  {
    Rig r;
    Answer a = r.ask(false, kPower, 24);
    CHECK(a.error == 0 && r.u32(0) == 1 && r.u32(4) == 1 && r.u32(8) == 1);
    std::memset(r.user.mem.data(), 0, 24);
    r.put32(0, 1);
    r.put32(4, 1);
    r.put32(8, 0);
    a = r.ask(true, kPower, 24);
    CHECK(a.error == 0 && (a.effects & kDoPowerChanged) && !r.st.power && r.st.powerSets == 1);
    a = r.ask(false, kPower, 24);
    CHECK(r.u32(8) == 0);
    a = r.ask(true, kScanReq, 0x80);
    CHECK(a.error == kErrPowerOff && !(a.effects & kDoScanStarted));
    a = r.ask(false, kScanResult, sizeof(ScanResult));
    CHECK(a.error == kErrPowerOff);
    CHECK(r.ask(false, kState, 0, false).error == 0 && r.rq.value == int32_t(kStateInit));
    r.put32(8, 1);
    a = r.ask(true, kPower, 24);
    CHECK(a.error == 0 && r.st.power && (a.effects & kDoPowerChanged));
    a = r.ask(true, kScanReq, 0x80);
    CHECK(a.error == 0 && (a.effects & kDoScanStarted));
    // a short buffer: what fits of the answer
    a = r.ask(false, kPower, 12);
    CHECK(a.error == 0 && a.wrote == 12);
  }

  // ---- the idle station: state, mode, PHY, noise, and the not-associated answers
  {
    Rig r;
    CHECK(r.ask(false, kState, 0, false).error == 0 && r.rq.value == int32_t(kStateInit));
    CHECK(r.ask(false, kState, 8).error == 0 && r.u32(0) == 1 && r.u32(4) == kStateInit);
    CHECK(r.ask(false, kOpMode, 0, false).error == 0 && r.rq.value == int32_t(kOpModeStation));
    CHECK(r.ask(false, kPhyMode, 12).error == 0 && r.u32(4) == kPhySupported && r.u32(8) == 0);
    CHECK(r.ask(false, kNoise, 52).error == 0 && int32_t(r.u32(12)) == -95 && int32_t(r.u32(28)) == -95);
    for (int32_t t : {kRssi, kChannel, kSsid, kBssid, kCurrentNetwork, kRate}) CHECK(r.ask(false, t, 1164).error == kErrNotAssociated);
    CHECK(r.ask(false, kAssociateResult, 0, false).error == 0 && r.rq.value == 0);
    Answer a = r.ask(false, kCountryCode, 3);
    CHECK(a.error == 0 && std::memcmp(r.at(0), "XX", 3) == 0 && r.rq.length == 3);
    // unknown requests, both ways
    CHECK(r.ask(false, kInfraEnumerated, 16).error == kErrUnsupported);
    CHECK(r.ask(false, kVirtualIfRole, 8).error == kErrUnsupported);   // the infrastructure interface is not a virtual one
    CHECK(r.ask(true, kInfraEnumerated, 8).error == kErrUnsupported);  // airportd ignores this set's result
    CHECK(r.ask(true, kAssociate, 656).error == kErrUnsupported);
    CHECK(r.ask(true, kCipherKey, 148).error == kErrUnsupported);
    CHECK(r.ask(false, 9999, 16).error == kErrUnsupported);
    // accepted sets that change nothing
    CHECK(r.ask(true, kCountryCode, 8).error == 0 && r.ask(true, kPowerSave, 8).error == 0);
  }

  // ---- channels: the 22 this station scans, each with its band
  {
    Rig r;
    Answer a = r.ask(false, kSupportedChannels, 4824);
    CHECK(a.error == 0 && r.u32(0) == 1 && r.u32(4) == 22 && a.wrote == 8 + 12 * 22);
    CHECK(r.u32(8) == 1 && r.u32(12) == 1 && r.u32(16) == (kChan20MHz | kChan2GHz | kChanActive));
    CHECK(r.u32(8 + 12 * 13 + 4) == 36 && r.u32(8 + 12 * 13 + 8) == (kChan20MHz | kChan5GHz | kChanActive));
    CHECK(r.u32(8 + 12 * 21 + 4) == 165);
    a = r.ask(false, kHwSupportedChannels, 776);
    CHECK(a.error == 0 && r.u32(4) == 22);
  }

  // ---- the scan: SCAN_REQ, then SCAN_RESULT once per network, then the end (EIO) and the cursor back at the start
  {
    Rig r;
    r.st.scan.bss[0] = network(1, "home-5g", 5180, -48, true);
    r.st.scan.bss[1] = network(2, "cafe", 2437, -71, false);
    r.st.scan.count = 2;
    std::memset(r.user.mem.data(), 0, 0x80);
    r.put32(kScanReqSsidLengthAt, 4);
    std::memcpy(r.user.mem.data() + kScanReqSsidAt, "cafe", 4);
    r.rq = {};
    r.rq.type = kScanReq;
    r.rq.length = 0x80;
    r.rq.data = User::kBase;
    CHECK(scanReqSsidLength(r.rq, r.io) == 4);
    Answer a = r.ask(true, kScanReq, 5528);
    CHECK(a.error == 0 && (a.effects & kDoScanStarted) && r.st.scanRequests == 1);
    // 27.3's 2280-byte record: the head and the elements written, its tail (a request identifier user space put at 0x88c) kept
    std::memset(r.user.mem.data(), 0, kScanResultBytes27);
    std::memcpy(r.user.mem.data() + 0x88c, "request-7", 10);
    a = r.ask(false, kScanResult, kScanResultBytes27);
    CHECK(a.error == 0 && a.wrote == 0x8c + 47 && std::memcmp(r.at(0x88c), "request-7", 10) == 0);
    ScanResult got;
    std::memcpy(&got, r.at(0), sizeof got);
    CHECK(got.version == 1 && got.channel.channel == 36 && (got.channel.flags & kChan5GHz) && got.rssi == -48 && got.noise == -95);
    CHECK(got.snr == 47 && got.ssidLength == 7 && std::memcmp(got.ssid, "home-5g", 7) == 0 && got.bssid[5] == 1);
    CHECK(got.capability == 0x11 && got.beaconInterval == 100 && got.rateCount == 12 && got.rates[0] == 1 && got.rates[11] == 54);
    // the elements: SSID, 8 rates, 4 extended rates, the RSN element (no DS parameter set at 5 GHz)
    const uint8_t *e = got.elements;
    CHECK(e[0] == 0 && e[1] == 7 && std::memcmp(e + 2, "home-5g", 7) == 0);
    CHECK(e[9] == 1 && e[10] == 8 && e[11] == 0x82);
    CHECK(e[19] == 50 && e[20] == 4 && e[21] == 0x30);
    CHECK(e[25] == 48 && e[26] == 20 && e[27] == 1);
    CHECK(got.elementsLength == 25 + 22);
    a = r.ask(false, kScanResult, sizeof(ScanResult));
    std::memcpy(&got, r.at(0), sizeof got);
    CHECK(a.error == 0 && got.channel.channel == 6 && (got.channel.flags & kChan2GHz) && got.capability == 0x1);
    // 2.4 GHz: the DS parameter set (channel 6) after the rates
    CHECK(got.elements[0] == 0 && got.elements[6] == 1 && got.elements[16] == 3 && got.elements[17] == 1 && got.elements[18] == 6);
    CHECK(got.elementsLength == 6 + 10 + 3 + 6);
    a = r.ask(false, kScanResult, sizeof(ScanResult));
    CHECK(a.error == kErrEndOfResults && r.st.cursor == 0 && r.st.listEnds == 1);
    // and again from the first
    a = r.ask(false, kScanResult, sizeof(ScanResult));
    std::memcpy(&got, r.at(0), sizeof got);
    CHECK(a.error == 0 && got.bssid[5] == 1);
    // a short buffer: the head of the record
    a = r.ask(false, kScanResult, 0x8c);
    CHECK(a.error == 0 && a.wrote == 0x8c);
    CHECK(resultBytes(got) == 0x8c + got.elementsLength);
    // SCANCACHE_CLEAR: nothing until the next scan; then the list again
    CHECK(r.ask(true, kScanCacheClear, 0, false).error == 0);
    CHECK(r.ask(false, kScanResult, sizeof(ScanResult)).error == kErrEndOfResults);
    r.st.scan.count = 2;   // (the kext refreshes the snapshot before a SCAN_REQ)
    CHECK(r.ask(true, kScanReqMultiple, 0x400).error == 0);
    CHECK(r.ask(false, kScanResult, sizeof(ScanResult)).error == 0);
    // an empty list: the end at once
    Rig e2;
    CHECK(e2.ask(true, kScanReq, 0x80).error == 0 && e2.ask(false, kScanResult, 1164).error == kErrEndOfResults);
  }

  // ---- the registry's address form; what is not a whole address is refused
  {
    uint8_t m[6];
    CHECK(parseBssid("3c:52:a1:10:20:fe", m) && m[0] == 0x3c && m[5] == 0xfe);
    CHECK(parseBssid("3C:52:A1:10:20:FE", m) && m[0] == 0x3c);
    CHECK(!parseBssid("3c:52:a1:**:**:**", m) && !parseBssid("3c:52:a1:10:20", m) && !parseBssid("3c:52:a1:10:20:fe:00", m));
    CHECK(!parseBssid(nullptr, m) && !parseBssid("", m));
  }

  // ---- the elements fit the space they are given (a long name and a long RSN element never overrun)
  {
    Bss b = network(3, "0123456789abcdef0123456789abcdef", 2412, -60, true);
    uint8_t out[40];
    std::memset(out, 0xee, sizeof out);
    const uint32_t n = buildElements(b, out, 38);
    CHECK(n <= 38 && out[0] == 0 && out[1] == 32 && out[38] == 0xee && out[39] == 0xee);
    b.rsn[1] = 200;   // a length the element does not have: left out
    uint8_t big[400];
    CHECK(buildElements(b, big, sizeof big) == 34 + 10 + 3 + 6);
  }

  // ---- the log's table: a kind once, its repeats not, a full table counted
  {
    Seen s;
    CHECK(s.first(kDriverVersion, false, 0x180, 0) && !s.first(kDriverVersion, false, 0x180, 0));
    CHECK(s.first(kDriverVersion, false, 0x180, kErrFault) && s.first(kDriverVersion, true, 0x180, kErrUnsupported));
    CHECK(s.first(kScanResult, false, 1164, 0) && s.first(kScanResult, false, 1164, kErrEndOfResults));
    for (int i = 0; i < 400; ++i) s.first(1000 + i, false, 4, 0);
    CHECK(s.count == Seen::kMax && s.overflow > 0);
  }

  if (failures) {
    std::printf("apple80211_host: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("apple80211_host: all checks passed\n");
  return 0;
}
