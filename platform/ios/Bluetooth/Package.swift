// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "Bluetooth",
    platforms: [
        .iOS(.v15),
    ],
    products: [
        .library(
            name: "Bluetooth",
            targets: ["Bluetooth"]
        ),
    ],
    targets: [
        .target(name: "Bluetooth"),
    ]
)
