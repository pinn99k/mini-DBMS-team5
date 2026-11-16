#pragma once

// 메모리 B+ 트리: id(i64) → record offset(u64). 유일 키, 삭제·범위 질의 없음 (architecture §9).
// - 노드 안 고정 크기 배열, 키·값 배열 분리(SoA). 노드 안 std::vector 금지.
// - 리프/내부 노드는 별도 구조체 + 공통 헤더(isLeaf). 가상 함수 없음.
//   그래서 unique_ptr는 isLeaf를 보고 올바른 타입으로 delete하는 deleter를 쓴다.
// - 분할은 bottom-up: 내려간 경로를 스택에 기록, 부모 포인터 없음. 반반 분할.
//   리프 분할 = copy-up, 내부 분할 = push-up.
// - 배열은 Cap+1 크기: 분할 직전에 Cap+1개를 잠깐 담기 위해서다.
// 스레드 안전하지 않다. Table의 락이 보호한다.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace minidb {

template <int Cap>
class BPlusTree {
  static_assert(Cap >= 3, "B+ tree capacity must be at least 3");

 public:
  static constexpr int kMaxKeys = Cap;
  static constexpr int kMinLeafKeys = (Cap + 1) / 2;
  static constexpr int kMinInternalKeys = Cap / 2;

  class Loader;

  BPlusTree() = default;
  BPlusTree(BPlusTree&&) noexcept = default;
  BPlusTree& operator=(BPlusTree&&) noexcept = default;

  // 이미 있는 키면 false.
  bool insert(int64_t key, uint64_t value);
  std::optional<uint64_t> find(int64_t key) const;
  size_t size() const { return size_; }
  int height() const { return height_; }
  bool validate(std::string* why = nullptr) const;
  std::vector<std::pair<int64_t, uint64_t>> items() const;  // 리프 연결 순서

 private:
  struct Node {
    explicit Node(bool leaf) : isLeaf(leaf) {}
    bool isLeaf;
    int count = 0;
  };
  struct NodeDeleter {
    void operator()(Node* n) const noexcept;
  };
  using NodePtr = std::unique_ptr<Node, NodeDeleter>;

  struct Leaf : Node {
    Leaf() : Node(true) {}
    int64_t keys[Cap + 1];
    uint64_t values[Cap + 1];
    Leaf* next = nullptr;  // 소유하지 않음
  };
  struct Internal : Node {
    Internal() : Node(false) {}
    int64_t keys[Cap + 1];
    NodePtr children[Cap + 2];  // count + 1개 사용
  };

  static int childIndex(const Internal* in, int64_t key) {
    // 경계값 = 오른쪽 서브트리의 최소 키 → key == 경계값이면 오른쪽으로
    return static_cast<int>(std::upper_bound(in->keys, in->keys + in->count, key) - in->keys);
  }
  static int64_t minKey(const Node* n) {
    while (!n->isLeaf) n = static_cast<const Internal*>(n)->children[0].get();
    return static_cast<const Leaf*>(n)->keys[0];
  }
  const Leaf* leftmostLeaf() const {
    const Node* n = root_.get();
    while (n && !n->isLeaf) n = static_cast<const Internal*>(n)->children[0].get();
    return static_cast<const Leaf*>(n);
  }
  bool checkNode(const Node* n, int depth, bool isRoot, const int64_t* lo, const int64_t* hi,
                 int& leafDepth, std::string& why) const;

  NodePtr root_;
  size_t size_ = 0;
  int height_ = 0;  // 레벨 수. 빈 트리 0, 리프 하나 1
};

// ---------------------------------------------------------------------------------------------
// 벌크 로딩: 오름차순 (key, value)를 스트리밍으로 받아 리프를 꽉 채우고 위로 쌓는다.
// 레벨의 마지막 노드가 최소 개수보다 적으면 바로 앞 노드와 균등하게 나눈다.
template <int Cap>
class BPlusTree<Cap>::Loader {
 public:
  void add(int64_t key, uint64_t value) {
    if (count_ > 0 && key <= last_) throw std::invalid_argument("bulk load keys must be strictly ascending");
    if (!cur_ || cur_->count == Cap) {
      cur_ = new Leaf();
      leaves_.emplace_back(cur_);
    }
    cur_->keys[cur_->count] = key;
    cur_->values[cur_->count] = value;
    ++cur_->count;
    last_ = key;
    ++count_;
  }
  BPlusTree finish();

 private:
  std::vector<NodePtr> leaves_;
  Leaf* cur_ = nullptr;
  int64_t last_ = 0;
  size_t count_ = 0;
};

// ---------------------------------------------------------------------------------------------
template <int Cap>
void BPlusTree<Cap>::NodeDeleter::operator()(Node* n) const noexcept {
  if (n->isLeaf) {
    delete static_cast<Leaf*>(n);
  } else {
    delete static_cast<Internal*>(n);
  }
}

