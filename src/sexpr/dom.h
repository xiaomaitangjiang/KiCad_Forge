#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kforge::sexpr {

/// A node in the S-expression DOM tree.
///
/// KiCAD S-expression structure (recursive):
///   (type_name
///     (property_name "value")
///     (child_node ...))
///
/// The first atom after '(' becomes the type name.
/// Subsequent atoms become property-value pairs.
/// Sub-lists become child nodes.
class DomNode {
public:
    using Ptr = std::unique_ptr<DomNode>;

    DomNode() = default;
    explicit DomNode(std::string type) : type_(std::move(type)) {}

    // --- Type ---
    const std::string& type() const { return type_; }
    void set_type(std::string t) { type_ = std::move(t); }

    // --- Atom value (for leaf atoms in property position) ---
    bool is_atom() const { return is_atom_; }
    const std::string& atom_value() const { return atom_value_; }
    void set_atom(std::string v) {
        is_atom_ = true;
        atom_value_ = std::move(v);
    }

    // --- Properties (atom-value pairs: (key value)) ---
    void add_property(std::string key, std::string value) {
        properties_.emplace(std::move(key), std::move(value));
    }
    std::optional<std::string_view> property(const std::string& key) const {
        auto it = properties_.find(key);
        if (it != properties_.end()) return it->second;
        return std::nullopt;
    }
    const std::unordered_map<std::string, std::string>& properties() const {
        return properties_;
    }
    bool has_property(const std::string& key) const {
        return properties_.count(key) > 0;
    }

    // --- Children ---
    DomNode* add_child(std::unique_ptr<DomNode> child) {
        children_.push_back(std::move(child));
        return children_.back().get();
    }
    DomNode* add_child(std::string type) {
        auto child = std::make_unique<DomNode>(std::move(type));
        auto* ptr = child.get();
        children_.push_back(std::move(child));
        return ptr;
    }
    const std::vector<Ptr>& children() const { return children_; }

    // --- Traversal helpers ---
    DomNode* find_child(std::string_view type) {
        for (auto& c : children_) {
            if (c->type() == type) return c.get();
        }
        return nullptr;
    }
    const DomNode* find_child(std::string_view type) const {
        for (auto& c : children_) {
            if (c->type() == type) return c.get();
        }
        return nullptr;
    }
    std::vector<DomNode*> find_children(std::string_view type) {
        std::vector<DomNode*> result;
        for (auto& c : children_) {
            if (c->type() == type) result.push_back(c.get());
        }
        return result;
    }

private:
    std::string type_;
    bool is_atom_{false};
    std::string atom_value_;
    std::unordered_map<std::string, std::string> properties_;
    std::vector<Ptr> children_;
};

}  // namespace kforge::sexpr
