#include "core/symbol.h"

// XXH64 one-shot — single header, define implementation in this TU only
#define XXH_INLINE_ALL
#include "../third_party/xxhash.h"

namespace kforge::core {

std::string Symbol::compute_hash(const Symbol& sym) {
    // Build a canonical string: name | pinSig | footprint | value
    std::string input = sym.name();
    input += "|";
    for (auto& p : sym.pins()) {
        input += p.number + "/" + p.name + "/" + p.electrical_type + "|";
    }
    input += sym.footprint() + "|" + sym.default_value();

    XXH64_hash_t h = XXH64(input.data(), input.size(), 0);
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)h);
    return std::string("KF") + buf;
}

void Symbol::regenerate_Kicad_Forge_ID() {
    pre_kf_id_ = std::move(kf_id_);
    kf_id_ = compute_hash(*this);
}

}  // namespace kforge::core
