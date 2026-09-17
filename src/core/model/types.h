#pragma once

#include <cstdint>
#include <string>
#include <filesystem>
#include <vector>
#include <optional>
#include <unordered_map>

namespace kforge::core {

// ============================================================
// ID types
// ============================================================
using Uuid = std::string;  // QUuid-compatible string format

// ============================================================
// Component type classification
// ============================================================
enum class ComponentType {
    Resistor,
    Capacitor,
    Inductor,
    Diode,
    LED,
    Transistor,
    MOSFET,
    JFET,
    VoltageRegulator,
    OpAmp,
    Comparator,
    LogicGate,
    Microcontroller,
    FPGA,
    Memory,
    Connector,
    Switch,
    Fuse,
    Crystal,
    Oscillator,
    Transformer,
    Relay,
    Jumper,
    TestPoint,
    MountingHole,
    FerriteBead,
    Varistor,
    Thermistor,
    Photodiode,
    Phototransistor,
    TRIAC,
    SCR,
    Unknown,
};

enum class PackageType {
    // SMD chip
    SMD_0201, SMD_0402, SMD_0603, SMD_0805, SMD_1206,
    SMD_1210, SMD_1812, SMD_2010, SMD_2512,
    // SMD IC
    SOT_23, SOT_223, SOT_363,
    SOIC_8, SOIC_14, SOIC_16, SOIC_20, SOIC_24, SOIC_28,
    SSOP, TSSOP, MSOP,
    QFP_32, QFP_44, QFP_64, QFP_100, QFP_144,
    QFN_16, QFN_20, QFN_24, QFN_32, QFN_48, QFN_64,
    BGA, LGA, PLCC,
    // Through-hole
    THT_Axial, THT_Radial, THT_DIP,
    DIP_8, DIP_14, DIP_16, DIP_20, DIP_24, DIP_28, DIP_40,
    // Connectors
    USB_A, USB_C, USB_Micro, USB_Mini,
    RJ45, RJ11,
    PinHeader, PinSocket,
    TerminalBlock,
    // Other
    Custom, Unknown,
};

// ============================================================
// Value types
// ============================================================
struct ValueRange {
    double min{0.0};
    double max{0.0};
    std::string unit;
};

struct PinDefinition {
    std::string number;
    std::string name;
    std::string electrical_type;  // input, output, bidirectional, power_in, etc.
    double x{0.0};
    double y{0.0};
};

// ============================================================
// Library metadata
// ============================================================
struct LibraryMeta {
    Uuid id;
    std::string name;
    std::filesystem::path file_path;
    std::string description;
    std::string component_library_id;
    int symbol_count{0};
    int footprint_count{0};
};

// ============================================================
// Generic component library container
// ============================================================
template <typename T>
struct Library {
    std::string name;
    std::filesystem::path file_path;
    std::vector<T> items;

    size_t size() const { return items.size(); }
    bool empty() const { return items.empty(); }
};

}  // namespace kforge::core
