#pragma once

#include <flutter/encodable_value.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace jaw {
using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

// Own the text: MSVC's exception classes can retain a dangling c_str() when
// Flutter builds with _HAS_EXCEPTIONS=0.
struct ArgumentError { std::string message; };

inline const EncodableValue* Find(const EncodableMap& map, const char* key) {
  auto it = map.find(EncodableValue(key));
  return it == map.end() ? nullptr : &it->second;
}
template <typename T>
const T* Get(const EncodableMap& map, const char* key) {
  auto value = Find(map, key);
  return value ? std::get_if<T>(value) : nullptr;
}
template <typename T>
const T& Require(const EncodableMap& map, const char* key) {
  auto value = Get<T>(map, key);
  if (!value) throw ArgumentError{std::string("Missing or invalid ") + key};
  return *value;
}
inline int64_t Integer(const EncodableValue& value) {
  if (auto n = std::get_if<int32_t>(&value)) return *n;
  if (auto n = std::get_if<int64_t>(&value)) return *n;
  throw ArgumentError{"Expected an integer"};
}
inline int64_t Integer(const EncodableMap& map, const char* key) {
  auto value = Find(map, key);
  if (!value) throw ArgumentError{std::string("Missing ") + key};
  return Integer(*value);
}
inline int64_t OptionalInteger(const EncodableMap& map, const char* key,
                               int64_t fallback = 0) {
  auto value = Find(map, key);
  return !value || value->IsNull() ? fallback : Integer(*value);
}

struct SourceNode {
  EncodableMap data;
  std::string id;
  std::string type;
  std::vector<SourceNode> children;
  std::vector<size_t> shuffle;
  size_t count = 1;

