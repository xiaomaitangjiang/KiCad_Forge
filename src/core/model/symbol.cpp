#include "core/model/symbol.h"

// XXH64 one-shot — single header, define implementation in this TU only
#define XXH_INLINE_ALL
#include "../third_party/xxhash.h"

namespace kforge::core {

namespace {

// Length-prefixed fields make the canonical byte stream unambiguous: moving a
// separator character between name and footprint must not collide.
void append_field(std::string& out, std::string_view value) {
    out += std::to_string(value.size());
    out += ':';
    out.append(value);
}

}  // namespace

std::string Symbol::compute_hash(const Symbol& sym) {
    // Identity-only canonical stream. See the declaration in symbol.h for the
    // rationale: metadata edits must not change the version fingerprint.
    std::string input;
    append_field(input, "kforge-symbol-identity-v1");
    append_field(input, sym.name());
    append_field(input, std::to_string(sym.pins().size()));
    for (const auto& p : sym.pins()) {
        append_field(input, p.number);
        append_field(input, p.name);
        append_field(input, p.electrical_type);
    }
    append_field(input, sym.footprint());

    XXH64_hash_t h = XXH64(input.data(), input.size(), 0);
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)h);
    return std::string("KF") + buf;
}

void Symbol::refresh_Kicad_Forge_ID() {
    std::string next = compute_hash(*this);
    if (next == kf_id_) return;
    if (!kf_id_.empty()) pre_kf_id_ = kf_id_;
    kf_id_ = std::move(next);
}

void Symbol::regenerate_Kicad_Forge_ID() {
    pre_kf_id_ = std::move(kf_id_);
    kf_id_ = compute_hash(*this);
}

}  // namespace kforge::core
