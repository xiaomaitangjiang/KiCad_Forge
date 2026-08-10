#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace kforge::sexpr {

/// A node in the S-expression DOM tree — zero-copy into source buffer.
///
/// All string fields are string_views pointing into the source text.
/// The source buffer MUST outlive the DOM tree.
class DomNode {
public:
    using Ptr = std::unique_ptr<DomNode>;

    DomNode() = default;
    explicit DomNode(std::string_view type) : type_(type) {}

    // --- Type ---
    std::string_view type() const { return type_; }
    void set_type(std::string_view t) { type_ = t; }

    // --- Atom value (for leaf atoms in property position) ---
    bool is_atom() const { return is_atom_; }
    std::string_view atom_value() const { return atom_value_; }
    void set_atom(std::string_view v) {
        is_atom_ = true;
        atom_value_ = v;
    }

    // --- Properties (key-value pairs) — linear scan, fast for typical 0-5 entries ---
    void add_property(std::string_view key, std::string_view value) {
        properties_.emplace_back(key, value);
    }
    std::optional<std::string_view> property(std::string_view key) const {
        for (const auto& [k, v] : properties_)
        {
            if (k == key) { return v; }
        }
        return std::nullopt;
    }
    const auto& properties() const { return properties_; }
    bool has_property(std::string_view key) const {
        return property(key).has_value();
    }

    // --- Children ---
    DomNode* add_child(std::unique_ptr<DomNode> child) {
        children_.push_back(std::move(child));
        return children_.back().get();
    }
    DomNode* add_child(std::string_view type) {
        auto child = std::make_unique<DomNode>(type);
        auto* ptr = child.get();
        children_.push_back(std::move(child));
        return ptr;
    }
    const std::vector<Ptr>& children() const { return children_; }

    // --- Traversal helpers ---
    DomNode* find_child(std::string_view type) {
        for (auto& c : children_)
        {
            if (c->type() == type) { return c.get(); }
        }
        return nullptr;
    }
    const DomNode* find_child(std::string_view type) const {
        for (auto& c : children_)
        {
            if (c->type() == type) { return c.get(); }
        }
        return nullptr;
    }
    std::vector<DomNode*> find_children(std::string_view type) {
        std::vector<DomNode*> result;
        for (auto& c : children_)
        {
            if (c->type() == type) { result.push_back(c.get()); }
        }
        return result;
    }

private:
    std::string_view type_;
    bool is_atom_{false};
    std::string_view atom_value_;
    std::vector<std::pair<std::string_view, std::string_view>> properties_;
    std::vector<Ptr> children_;
};

}  // namespace kforge::sexpr