  static std::vector<size_t> Permutation(const EncodableList& list, size_t size) {
    if (list.size() != size) throw ArgumentError{"shuffleOrder has incorrect length"};
    std::vector<size_t> order;
    std::vector<bool> seen(size, false);
    for (const auto& value : list) {
      auto index = Integer(value);
      if (index < 0 || static_cast<uint64_t>(index) >= size || seen[static_cast<size_t>(index)])
        throw ArgumentError{"shuffleOrder must be a permutation"};
      seen[static_cast<size_t>(index)] = true;
      order.push_back(static_cast<size_t>(index));
    }
    return order;
  }
  static SourceNode Parse(const EncodableMap& data, size_t depth = 0) {
    if (depth > 64) throw ArgumentError{"Audio source nesting is too deep"};
    SourceNode node;
    node.data = data;
    node.id = Require<std::string>(data, "id");
    node.type = Require<std::string>(data, "type");
    if (node.type == "concatenating") {
      for (const auto& child : Require<EncodableList>(data, "children")) {
        auto map = std::get_if<EncodableMap>(&child);
        if (!map) throw ArgumentError{"Source child must be a map"};
        node.children.push_back(Parse(*map, depth + 1));
      }
      node.shuffle = Permutation(Require<EncodableList>(data, "shuffleOrder"), node.children.size());
    } else if (node.type == "looping") {
      auto count = Integer(data, "count");
      if (count < 0 || count > 100000) throw ArgumentError{"Invalid looping count"};
      node.count = static_cast<size_t>(count);
      node.children.push_back(Parse(Require<EncodableMap>(data, "child"), depth + 1));
    } else if (node.type == "clipping") {
      const auto& child = Require<EncodableMap>(data, "child");
      auto leaf = Parse(child, depth + 1);
      if (leaf.type != "progressive" && leaf.type != "hls" && leaf.type != "dash")
        throw ArgumentError{"Clipping requires a URI source"};
      auto start = OptionalInteger(data, "start");
      auto end = OptionalInteger(data, "end", -1);
      if (start < 0 || (end != -1 && end <= start)) throw ArgumentError{"Invalid clipping range"};
    } else if (node.type == "progressive" || node.type == "hls" || node.type == "dash") {
      if (Require<std::string>(data, "uri").empty()) throw ArgumentError{"Source URI is empty"};
      if (auto headers = Find(data, "headers"); headers && !headers->IsNull()) {
        auto map = std::get_if<EncodableMap>(headers);
        if (!map) throw ArgumentError{"headers must be a map"};
        if (!map->empty()) throw ArgumentError{"Direct HTTP headers are unsupported; use just_audio's proxy"};
      }
    } else {
      throw ArgumentError{"Unsupported audio source type: " + node.type};
    }
    return node;
  }
  size_t Size() const {
    if (type == "looping") {
      auto size = children[0].Size();
      if (size && count > 100000 / size) throw ArgumentError{"Audio source is too large"};
      return count * size;
    }
    if (type != "concatenating") return 1;
    size_t size = 0;
    for (const auto& child : children) {
      size += child.Size();
      if (size > 100000) throw ArgumentError{"Audio source is too large"};
    }
    return size;
  }
  void Flatten(std::vector<EncodableMap>& leaves) const {
    if (type == "concatenating") {
      for (const auto& child : children) child.Flatten(leaves);
    } else if (type == "looping") {
      for (size_t i = 0; i < count; ++i) children[0].Flatten(leaves);
    } else {
      leaves.push_back(data);
    }
  }
  void Order(std::vector<size_t>& order, size_t offset = 0) const {
    if (type == "concatenating") {
      std::vector<size_t> offsets;
      size_t next = offset;
      for (const auto& child : children) { offsets.push_back(next); next += child.Size(); }
      for (auto index : shuffle) children[index].Order(order, offsets[index]);
    } else if (type == "looping") {
      for (size_t i = 0; i < count; ++i) children[0].Order(order, offset + i * children[0].Size());
    } else {
      order.push_back(offset);
    }
  }
  SourceNode* FindById(const std::string& target) {
    if (id == target) return this;
    for (auto& child : children) if (auto match = child.FindById(target)) return match;
    return nullptr;
  }
  void UpdateShuffle(const SourceNode& other) {
    if (id != other.id || type != other.type || children.size() != other.children.size())
      throw ArgumentError{"Shuffle source does not match loaded tree"};
    shuffle = other.shuffle;
    for (size_t i = 0; i < children.size(); ++i) children[i].UpdateShuffle(other.children[i]);
  }
  void Mutate(const std::string& method, const EncodableMap& args) {
    if (method != "concatenatingInsertAll" && method != "concatenatingRemoveRange" && method != "concatenatingMove")
      throw ArgumentError{"Unknown playlist mutation"};
    auto* target = FindById(Require<std::string>(args, "id"));
    if (!target || target->type != "concatenating") throw ArgumentError{"Unknown concatenating source id"};
    auto& list = target->children;
    auto check = [&list](int64_t index, bool allowEnd) {
      if (index < 0 || static_cast<uint64_t>(index) > list.size() ||
          (!allowEnd && static_cast<uint64_t>(index) == list.size()))
        throw ArgumentError{"Playlist index is out of bounds"};
      return static_cast<size_t>(index);
    };
    if (method == "concatenatingInsertAll") {
      auto index = check(Integer(args, "index"), true);
      std::vector<SourceNode> added;
      for (const auto& child : Require<EncodableList>(args, "children")) {
        auto map = std::get_if<EncodableMap>(&child);
        if (!map) throw ArgumentError{"Source child must be a map"};
        added.push_back(Parse(*map));
      }
      list.insert(list.begin() + index, added.begin(), added.end());
    } else if (method == "concatenatingRemoveRange") {
      auto start = check(Integer(args, "startIndex"), true);
      auto end = check(Integer(args, "endIndex"), true);
      if (end < start) throw ArgumentError{"Invalid playlist range"};
      list.erase(list.begin() + start, list.begin() + end);
    } else {
      auto from = check(Integer(args, "currentIndex"), false);
      auto to = check(Integer(args, "newIndex"), false);
      auto moved = std::move(list[from]);
      list.erase(list.begin() + from);
      list.insert(list.begin() + to, std::move(moved));
    }
    target->shuffle = Permutation(Require<EncodableList>(args, "shuffleOrder"), list.size());
    Size();
    ValidateDepth();
  }
  void ValidateDepth(size_t depth = 0) const {
    if (depth > 64) throw ArgumentError{"Audio source nesting is too deep"};
    if (type == "clipping" && depth == 64) throw ArgumentError{"Audio source nesting is too deep"};
    for (const auto& child : children) child.ValidateDepth(depth + 1);
  }
};
}  // namespace jaw
