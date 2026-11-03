#pragma once
// Student-written top-down merge sort on an array (std::vector used only as storage).
// Stable, O(n log n) time in every case, O(n) auxiliary buffer allocated once.
// Skips the merge when the two halves are already in order (best case O(n)).
#include <cstddef>
#include <vector>

namespace detail {
template <class T, class Less>
void mergeSortRange(std::vector<T>& a, std::vector<T>& buf, std::size_t lo, std::size_t hi,
                    Less& less, long long& cmps) {
  if (hi - lo < 2) return;
  const std::size_t mid = lo + (hi - lo) / 2;
  mergeSortRange(a, buf, lo, mid, less, cmps);
  mergeSortRange(a, buf, mid, hi, less, cmps);
  ++cmps;
  if (!less(a[mid], a[mid - 1])) return;  // halves already ordered
  std::size_t i = lo, j = mid, k = lo;
  while (i < mid && j < hi) {
    ++cmps;
    if (less(a[j], a[i])) buf[k++] = a[j++];  // take right only if strictly smaller: stable
    else buf[k++] = a[i++];
  }
  while (i < mid) buf[k++] = a[i++];
  while (j < hi) buf[k++] = a[j++];
  for (k = lo; k < hi; ++k) a[k] = buf[k];
}
}  // namespace detail

template <class T, class Less>
long long mergeSort(std::vector<T>& a, Less less) {
  long long cmps = 0;
  if (a.size() < 2) return 0;
  std::vector<T> buf(a.size());
  detail::mergeSortRange(a, buf, 0, a.size(), less, cmps);
  return cmps;
}
