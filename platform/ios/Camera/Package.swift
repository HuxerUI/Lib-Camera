// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "Camera",
    platforms: [
        .iOS(.v15),
    ],
    products: [
        .library(
            name: "Camera",
            targets: ["Camera"]
        ),
    ],
    targets: [
        .target(
            name: "Camera",
            linkerSettings: [
                .linkedFramework("Accelerate"),
                .linkedFramework("AVFoundation"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("CoreImage"),
                .linkedFramework("ImageIO"),
                .linkedFramework("UIKit"),
                .linkedFramework("AudioToolbox"),
                .linkedFramework("VideoToolbox"),
                .linkedFramework("PhotosUI"),
                .linkedFramework("UniformTypeIdentifiers"),
            ]
        ),
    ]
)
