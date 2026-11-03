#pragma once
// Student-written singly linked list with a tail pointer.
// Holds the context package while it is assembled: O(1) append in acceptance order,
// then an in-place linked-list merge sort (O(k log k), O(log k) recursion, no extra array)
// applies the chosen ordering rule.
#include <cstddef>

template <class T>
class LinkedList {
 public:
  LinkedList() = default;
  ~LinkedList() { clear(); }
  LinkedList(const LinkedList&) = delete;
  LinkedList& operator=(const LinkedList&) = delete;

  void pushBack(const T& v) {
    Node* n = new Node{v, nullptr};
    if (!head_) head_ = tail_ = n;
    else {
      tail_->next = n;
      tail_ = n;
    }
    ++size_;
  }

  template <class Less>
  void sort(Less less) {
    head_ = sortList(head_, less);
    tail_ = head_;
    while (tail_ && tail_->next) tail_ = tail_->next;
  }

  template <class F>
  void forEach(F f) const {
    for (Node* n = head_; n; n = n->next) f(n->value);
  }

  void clear() {
    while (head_) {
      Node* n = head_->next;
      delete head_;
      head_ = n;
    }
    tail_ = nullptr;
    size_ = 0;
  }

  std::size_t size() const { return size_; }
  std::size_t bytes() const;

 private:
  struct Node {
    T value;
    Node* next;
  };

  template <class Less>
  static Node* sortList(Node* h, Less& less) {
    if (!h || !h->next) return h;
    Node* slow = h;  // split in the middle with slow/fast pointers
    Node* fast = h->next;
    while (fast && fast->next) {
      slow = slow->next;
      fast = fast->next->next;
    }
    Node* right = slow->next;
    slow->next = nullptr;
    Node* a = sortList(h, less);
    Node* b = sortList(right, less);
    Node* out = nullptr;
    Node** tailp = &out;
    while (a && b) {
      if (less(b->value, a->value)) {  // stable: left wins ties
        *tailp = b;
        b = b->next;
      } else {
        *tailp = a;
        a = a->next;
      }
      tailp = &(*tailp)->next;
    }
    *tailp = a ? a : b;
    return out;
  }

  Node* head_ = nullptr;
  Node* tail_ = nullptr;
  std::size_t size_ = 0;
};

template <class T>
std::size_t LinkedList<T>::bytes() const {
  return size_ * sizeof(Node);
}