template <int Cap>
std::optional<uint64_t> BPlusTree<Cap>::find(int64_t key) const {
  const Node* n = root_.get();
  if (!n) return std::nullopt;
  while (!n->isLeaf) {
    const auto* in = static_cast<const Internal*>(n);
    n = in->children[childIndex(in, key)].get();
  }
  const auto* lf = static_cast<const Leaf*>(n);
  const int64_t* p = std::lower_bound(lf->keys, lf->keys + lf->count, key);
  if (p != lf->keys + lf->count && *p == key) return lf->values[p - lf->keys];
  return std::nullopt;
}

template <int Cap>
bool BPlusTree<Cap>::insert(int64_t key, uint64_t value) {
  if (!root_) {
    auto* lf = new Leaf();
    root_.reset(lf);
    lf->keys[0] = key;
    lf->values[0] = value;
    lf->count = 1;
    size_ = 1;
    height_ = 1;
    return true;
  }

  struct Step {
    Internal* node;
    int idx;
  };
  Step path[64];
  int depth = 0;
  Node* n = root_.get();
  while (!n->isLeaf) {
    auto* in = static_cast<Internal*>(n);
    int i = childIndex(in, key);
    path[depth++] = Step{in, i};
    n = in->children[i].get();
  }

  auto* lf = static_cast<Leaf*>(n);
  int pos = static_cast<int>(std::lower_bound(lf->keys, lf->keys + lf->count, key) - lf->keys);
  if (pos < lf->count && lf->keys[pos] == key) return false;
  std::move_backward(lf->keys + pos, lf->keys + lf->count, lf->keys + lf->count + 1);
  std::move_backward(lf->values + pos, lf->values + lf->count, lf->values + lf->count + 1);
  lf->keys[pos] = key;
  lf->values[pos] = value;
  ++lf->count;
  ++size_;
  if (lf->count <= Cap) return true;

  // 리프 분할 (copy-up): Cap+1개 → 왼쪽 ⌊(Cap+1)/2⌋, 나머지 오른쪽. 오른쪽 첫 키를 부모로 복사.
  auto* right = new Leaf();
  NodePtr newChild(right);
  const int left = (Cap + 1) / 2;
  std::copy(lf->keys + left, lf->keys + lf->count, right->keys);
  std::copy(lf->values + left, lf->values + lf->count, right->values);
  right->count = lf->count - left;
  lf->count = left;
  right->next = lf->next;
  lf->next = right;
  int64_t sep = right->keys[0];

  while (depth > 0) {
    Step s = path[--depth];
    Internal* in = s.node;
    const int i = s.idx;
    std::move_backward(in->keys + i, in->keys + in->count, in->keys + in->count + 1);
    for (int c = in->count + 1; c > i + 1; --c) in->children[c] = std::move(in->children[c - 1]);
    in->keys[i] = sep;
    in->children[i + 1] = std::move(newChild);
    ++in->count;
    if (in->count <= Cap) return true;

    // 내부 분할 (push-up): Cap+1개 → 왼쪽 ⌊Cap/2⌋, 가운데 1개는 위로, 나머지 오른쪽.
    auto* r = new Internal();
    NodePtr rp(r);
    const int leftKeys = Cap / 2;
    sep = in->keys[leftKeys];
    const int rightKeys = in->count - leftKeys - 1;
    std::copy(in->keys + leftKeys + 1, in->keys + in->count, r->keys);
    for (int c = 0; c <= rightKeys; ++c) r->children[c] = std::move(in->children[leftKeys + 1 + c]);
    r->count = rightKeys;
    in->count = leftKeys;
    newChild = std::move(rp);
  }

  // 루트까지 분할 → 새 루트
  auto* nr = new Internal();
  NodePtr nrp(nr);
  nr->keys[0] = sep;
  nr->children[0] = std::move(root_);
  nr->children[1] = std::move(newChild);
  nr->count = 1;
  root_ = std::move(nrp);
  ++height_;
  return true;
}

template <int Cap>
bool BPlusTree<Cap>::checkNode(const Node* n, int depth, bool isRoot, const int64_t* lo,
                               const int64_t* hi, int& leafDepth, std::string& why) const {
  const int minKeys = n->isLeaf ? kMinLeafKeys : kMinInternalKeys;
  if (n->count > Cap) return why = "node has more than Cap keys", false;
  if (isRoot ? n->count < 1 : n->count < minKeys) return why = "node has too few keys", false;

  const int64_t* keys = n->isLeaf ? static_cast<const Leaf*>(n)->keys : static_cast<const Internal*>(n)->keys;
  for (int i = 0; i < n->count; ++i) {
    if (i > 0 && keys[i - 1] >= keys[i]) return why = "keys not strictly ascending in node", false;
    if ((lo && keys[i] < *lo) || (hi && keys[i] >= *hi)) return why = "key outside parent range", false;
  }

  if (n->isLeaf) {
    if (leafDepth < 0) leafDepth = depth;
    if (leafDepth != depth) return why = "leaves at different depths", false;
    return true;
  }

  const auto* in = static_cast<const Internal*>(n);
  for (int i = 0; i <= in->count; ++i) {
    const Node* child = in->children[i].get();
    if (!child) return why = "missing child pointer", false;
    if (i > 0 && in->keys[i - 1] != minKey(child)) return why = "separator != min key of right subtree", false;
    const int64_t* clo = i == 0 ? lo : &in->keys[i - 1];
    const int64_t* chi = i == in->count ? hi : &in->keys[i];
    if (!checkNode(child, depth + 1, false, clo, chi, leafDepth, why)) return false;
  }
  return true;
}

