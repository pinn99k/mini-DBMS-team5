#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <numeric>
#include <random>

#include "index/bplus_tree.h"

using namespace minidb;

template <typename T>
class BPlusTreeTest : public ::testing::Test {};

template <int N>
struct Cap {
  static constexpr int value = N;
};
using Capacities = ::testing::Types<Cap<3>, Cap<4>, Cap<128>>;
TYPED_TEST_SUITE(BPlusTreeTest, Capacities);

#define TREE BPlusTree<TypeParam::value>

TYPED_TEST(BPlusTreeTest, EmptyTree) {
  TREE t;
  EXPECT_TRUE(t.validate());
  EXPECT_FALSE(t.find(1));
  EXPECT_EQ(t.size(), 0u);
}

TYPED_TEST(BPlusTreeTest, RandomInsertFindWithValidateEveryNOps) {
  TREE t;
  std::map<int64_t, uint64_t> ref;
  std::mt19937_64 rng(7);
  for (int i = 0; i < 20000; ++i) {
    const auto key = static_cast<int64_t>(rng() % 50000) - 25000;
    const bool fresh = ref.emplace(key, static_cast<uint64_t>(i)).second;
    EXPECT_EQ(t.insert(key, static_cast<uint64_t>(i)), fresh);  // 중복 키는 false
    if (i % 997 == 0) {
      std::string why;
      ASSERT_TRUE(t.validate(&why)) << why << " at op " << i;
    }
  }
  std::string why;
  ASSERT_TRUE(t.validate(&why)) << why;
  EXPECT_EQ(t.size(), ref.size());
  for (const auto& [k, v] : ref) ASSERT_EQ(t.find(k), v);
  for (int i = 0; i < 1000; ++i) {
    const auto key = static_cast<int64_t>(rng() % 100000) + 30000;  // 없는 키
    EXPECT_FALSE(t.find(key));
  }
}

TYPED_TEST(BPlusTreeTest, SequentialInsertCascadesSplitsToNewRoot) {
  TREE t;
  int lastHeight = 0;
  int rootSplits = 0;
  for (int64_t k = 1; k <= 5000; ++k) {
    t.insert(k, static_cast<uint64_t>(k * 10));
    if (t.height() > lastHeight) {
      ++rootSplits;
      lastHeight = t.height();
      std::string why;
      ASSERT_TRUE(t.validate(&why)) << why << " at key " << k;
    }
  }
  EXPECT_GE(rootSplits, TypeParam::value == 128 ? 2 : 5);  // 연쇄 분할로 루트가 여러 번 바뀜
  ASSERT_TRUE(t.validate());
  auto items = t.items();
  ASSERT_EQ(items.size(), 5000u);
  for (size_t i = 0; i < items.size(); ++i) EXPECT_EQ(items[i].first, static_cast<int64_t>(i + 1));
}

TYPED_TEST(BPlusTreeTest, BulkLoadMatchesInsertBuiltTree) {
  for (int n : {0, 1, 2, 3, 4, 5, 7, 9, 17, 100, 129, 130, 257, 1000, 16513, 40000}) {
    typename TREE::Loader loader;
    TREE inserted;
    for (int i = 1; i <= n; ++i) {
      loader.add(i * 3, static_cast<uint64_t>(i));
      inserted.insert(i * 3, static_cast<uint64_t>(i));
    }
    TREE bulk = loader.finish();
    std::string why;
    ASSERT_TRUE(bulk.validate(&why)) << "n=" << n << ": " << why;
    EXPECT_EQ(bulk.size(), static_cast<size_t>(n));
    EXPECT_EQ(bulk.items(), inserted.items()) << "n=" << n;
    for (int k = 0; k <= 3 * n + 2; ++k) ASSERT_EQ(bulk.find(k), inserted.find(k)) << "n=" << n << " k=" << k;
    EXPECT_LE(bulk.height(), inserted.height());
  }
}

TYPED_TEST(BPlusTreeTest, BulkLoadedTreeAcceptsFurtherInserts) {
  typename TREE::Loader loader;
  for (int i = 1; i <= 1000; ++i) loader.add(i, static_cast<uint64_t>(i));
  TREE t = loader.finish();
  for (int i = 1001; i <= 3000; ++i) ASSERT_TRUE(t.insert(i, static_cast<uint64_t>(i)));
  EXPECT_FALSE(t.insert(500, 0));
  std::string why;
  ASSERT_TRUE(t.validate(&why)) << why;
  EXPECT_EQ(t.find(2500), 2500u);
}

TEST(BPlusTreeLoader, RejectsNonAscendingKeys) {
  BPlusTree<4>::Loader loader;
  loader.add(5, 0);
  EXPECT_THROW(loader.add(5, 1), std::invalid_argument);
  EXPECT_THROW(loader.add(3, 1), std::invalid_argument);
}

TEST(BPlusTreeMillion, LookupsAfterOneMillionInserts) {
  BPlusTree<128> t;
  for (int64_t k = 1; k <= 1000000; ++k) t.insert(k, static_cast<uint64_t>(k) * 2);
  ASSERT_TRUE(t.validate());
  std::mt19937_64 rng(1);
  for (int i = 0; i < 10000; ++i) {
    const auto k = static_cast<int64_t>(rng() % 1000000) + 1;
    ASSERT_EQ(t.find(k), static_cast<uint64_t>(k) * 2);
  }
  EXPECT_FALSE(t.find(0));
  EXPECT_FALSE(t.find(1000001));
}
