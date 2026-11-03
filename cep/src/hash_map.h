#pragma once
// Student-written open-addressing hash map: std::string key -> int value.
// Linear probing, FNV-1a hash, power-of-two capacity, grows (doubles) above load 0.6.
// Used for: unique passage ids at load, id -> index for superseded links,
// covered evidence tags and chosen duplicate groups in the hybrid policy.
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class HashMap {
 public:
  explicit HashMap(std::size_t expected = 8) {
    std::size_t cap = 16;
    while (cap * 3 < expected * 5) cap <<= 1;  // keep load <= 0.6
    slots_.assign(cap, Slot());
  }

  // Inserts key -> value if the key is absent. Returns false if it was already present.
  bool insert(const std::string& key, int value) {
    if ((count_ + 1) * 5 > slots_.size() * 3) grow();
    std::size_t i = probe(key);
    if (slots_[i].used) return false;
    slots_[i].used = true;
    slots_[i].key = key;
    slots_[i].value = value;
    ++count_;
    return true;
  }

  const int* find(const std::string& key) const {
    std::size_t i = probe(key);
    return slots_[i].used ? &slots_[i].value : nullptr;
  }
  bool contains(const std::string& key) const { return find(key) != nullptr; }

  std::size_t size() const { return count_; }
  std::size_t capacity() const { return slots_.size(); }
  std::size_t bytes() const { return slots_.size() * sizeof(Slot); }
  long long probes() const { return probes_; }

  // Invariant check used by the tests: every stored key is reachable from its home slot.
  bool checkInvariants() const {
    std::size_t used = 0;
    for (const Slot& s : slots_) {
      if (!s.used) continue;
      ++used;
      if (find(s.key) != &s.value) return false;
    }
    return used == count_ && count_ * 5 <= slots_.size() * 3;
  }

 private:
  struct Slot {
    std::string key;
    int value = 0;
    bool used = false;
  };

  static std::uint64_t hash(const std::string& s) {
    std::uint64_t h = 1469598103934665603ULL;  // FNV-1a 64-bit
    for (unsigned char ch : s) {
      h ^= ch;
      h *= 1099511628211ULL;
    }
    return h;
  }

  std::size_t probe(const std::string& key) const {
    const std::size_t mask = slots_.size() - 1;
    std::size_t i = static_cast<std::size_t>(hash(key)) & mask;
    while (slots_[i].used && slots_[i].key != key) {
      i = (i + 1) & mask;
      ++probes_;
    }
    return i;
  }

  void grow() {
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.assign(old.size() * 2, Slot());
    count_ = 0;
    for (Slot& s : old) {
      if (!s.used) continue;
      std::size_t i = probe(s.key);
      slots_[i] = std::move(s);
      ++count_;
    }
  }

  std::vector<Slot> slots_;
  std::size_t count_ = 0;
  mutable long long probes_ = 0;
};