template <int Cap>
bool BPlusTree<Cap>::validate(std::string* whyOut) const {
  std::string why;
  auto fail = [&](const std::string& m) {
    if (whyOut) *whyOut = m;
    return false;
  };
  if (!root_) return size_ == 0 && height_ == 0 ? true : fail("empty tree with nonzero size");

  int leafDepth = -1;
  if (!checkNode(root_.get(), 1, true, nullptr, nullptr, leafDepth, why)) return fail(why);
  if (leafDepth != height_) return fail("height does not match leaf depth");

  size_t total = 0;
  bool first = true;
  int64_t prev = 0;
  for (const Leaf* lf = leftmostLeaf(); lf; lf = lf->next) {
    for (int i = 0; i < lf->count; ++i) {
      if (!first && lf->keys[i] <= prev) return fail("leaf chain not ascending");
      prev = lf->keys[i];
      first = false;
      ++total;
    }
  }
  if (total != size_) return fail("leaf chain count != size");
  return true;
}

template <int Cap>
std::vector<std::pair<int64_t, uint64_t>> BPlusTree<Cap>::items() const {
  std::vector<std::pair<int64_t, uint64_t>> out;
  out.reserve(size_);
  for (const Leaf* lf = leftmostLeaf(); lf; lf = lf->next) {
    for (int i = 0; i < lf->count; ++i) out.emplace_back(lf->keys[i], lf->values[i]);
  }
  return out;
}

template <int Cap>
BPlusTree<Cap> BPlusTree<Cap>::Loader::finish() {
  BPlusTree tree;
  if (leaves_.empty()) return tree;
  tree.size_ = count_;

  // 마지막 리프가 최소 개수 미만이면 앞 리프와 균등하게 나눈다.
  // (앞 리프는 꽉 차 있으므로 합 ≥ Cap+1 → 양쪽 모두 ⌊(Cap+1)/2⌋ 이상)
  if (leaves_.size() > 1) {
    auto* last = static_cast<Leaf*>(leaves_.back().get());
    auto* prev = static_cast<Leaf*>(leaves_[leaves_.size() - 2].get());
    if (last->count < kMinLeafKeys) {
      const int total = prev->count + last->count;
      const int keepPrev = (total + 1) / 2;
      const int moved = prev->count - keepPrev;
      std::move_backward(last->keys, last->keys + last->count, last->keys + last->count + moved);
      std::move_backward(last->values, last->values + last->count, last->values + last->count + moved);
      std::copy(prev->keys + keepPrev, prev->keys + prev->count, last->keys);
      std::copy(prev->values + keepPrev, prev->values + prev->count, last->values);
      last->count += moved;
      prev->count = keepPrev;
    }
  }
  for (size_t i = 0; i + 1 < leaves_.size(); ++i) {
    static_cast<Leaf*>(leaves_[i].get())->next = static_cast<Leaf*>(leaves_[i + 1].get());
  }

  std::vector<NodePtr> level = std::move(leaves_);
  std::vector<int64_t> mins;
  mins.reserve(level.size());
  for (const auto& p : level) mins.push_back(static_cast<const Leaf*>(p.get())->keys[0]);

  int height = 1;
  while (level.size() > 1) {
    const size_t n = level.size();
    const size_t maxChildren = Cap + 1;
    const size_t minChildren = Cap / 2 + 1;
    const size_t groups = (n + maxChildren - 1) / maxChildren;
    std::vector<size_t> sizes(groups, maxChildren);
    sizes.back() = n - (groups - 1) * maxChildren;
    if (groups > 1 && sizes.back() < minChildren) {
      const size_t t = sizes[groups - 2] + sizes.back();
      sizes[groups - 2] = (t + 1) / 2;
      sizes.back() = t / 2;
    }

    std::vector<NodePtr> next;
    std::vector<int64_t> nextMins;
    size_t idx = 0;
    for (size_t g : sizes) {
      auto* in = new Internal();
      NodePtr p(in);
      nextMins.push_back(mins[idx]);
      for (size_t c = 0; c < g; ++c, ++idx) {
        if (c > 0) in->keys[c - 1] = mins[idx];
        in->children[c] = std::move(level[idx]);
      }
      in->count = static_cast<int>(g) - 1;
      next.push_back(std::move(p));
    }
    level = std::move(next);
    mins = std::move(nextMins);
    ++height;
  }

  tree.root_ = std::move(level[0]);
  tree.height_ = height;
  leaves_.clear();
  cur_ = nullptr;
  count_ = 0;
  return tree;
}

}  // namespace minidb
