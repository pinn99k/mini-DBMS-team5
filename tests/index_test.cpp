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

// ---- 삭제·값 바꾸기·범위 (architecture §9.6, decisions #21 #22) -------------------------------

TYPED_TEST(BPlusTreeTest, RandomInsertEraseMatchesMapWithValidate) {
  TREE t;
  std::map<int64_t, uint64_t> ref;
  std::mt19937_64 rng(11);
  for (int i = 0; i < 40000; ++i) {
    const auto key = static_cast<int64_t>(rng() % 3000);
    const auto op = rng() % 10;
    if (op < 5) {
      EXPECT_EQ(t.insert(key, static_cast<uint64_t>(i)), ref.emplace(key, static_cast<uint64_t>(i)).second);
    } else if (op < 9) {
      EXPECT_EQ(t.erase(key), ref.erase(key) == 1) << "key " << key;
    } else {
      auto it = ref.find(key);
      EXPECT_EQ(t.assign(key, 7), it != ref.end());
      if (it != ref.end()) it->second = 7;
    }
    if (i % 499 == 0) {
      std::string why;
      ASSERT_TRUE(t.validate(&why)) << why << " at op " << i;
    }
  }
  std::string why;
  ASSERT_TRUE(t.validate(&why)) << why;
  const std::vector<std::pair<int64_t, uint64_t>> expected(ref.begin(), ref.end());
  EXPECT_EQ(t.items(), expected);
}

TYPED_TEST(BPlusTreeTest, EraseEverythingShrinksToEmpty) {
  for (bool reverse : {false, true}) {
    typename TREE::Loader loader;
    for (int i = 1; i <= 5000; ++i) loader.add(i, static_cast<uint64_t>(i));
    TREE t = loader.finish();
    int lastHeight = t.height();
    int shrinks = 0;
    for (int i = 1; i <= 5000; ++i) {
      const int64_t key = reverse ? 5001 - i : i;
      ASSERT_TRUE(t.erase(key)) << key;
      EXPECT_FALSE(t.erase(key));
      EXPECT_FALSE(t.find(key));
      if (t.height() < lastHeight) {
        ++shrinks;
        lastHeight = t.height();
        std::string why;
        ASSERT_TRUE(t.validate(&why)) << why << " at key " << key;
      }
    }
    EXPECT_GE(shrinks, 2);  // 병합이 루트까지 올라가 높이가 줄어듦
    EXPECT_EQ(t.size(), 0u);
    EXPECT_EQ(t.height(), 0);
    EXPECT_TRUE(t.validate());
    EXPECT_TRUE(t.insert(42, 1));  // 빈 트리에 다시 넣을 수 있다
    EXPECT_EQ(t.find(42), 1u);
  }
}

TEST(BPlusTreeErase, GuideExampleCapacityThree) {
  // guide.md 8.5와 확인 질문 8-1: 1..10에서 4를 지우고 7을 지운다
  BPlusTree<3> t;
  for (int64_t k = 1; k <= 10; ++k) t.insert(k, static_cast<uint64_t>(k));
  ASSERT_EQ(t.height(), 3);
  ASSERT_TRUE(t.erase(4));
  std::string why;
  ASSERT_TRUE(t.validate(&why)) << why;
  EXPECT_EQ(t.height(), 3);
  ASSERT_TRUE(t.erase(7));
  ASSERT_TRUE(t.validate(&why)) << why;  // 루트에 남은 경계값 7이 8로 고쳐져야 통과
  EXPECT_EQ(t.height(), 2);
  EXPECT_EQ(t.size(), 8u);
}

TYPED_TEST(BPlusTreeTest, RangeReturnsKeysInOrderWithLimit) {
  TREE t;
  using Items = std::vector<std::pair<int64_t, uint64_t>>;
  Items out;
  EXPECT_EQ(t.range(0, 100, 10, out), 0u);
  for (int64_t k = 2; k <= 2000; k += 2) t.insert(k, static_cast<uint64_t>(k));
  EXPECT_EQ(t.range(5, 11, 100, out), 3u);  // 6, 8, 10
  EXPECT_EQ(out, (Items{{6, 6}, {8, 8}, {10, 10}}));
  out.clear();
  // 이어 받기: 마지막 키 + 1부터 다시 부른다 (Table::Cursor가 쓰는 방식)
  int64_t from = 1;
  size_t total = 0;
  for (;;) {
    const size_t before = out.size();
    if (t.range(from, 1500, 7, out) == 0) break;
    total += out.size() - before;
    from = out.back().first + 1;
  }
  EXPECT_EQ(total, 750u);
  for (size_t i = 0; i < out.size(); ++i) ASSERT_EQ(out[i].first, static_cast<int64_t>(2 * (i + 1)));
  out.clear();
  EXPECT_EQ(t.range(2001, 5000, 10, out), 0u);
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
