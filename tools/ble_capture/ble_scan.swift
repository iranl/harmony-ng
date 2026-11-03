// ble_scan: log BLE advertisements and flag payloads not seen before, so the
// packets sent by an app or remote button press stand out from the background.
//
//   swiftc -O ble_scan.swift -o ble_scan
//   ./ble_scan [-r MIN_RSSI] [-f SUBSTRING] [--apple] [-v] | tee scan.log
//
// While it runs, type a label and press Enter (e.g. "light on") just before
// pressing the button; a marker line is printed so new payloads can be tied
// to the action that produced them.

import CoreBluetooth
import Foundation

setvbuf(stdout, nil, _IOLBF, 0)

struct Options {
    var minRSSI = -70
    var filter: String?
    var includeApple = false
    var verbose = false
}

func parseOptions() -> Options {
    var o = Options()
    var args = CommandLine.arguments.dropFirst().makeIterator()
    while let a = args.next() {
        switch a {
        case "-r": o.minRSSI = Int(args.next() ?? "") ?? o.minRSSI
        case "-f": o.filter = args.next()
        case "--apple": o.includeApple = true
        case "-v": o.verbose = true
        default:
            print("usage: ble_scan [-r MIN_RSSI] [-f SUBSTRING] [--apple] [-v]")
            print("  -r       ignore packets weaker than MIN_RSSI dBm (default -70)")
            print("  -f       only show packets whose payload or device id contains SUBSTRING")
            print("  --apple  include Apple Continuity packets (manufacturer 0x004C)")
            print("  -v       print every packet, not only new payloads")
            exit(a == "-h" || a == "--help" ? 0 : 2)
        }
    }
    return o
}

let opts = parseOptions()
let start = Date()

func log(_ s: String) {
    print(String(format: "%8.2f  ", Date().timeIntervalSince(start)) + s)
}

func hex(_ d: Data) -> String {
    d.map { String(format: "%02x", $0) }.joined()
}

final class Scanner: NSObject, CBCentralManagerDelegate {
    private var central: CBCentralManager!
    private var seen: [String: Int] = [:]

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        switch c.state {
        case .poweredOn:
            c.scanForPeripherals(withServices: nil,
                                 options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
            log("scanning (min RSSI \(opts.minRSSI) dBm); type a label + Enter to mark an action")
        case .unauthorized:
            log("Bluetooth access denied: enable your terminal app in System Settings > Privacy & Security > Bluetooth")
            exit(1)
        case .poweredOff:
            log("Bluetooth is off")
        default:
            log("Bluetooth state \(c.state.rawValue)")
        }
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData ad: [String: Any], rssi: NSNumber) {
        let r = rssi.intValue
        if r == 127 || r < opts.minRSSI { return }

        var parts: [String] = []
        if let n = ad[CBAdvertisementDataLocalNameKey] as? String { parts.append("name=\(n)") }
        if let m = ad[CBAdvertisementDataManufacturerDataKey] as? Data {
            let bytes = [UInt8](m)
            if bytes.count >= 2, bytes[0] == 0x4C, bytes[1] == 0x00, !opts.includeApple { return }
            parts.append("mfg=\(hex(m))")
        }
        if let u = ad[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] {
            parts.append("uuids=" + u.map(\.uuidString).joined(separator: ","))
        }
        if let u = ad[CBAdvertisementDataOverflowServiceUUIDsKey] as? [CBUUID] {
            parts.append("overflow=" + u.map(\.uuidString).joined(separator: ","))
        }
        if let u = ad[CBAdvertisementDataSolicitedServiceUUIDsKey] as? [CBUUID] {
            parts.append("solicit=" + u.map(\.uuidString).joined(separator: ","))
        }
        if let sd = ad[CBAdvertisementDataServiceDataKey] as? [CBUUID: Data] {
            for (k, v) in sd.sorted(by: { $0.key.uuidString < $1.key.uuidString }) {
                parts.append("svc[\(k.uuidString)]=\(hex(v))")
            }
        }
        if let t = ad[CBAdvertisementDataTxPowerLevelKey] as? NSNumber { parts.append("tx=\(t)") }
        if parts.isEmpty { parts.append("(empty)") }

        let payload = parts.joined(separator: " ")
        let id = p.identifier.uuidString
        if let f = opts.filter,
           !payload.localizedCaseInsensitiveContains(f), !id.hasPrefix(f.uppercased()) { return }

        let connectable = (ad[CBAdvertisementDataIsConnectable] as? NSNumber)?.boolValue ?? false
        let key = id + "|" + payload
        let count = (seen[key] ?? 0) + 1
        seen[key] = count
        if count == 1 || opts.verbose {
            let tag = count == 1 ? "NEW " : "    "
            log(String(format: "%@ %@ %4d dBm %@ %@",
                       tag, String(id.prefix(8)), r, connectable ? "C" : "-", payload))
        }
    }
}

let scanner = Scanner()

var marks = 0
Thread {
    while let line = readLine() {
        DispatchQueue.main.async {
            marks += 1
            log("===== MARK \(marks): \(line) =====")
        }
    }
}.start()

dispatchMain()
