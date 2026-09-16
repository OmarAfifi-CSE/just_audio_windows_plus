#include "../source_model.hpp"

#include <gtest/gtest.h>
#include <limits>

namespace jaw {
namespace {
EncodableMap Uri(const std::string& id, const std::string& type = "progressive") {
  return {{EncodableValue("id"), EncodableValue(id)},
          {EncodableValue("type"), EncodableValue(type)},
          {EncodableValue("uri"), EncodableValue("https://example.com/" + id)}};
}
EncodableList Indices(std::initializer_list<int64_t> indices) {
  EncodableList result;
  for (auto index : indices) result.emplace_back(index);
  return result;
}
EncodableMap Concat(const std::string& id, const EncodableList& children,
                    const EncodableList& order) {
  return {{EncodableValue("id"), EncodableValue(id)},
          {EncodableValue("type"), EncodableValue("concatenating")},
          {EncodableValue("children"), EncodableValue(children)},
          {EncodableValue("shuffleOrder"), EncodableValue(order)}};
}
EncodableMap Loop(const EncodableMap& child, int64_t count) {
  return {{EncodableValue("id"), EncodableValue("loop")},
          {EncodableValue("type"), EncodableValue("looping")},
          {EncodableValue("child"), EncodableValue(child)},
          {EncodableValue("count"), EncodableValue(count)}};
}
EncodableMap Clip(const EncodableMap& child, int64_t start, int64_t end) {
  return {{EncodableValue("id"), EncodableValue("clip")},
          {EncodableValue("type"), EncodableValue("clipping")},
          {EncodableValue("child"), EncodableValue(child)},
          {EncodableValue("start"), EncodableValue(start)},
          {EncodableValue("end"), EncodableValue(end)}};
}
std::vector<std::string> LeafIds(const SourceNode& node) {
  std::vector<EncodableMap> leaves;
  node.Flatten(leaves);
  std::vector<std::string> ids;
  for (const auto& leaf : leaves) ids.push_back(Require<std::string>(leaf, "id"));
  return ids;
}
std::vector<size_t> Order(const SourceNode& node) {
  std::vector<size_t> order;
  node.Order(order);
  return order;
}
EncodableMap MoveArgs(int64_t from, int64_t to) {
  return {{EncodableValue("id"), EncodableValue("inner")},
          {EncodableValue("currentIndex"), EncodableValue(from)},
          {EncodableValue("newIndex"), EncodableValue(to)},
          {EncodableValue("shuffleOrder"), EncodableValue(Indices({1, 0}))}};
}

TEST(SourceModel, RecursiveFlattenAndShuffleUsePhysicalLeafOffsets) {
  auto inner = Concat("inner", {EncodableValue(Uri("a")), EncodableValue(Uri("b", "hls"))}, Indices({1, 0}));
  auto clip = Clip(Uri("c", "dash"), 1000000, 3000000);
  auto node = SourceNode::Parse(Concat("root", {EncodableValue(Loop(inner, 2)), EncodableValue(clip)}, Indices({1, 0})));
  EXPECT_EQ(node.Size(), 5u);
  EXPECT_EQ(LeafIds(node), (std::vector<std::string>{"a", "b", "a", "b", "clip"}));
  EXPECT_EQ(Order(node), (std::vector<size_t>{4, 1, 0, 3, 2}));
  std::vector<EncodableMap> leaves;
  node.Flatten(leaves);
  EXPECT_EQ(leaves.back(), clip);
}

TEST(SourceModel, LoopCountsAndExpandedSizeAreBounded) {
  auto zero = SourceNode::Parse(Loop(Uri("a"), 0));
  EXPECT_EQ(zero.Size(), 0u);
  EXPECT_TRUE(LeafIds(zero).empty());
  EXPECT_TRUE(Order(zero).empty());
  EXPECT_EQ(SourceNode::Parse(Loop(Uri("a"), 100000)).Size(), 100000u);
  for (int64_t count : {-1LL, 100001LL, (std::numeric_limits<int64_t>::max)()})
    EXPECT_THROW(SourceNode::Parse(Loop(Uri("a"), count)), ArgumentError);
  EXPECT_THROW(SourceNode::Parse(Loop(Loop(Uri("a"), 1000), 101)).Size(), ArgumentError);
  EXPECT_THROW(SourceNode::Parse(Concat("root", {EncodableValue(Loop(Uri("a"), 100000)), EncodableValue(Uri("b"))}, Indices({0, 1}))).Size(), ArgumentError);
}

TEST(SourceModel, DepthBoundaryRejectsSixtyFifthWrapper) {
  auto source = Uri("a");
  for (int i = 0; i < 64; ++i) source = Loop(source, 1);
  EXPECT_NO_THROW(SourceNode::Parse(source));
  EXPECT_THROW(SourceNode::Parse(Loop(source, 1)), ArgumentError);
}

TEST(SourceModel, InvalidClippingRangesAndNonUriChildrenAreRejected) {
  EXPECT_NO_THROW(SourceNode::Parse(Clip(Uri("a"), 0, -1)));
  EXPECT_NO_THROW(SourceNode::Parse(Clip(Uri("a"), 3000000000LL, 4000000000LL)));
  EXPECT_THROW(SourceNode::Parse(Clip(Uri("a"), -1, 5)), ArgumentError);
  EXPECT_THROW(SourceNode::Parse(Clip(Uri("a"), 5, 5)), ArgumentError);
  EXPECT_THROW(SourceNode::Parse(Clip(Uri("a"), 5, 4)), ArgumentError);
  EXPECT_THROW(SourceNode::Parse(Clip(Loop(Uri("a"), 2), 0, 5)), ArgumentError);
}

TEST(SourceModel, ShuffleMustBeAnExactIntegerPermutation) {
  const EncodableList children{EncodableValue(Uri("a")), EncodableValue(Uri("b"))};
  for (const auto& order : {Indices({0}), Indices({0, 0}), Indices({-1, 0}), Indices({0, 2}), Indices({0, (std::numeric_limits<int64_t>::max)()})})
    EXPECT_THROW(SourceNode::Parse(Concat("root", children, order)), ArgumentError);
  EXPECT_THROW(SourceNode::Parse(Concat("root", children, {EncodableValue(0.0), EncodableValue(int32_t{1})})), ArgumentError);
  EXPECT_TRUE(Order(SourceNode::Parse(Concat("root", {}, {}))).empty());
}

TEST(SourceModel, NestedInsertMoveRemoveRecomputeGlobalShuffleOffsets) {
  auto inner = Concat("inner", {EncodableValue(Uri("a")), EncodableValue(Uri("b"))}, Indices({1, 0}));
  auto node = SourceNode::Parse(Concat("root", {EncodableValue(Uri("before")), EncodableValue(Loop(inner, 2)), EncodableValue(Uri("after"))}, Indices({2, 1, 0})));
  node.Mutate("concatenatingInsertAll", {{EncodableValue("id"), EncodableValue("inner")}, {EncodableValue("index"), EncodableValue(int64_t{1})}, {EncodableValue("children"), EncodableValue(EncodableList{EncodableValue(Uri("x"))})}, {EncodableValue("shuffleOrder"), EncodableValue(Indices({2, 0, 1}))}});
  EXPECT_EQ(LeafIds(node), (std::vector<std::string>{"before", "a", "x", "b", "a", "x", "b", "after"}));
  EXPECT_EQ(Order(node), (std::vector<size_t>{7, 3, 1, 2, 6, 4, 5, 0}));
  node.Mutate("concatenatingRemoveRange", {{EncodableValue("id"), EncodableValue("inner")}, {EncodableValue("startIndex"), EncodableValue(int64_t{1})}, {EncodableValue("endIndex"), EncodableValue(int64_t{2})}, {EncodableValue("shuffleOrder"), EncodableValue(Indices({1, 0}))}});
  node.Mutate("concatenatingMove", MoveArgs(0, 1));
  EXPECT_EQ(LeafIds(node), (std::vector<std::string>{"before", "b", "a", "b", "a", "after"}));
  EXPECT_EQ(Order(node), (std::vector<size_t>{5, 2, 1, 4, 3, 0}));
}

TEST(SourceModel, InvalidMutationArgumentsCannotBeNarrowedIntoValidIndices) {
  auto original = SourceNode::Parse(Concat("inner", {EncodableValue(Uri("a")), EncodableValue(Uri("b"))}, Indices({0, 1})));
  for (auto index : {-1LL, 2LL, 4294967296LL, (std::numeric_limits<int64_t>::max)()}) {
    auto candidate = original;
    EXPECT_THROW(candidate.Mutate("concatenatingMove", MoveArgs(index, 0)), ArgumentError);
    candidate = original;
    EXPECT_THROW(candidate.Mutate("concatenatingMove", MoveArgs(0, index)), ArgumentError);
  }
  for (const char* field : {"id", "currentIndex", "newIndex", "shuffleOrder"}) {
    auto args = MoveArgs(0, 1);
    args.erase(EncodableValue(field));
    auto candidate = original;
    EXPECT_THROW(candidate.Mutate("concatenatingMove", args), ArgumentError);
  }
  auto args = MoveArgs(0, 1);
  args[EncodableValue("id")] = EncodableValue("missing");
  EXPECT_THROW(original.Mutate("concatenatingMove", args), ArgumentError);
}

TEST(SourceModel, UpdateShufflePreservesLeafOrderAndChecksTreeIdentity) {
  auto source = Concat("root", {EncodableValue(Uri("a")), EncodableValue(Uri("b"))}, Indices({0, 1}));
  auto node = SourceNode::Parse(source);
  source[EncodableValue("shuffleOrder")] = EncodableValue(Indices({1, 0}));
  node.UpdateShuffle(SourceNode::Parse(source));
  EXPECT_EQ(LeafIds(node), (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(Order(node), (std::vector<size_t>{1, 0}));
  source[EncodableValue("id")] = EncodableValue("different");
  EXPECT_THROW(node.UpdateShuffle(SourceNode::Parse(source)), ArgumentError);
}

TEST(SourceModel, MutationChecksDepthInContextOfEntireLoadedTree) {
  auto source = Concat("inner", {}, {});
  for (int i = 0; i < 63; ++i) source = Loop(source, 1);
  auto original = SourceNode::Parse(source);
  EncodableMap args{{EncodableValue("id"), EncodableValue("inner")},
                    {EncodableValue("index"), EncodableValue(int32_t{0})},
                    {EncodableValue("children"), EncodableValue(EncodableList{EncodableValue(Uri("a"))})},
                    {EncodableValue("shuffleOrder"), EncodableValue(Indices({0}))}};
  auto candidate = original;
  EXPECT_NO_THROW(candidate.Mutate("concatenatingInsertAll", args));
  EXPECT_EQ(candidate.Size(), 1u);
  for (const auto& child : {Loop(Uri("a"), 1), Clip(Uri("a"), 0, 10)}) {
    candidate = original;
    args[EncodableValue("children")] = EncodableValue(EncodableList{EncodableValue(child)});
    EXPECT_THROW(candidate.Mutate("concatenatingInsertAll", args), ArgumentError);
  }
  EXPECT_EQ(original.Size(), 0u);
}

TEST(SourceModel, UnknownMutationMethodIsRejected) {
  auto node = SourceNode::Parse(Concat("inner", {EncodableValue(Uri("a")), EncodableValue(Uri("b"))}, Indices({0, 1})));
  EXPECT_THROW(node.Mutate("concatenatingTypo", MoveArgs(0, 1)), ArgumentError);
}

TEST(SourceModel, InsertAndRemoveValidateArgumentsAndAllowEmptyEndRange) {
  auto original = SourceNode::Parse(Concat("inner", {EncodableValue(Uri("a")), EncodableValue(Uri("b"))}, Indices({0, 1})));
  EncodableMap insert{{EncodableValue("id"), EncodableValue("inner")},
                      {EncodableValue("index"), EncodableValue(int64_t{2})},
                      {EncodableValue("children"), EncodableValue(EncodableList{})},
                      {EncodableValue("shuffleOrder"), EncodableValue(Indices({0, 1}))}};
  EXPECT_NO_THROW(original.Mutate("concatenatingInsertAll", insert));
  for (const char* field : {"index", "children", "shuffleOrder"}) {
    auto args = insert;
    args.erase(EncodableValue(field));
    auto candidate = original;
    EXPECT_THROW(candidate.Mutate("concatenatingInsertAll", args), ArgumentError);
  }
  auto invalid = insert;
  invalid[EncodableValue("children")] = EncodableValue(EncodableList{EncodableValue("not a map")});
  EXPECT_THROW(original.Mutate("concatenatingInsertAll", invalid), ArgumentError);
  EncodableMap remove{{EncodableValue("id"), EncodableValue("inner")},
                      {EncodableValue("startIndex"), EncodableValue(int64_t{2})},
                      {EncodableValue("endIndex"), EncodableValue(int64_t{2})},
                      {EncodableValue("shuffleOrder"), EncodableValue(Indices({0, 1}))}};
  EXPECT_NO_THROW(original.Mutate("concatenatingRemoveRange", remove));
  EXPECT_EQ(LeafIds(original), (std::vector<std::string>{"a", "b"}));
  for (const auto& bounds : std::vector<std::pair<int64_t, int64_t>>{{1, 0}, {-1, 1}, {0, 3}, {0, 4294967296LL}}) {
    auto args = remove;
    args[EncodableValue("startIndex")] = EncodableValue(bounds.first);
    args[EncodableValue("endIndex")] = EncodableValue(bounds.second);
    auto candidate = original;
    EXPECT_THROW(candidate.Mutate("concatenatingRemoveRange", args), ArgumentError);
  }
  remove[EncodableValue("startIndex")] = EncodableValue(int32_t{0});
  remove[EncodableValue("shuffleOrder")] = EncodableValue(EncodableList{});
  original.Mutate("concatenatingRemoveRange", remove);
  EXPECT_EQ(original.Size(), 0u);
  EXPECT_TRUE(Order(original).empty());
}

TEST(SourceModel, MissingWrongTypedAndUnsupportedSourcesFailWithOwnedMessages) {
  EXPECT_THROW(SourceNode::Parse({}), ArgumentError);
  auto source = Uri("a");
  source[EncodableValue("uri")] = EncodableValue(int32_t{4});
  EXPECT_THROW(SourceNode::Parse(source), ArgumentError);
  source[EncodableValue("uri")] = EncodableValue("");
  EXPECT_THROW(SourceNode::Parse(source), ArgumentError);
  EXPECT_THROW(Integer(EncodableValue(true)), ArgumentError);
  EXPECT_THROW(Integer(EncodableValue(1.0)), ArgumentError);
  EXPECT_EQ(Integer(EncodableValue((std::numeric_limits<int64_t>::max)())), (std::numeric_limits<int64_t>::max)());
  ArgumentError saved{""};
  const std::string unsupported(1024, 'z');
  try {
    SourceNode::Parse(Uri("a", unsupported));
    FAIL() << "Expected unsupported source to fail";
  } catch (const ArgumentError& error) {
    saved = error;
  }
  const std::vector<std::string> churn(200, std::string(2048, 'x'));
  EXPECT_EQ(saved.message, "Unsupported audio source type: " + unsupported);
}
}  // namespace
}  // namespace jaw
