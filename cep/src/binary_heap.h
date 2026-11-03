#pragma once
// Student-written array-based binary heap (priority queue).
// Better(a, b) == true means a must sit above b, so the root is the "best" element.
//   build  : Floyd bottom-up heapify, O(n)
//   push   : sift-up, O(log n)
//   pop    : move last to root + sift-down, O(log n)
// Every element comparison is counted so the benchmark can report operation counts.
#include <cstddef>
#include <utility>
#include <vector>

template <class T, class Better>
class BinaryHeap {
 public:
  explicit BinaryHeap(Better better) : better_(better) {}

  void build(std::vector<T> items) {
    a_ = std::move(items);
    for (std::size_t i = a_.size() / 2; i-- > 0;) siftDown(i);
  }

  void push(const T& x) {
    a_.push_back(x);
    siftUp(a_.size() - 1);
  }

  T pop() {
    T top = a_[0];
    a_[0] = a_.back();
    a_.pop_back();
    if (!a_.empty()) siftDown(0);
    return top;
  }

  const T& top() const { return a_[0]; }
  bool empty() const { return a_.empty(); }
  std::size_t size() const { return a_.size(); }
  const std::vector<T>& items() const { return a_; }
  long long comparisons() const { return cmps_; }
  std::size_t bytes() const { return a_.capacity() * sizeof(T); }

  // Heap-order invariant: no child is better than its parent.
  bool isHeap() const {
    for (std::size_t i = 1; i < a_.size(); ++i)
      if (better_(a_[i], a_[(i - 1) / 2])) return false;
    return true;
  }

 private:
  bool better(const T& x, const T& y) {
    ++cmps_;
    return better_(x, y);
  }

  void siftUp(std::size_t i) {
    T x = a_[i];
    while (i > 0) {
      std::size_t p = (i - 1) / 2;
      if (!better(x, a_[p])) break;
      a_[i] = a_[p];
      i = p;
    }
    a_[i] = x;
  }

  void siftDown(std::size_t i) {
    const std::size_t n = a_.size();
    T x = a_[i];
    for (;;) {
      std::size_t c = 2 * i + 1;
      if (c >= n) break;
      if (c + 1 < n && better(a_[c + 1], a_[c])) ++c;
      if (!better(a_[c], x)) break;
      a_[i] = a_[c];
      i = c;
    }
    a_[i] = x;
  }

  Better better_;
  std::vector<T> a_;
  long long cmps_ = 0;
};
