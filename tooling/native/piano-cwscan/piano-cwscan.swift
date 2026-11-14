// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano-cwscan: what CoreWLAN (through airportd) sees of the Wi-Fi interface, for the apple80211 milestones
// (docs/drivers/wifi-menu.md sections 5.8 and 6): the interface names, the power, the mode, and one scan's count and error. Never a
// network's name or address: counts by band and security only, so its output can go into a log or a report.
//   swiftc -O -target arm64-apple-macos14 tooling/native/piano-cwscan/piano-cwscan.swift -o out/piano-cwscan/piano-cwscan
//   piano-cwscan [scan] [SECONDS]   (without "scan": no scan is asked, only the state)
// It ends itself after SECONDS (default 40; exit status 3 and a "gave up" line): on the M1 boot a CoreWLAN call without a usable
// interface never returned and held the session's ssh.
import CoreWLAN
import Foundation

let args = Array(CommandLine.arguments.dropFirst())
let limit = args.compactMap { Double($0) }.first ?? 40
let started = Date()
let watchdog = Thread {
  Thread.sleep(forTimeInterval: limit)
  print("piano-cwscan: gave up after \(Int(limit)) s (a CoreWLAN call did not return)")
  fflush(stdout)
  exit(3)
}
watchdog.start()

let client = CWWiFiClient.shared()
let names = client.interfaceNames() ?? []
print("piano-cwscan: interfaces \(names.count): \(names.joined(separator: " "))")
fflush(stdout)
guard let wifi = client.interface() else {
  print("piano-cwscan: no default Wi-Fi interface")
  exit(1)
}
print("piano-cwscan: \(wifi.interfaceName ?? "?"): power \(wifi.powerOn() ? "on" : "off"), mode \(wifi.interfaceMode().rawValue), "
  + "service active \(wifi.serviceActive()), country \(wifi.countryCode() ?? "-"), channels \(wifi.supportedWLANChannels()?.count ?? 0)")
fflush(stdout)
if args.contains("scan") {
  do {
    let found = try wifi.scanForNetworks(withSSID: nil)
    var bands: [String: Int] = [:]
    var secured = 0
    for n in found {
      let band = n.wlanChannel?.channelBand == .band5GHz ? "5GHz" : n.wlanChannel?.channelBand == .band2GHz ? "2.4GHz" : "other"
      bands[band, default: 0] += 1
      if !n.supportsSecurity(.none) { secured += 1 }
    }
    let ms = Int(Date().timeIntervalSince(started) * 1000)
    print("piano-cwscan: scan: \(found.count) network(s) in \(ms) ms, by band \(bands.sorted { $0.key < $1.key }), \(secured) secured")
  } catch {
    let e = error as NSError
    print("piano-cwscan: scan failed: domain \(e.domain) code \(e.code)")
    exit(2)
  }
}
exit(0)
