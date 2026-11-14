// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano-cwscan: what CoreWLAN (through airportd) sees of the Wi-Fi interface, for the apple80211 milestones
// (docs/drivers/wifi-menu.md section 5.8): the interface names, the power, the mode, and one scan's count and error. Never a
// network's name or address: counts by band and security only, so its output can go into a log or a report.
//   swiftc -O -target arm64-apple-macos14 tooling/native/piano-cwscan/piano-cwscan.swift -o out/piano-cwscan/piano-cwscan
//   piano-cwscan [scan]      (without "scan": no scan is asked, only the state)
import CoreWLAN
import Foundation

let client = CWWiFiClient.shared()
let names = client.interfaceNames() ?? []
print("piano-cwscan: interfaces \(names.count): \(names.joined(separator: " "))")
guard let wifi = client.interface() else {
  print("piano-cwscan: no default Wi-Fi interface")
  exit(1)
}
print("piano-cwscan: \(wifi.interfaceName ?? "?"): power \(wifi.powerOn() ? "on" : "off"), mode \(wifi.interfaceMode().rawValue), "
  + "service active \(wifi.serviceActive()), country \(wifi.countryCode() ?? "-"), channels \(wifi.supportedWLANChannels()?.count ?? 0)")
if CommandLine.arguments.dropFirst().contains("scan") {
  let started = Date()
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
