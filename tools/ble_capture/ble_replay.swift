// ble_replay: advertise a captured list of service UUIDs for a short time, to
// test whether the fan acts on a replayed packet.
//
//   swiftc -O ble_replay.swift -o ble_replay
//   ./ble_replay [-t SECONDS] AABBCCDD,11223344,55667788
//
// The UUID list may also be pasted straight from a ble_scan line ("uuids=...").

import CoreBluetooth
import Foundation

setvbuf(stdout, nil, _IOLBF, 0)

var seconds = 1.5
var raw: [String] = []
var args = CommandLine.arguments.dropFirst().makeIterator()
while let a = args.next() {
    if a == "-t" { seconds = Double(args.next() ?? "") ?? seconds } else { raw.append(a) }
}
let list = raw.joined(separator: ",")
    .replacingOccurrences(of: "uuids=", with: "")
    .split(whereSeparator: { $0 == "," || $0 == " " })
    .map(String.init)
if list.isEmpty {
    print("usage: ble_replay [-t SECONDS] UUID,UUID,...")
    exit(2)
}
let uuids = list.map { CBUUID(string: $0) }

final class Advertiser: NSObject, CBPeripheralManagerDelegate {
    private var manager: CBPeripheralManager!

    override init() {
        super.init()
        manager = CBPeripheralManager(delegate: self, queue: .main)
    }

    func peripheralManagerDidUpdateState(_ p: CBPeripheralManager) {
        switch p.state {
        case .poweredOn:
            p.startAdvertising([CBAdvertisementDataServiceUUIDsKey: uuids])
        case .unauthorized:
            print("Bluetooth access denied: enable your terminal app in System Settings > Privacy & Security > Bluetooth")
            exit(1)
        case .poweredOff:
            print("Bluetooth is off")
            exit(1)
        default:
            break
        }
    }

    func peripheralManagerDidStartAdvertising(_ p: CBPeripheralManager, error: Error?) {
        if let error {
            print("advertising failed: \(error.localizedDescription)")
            exit(1)
        }
        print("advertising \(uuids.map(\.uuidString).joined(separator: ",")) for \(seconds) s")
        DispatchQueue.main.asyncAfter(deadline: .now() + seconds) {
            p.stopAdvertising()
            exit(0)
        }
    }
}

let advertiser = Advertiser()
dispatchMain()
