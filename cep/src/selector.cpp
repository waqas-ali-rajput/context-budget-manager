#include "selector.h"

#include <chrono>
#include <cmath>

#include "binary_heap.h"
#include "hash_map.h"
#include "linked_list.h"
#include "merge_sort.h"

const char* policyName(Policy p) {
  switch (p) {
    case Policy::Recency: return "recency-first";
    case Policy::Relevance: return "relevance-first";
    case Policy::Density: return "relevance-per-token";
    case Policy::Hybrid: return "hybrid-fresh-cover";
  }
  return "?";
}
const char* engineName(Engine e) { return e == Engine::Sort ? "merge-sort" : "heap"; }
const char* orderName(OrderRule o) {
  switch (o) {
    case OrderRule::Priority: return "priority (mandatory first, then acceptance order)";
    case OrderRule::Chronological: return "chronological (oldest first)";
    case OrderRule::ReverseChronological: return "reverse-chronological (newest first)";
  }
  return "?";
}
const char* statusName(Status s) {
  switch (s) {
    case Status::Ok: return "OK";
    case Status::Infeasible: return "INFEASIBLE";
    case Status::BudgetViolation: return "BUDGET_VIOLATION";
  }
  return "?";
}
const char* omitName(OmitReason r) {
  switch (r) {
    case OmitReason::ExceedsBudget: return "cost exceeds B_CTX on its own";
    case OmitReason::NoRemainingCapacity: return "insufficient remaining capacity";
    case OmitReason::Superseded: return "superseded by a newer candidate";
    case OmitReason::Redundant: return "duplicate group already covered, no new evidence";
    case OmitReason::InfeasibleMandatory: return "not selected: mandatory set infeasible";
  }
  return "?";
}
bool parsePolicy(const std::string& s, Policy& out) {
  if (s == "recency") out = Policy::Recency;
  else if (s == "relevance") out = Policy::Relevance;
  else if (s == "density") out = Policy::Density;
  else if (s == "hybrid") out = Policy::Hybrid;
  else return false;
  return true;
}
bool parseOrder(const std::string& s, OrderRule& out) {
  if (s == "priority") out = OrderRule::Priority;
  else if (s == "chrono") out = OrderRule::Chronological;
  else if (s == "newest") out = OrderRule::ReverseChronological;
  else return false;
  return true;
}

namespace {

using Clock = std::chrono::steady_clock;

// One tie-breaking chain for every policy: relevance desc, timestamp desc, passage id asc.
bool tieBetter(const Passage& a, const Passage& b) {
  if (a.relevance != b.relevance) return a.relevance > b.relevance;
  if (a.timestamp != b.timestamp) return a.timestamp > b.timestamp;
  return a.passageId < b.passageId;
}

struct Keyed {
  double key;
  int index;
};

struct KeyedBetter {
  const std::vector<Passage>* c;
  bool operator()(const Keyed& x, const Keyed& y) const {
    if (x.key != y.key) return x.key > y.key;
    return tieBetter((*c)[x.index], (*c)[y.index]);
  }
};

double baselineKey(const Passage& p, Policy policy) {
  switch (policy) {
    case Policy::Recency: return static_cast<double>(p.timestamp);
    case Policy::Relevance: return p.relevance;
    default: return p.relevance / (p.cost > 0 ? p.cost : 0.5);  // c = 0 ranks as 0.5 tokens
  }
}

// The feasible-item check shared by all four policies: an indivisible passage is accepted
// only if its whole cost fits the remaining capacity.
class Packer {
 public:
  Packer(const std::vector<Passage>& c, int ctx, Selection& s) : c_(c), ctx_(ctx), remaining_(ctx), s_(s) {}

  bool tryAccept(int i, double priority) {
    const int cost = c_[i].cost;
    if (cost > ctx_) {
      s_.omitted.push_back({i, OmitReason::ExceedsBudget, -1});
      return false;
    }
    if (cost > remaining_) {
      s_.omitted.push_back({i, OmitReason::NoRemainingCapacity, -1});
      return false;
    }
    remaining_ -= cost;
    s_.tokenTotal += cost;
    accepted_.push_back({i, priority, c_[i].mandatory});
    return true;
  }
  void omit(int i, OmitReason r, int by = -1) { s_.omitted.push_back({i, r, by}); }
  void omitUnchecked(int i) {
    omit(i, c_[i].cost > ctx_ ? OmitReason::ExceedsBudget : OmitReason::NoRemainingCapacity);
  }
  int remaining() const { return remaining_; }
  const std::vector<Chosen>& accepted() const { return accepted_; }
  std::size_t bytes() const { return accepted_.capacity() * sizeof(Chosen); }

 private:
  const std::vector<Passage>& c_;
  int ctx_;
  int remaining_;
  Selection& s_;
  std::vector<Chosen> accepted_;
};

// FR-5: mandatory passages go first; if they cannot all fit, report INFEASIBLE and select nothing.
bool mandatoryFeasible(const std::vector<Passage>& c, const std::vector<int>& mand, int ctx, Selection& s) {
  long long need = 0;
  for (int i : mand) need += c[i].cost;
  if (need <= ctx) return true;
  s.status = Status::Infeasible;
  std::string ids;
  for (int i : mand) ids += (ids.empty() ? "" : " + ") + c[i].passageId + "(" + std::to_string(c[i].cost) + ")";
  s.diagnostic = "INFEASIBLE: mandatory passages " + ids + " need " + std::to_string(need) +
                 " tokens but B_CTX = " + std::to_string(ctx) + "; no context was selected.";
  for (std::size_t i = 0; i < c.size(); ++i)
    s.omitted.push_back({static_cast<int>(i), OmitReason::InfeasibleMandatory, -1});
  return false;
}

// Marginal-gain model of the proposed hybrid policy.
class HybridModel {
 public:
  HybridModel(const std::vector<Passage>& c, const Params& p)
      : c_(c), p_(p), ids_(c.size()), staleGroups_(16), covered_(64), groups_(64),
        staleBy_(c.size(), -1), weight_(c.size()), costPow_(c.size()) {
    const int n = static_cast<int>(c.size());
    for (int i = 0; i < n; ++i) ids_.insert(c[i].passageId, i);
    // FR-7: a passage is stale if a candidate supersedes it, or if it is a near-duplicate
    // (same duplicate group) of a superseded passage, e.g. a forum repost of an old notice.
    for (int j = 0; j < n; ++j)
      if (!c[j].supersedes.empty())
        if (const int* f = ids_.find(c[j].supersedes)) staleBy_[*f] = j;
    for (int i = 0; i < n; ++i)
      if (staleBy_[i] >= 0) staleGroups_.insert(c[i].dupGroup, i);
    if (staleGroups_.size() > 0)
      for (int i = 0; i < n; ++i)
        if (staleBy_[i] < 0 && c[i].supersedes.empty())
          if (const int* f = staleGroups_.find(c[i].dupGroup))
            if (*f != i) staleBy_[i] = staleBy_[*f];
    for (int i = 0; i < n; ++i) {
      double w = c[i].relevance * reliabilityWeight(c[i].reliability);
      if (staleBy_[i] >= 0 && !p.excludeStale) w *= p.stalePenalty;
      weight_[i] = w;
      costPow_[i] = std::pow(c[i].cost > 1 ? c[i].cost : 1, p.alpha);
    }
  }

  int staleBy(int i) const { return staleBy_[i]; }
  double weight(int i) const { return weight_[i]; }

  // gain = w * (lambda + (1 - lambda) * share of new evidence tags) * (dupPenalty if group chosen)
  // Non-increasing as the selection grows, which makes lazy re-evaluation exact.
  double priority(int i) const {
    const Passage& p = c_[i];
    double frac = 0.0;
    if (!p.tags.empty()) {
      int fresh = 0;
      for (const std::string& t : p.tags)
        if (!covered_.contains(t)) ++fresh;
      frac = static_cast<double>(fresh) / static_cast<double>(p.tags.size());
    }
    double g = weight_[i] * (p_.lambda + (1.0 - p_.lambda) * frac);
    if (groups_.contains(p.dupGroup)) g *= p_.dupPenalty;
    return g / costPow_[i];
  }

  // A copy of an already chosen group that adds no uncovered evidence tag. Stays true once true.
  bool redundant(int i) const {
    const Passage& p = c_[i];
    if (!groups_.contains(p.dupGroup)) return false;
    for (const std::string& t : p.tags)
      if (!covered_.contains(t)) return false;
    return true;
  }

  void accept(int i) {
    for (const std::string& t : c_[i].tags) covered_.insert(t, i);
    groups_.insert(c_[i].dupGroup, i);
  }

  std::size_t bytes() const {
    return ids_.bytes() + staleGroups_.bytes() + covered_.bytes() + groups_.bytes() +
           staleBy_.capacity() * sizeof(int) + weight_.capacity() * sizeof(double) +
           costPow_.capacity() * sizeof(double);
  }
  long long probes() const { return ids_.probes() + staleGroups_.probes() + covered_.probes() + groups_.probes(); }

 private:
  const std::vector<Passage>& c_;
  const Params& p_;
  HashMap ids_, staleGroups_, covered_, groups_;
  std::vector<int> staleBy_;
  std::vector<double> weight_, costPow_;
};

void splitMandatory(const std::vector<Passage>& c, std::vector<int>& mand, std::vector<int>& opt) {
  for (std::size_t i = 0; i < c.size(); ++i) (c[i].mandatory ? mand : opt).push_back(static_cast<int>(i));
}

// Sorts mandatory passages by key so they enter the package in a deterministic order.
std::vector<Keyed> orderedMandatory(const std::vector<Passage>& c, const std::vector<int>& mand,
                                    const std::vector<double>& key, long long& cmps) {
  std::vector<Keyed> m;
  for (int i : mand) m.push_back({key[i], i});
  cmps += mergeSort(m, KeyedBetter{&c});
  return m;
}

struct PackItem {
  Chosen chosen;
  int rank;
};

// FR-8: assemble the package in a linked list and apply the ordering rule with list merge sort.
std::size_t packageInOrder(const std::vector<Passage>& c, const std::vector<Chosen>& accepted, OrderRule order,
                           Selection& s) {
  LinkedList<PackItem> list;
  for (std::size_t r = 0; r < accepted.size(); ++r) list.pushBack({accepted[r], static_cast<int>(r)});
  if (order == OrderRule::Priority) {
    list.sort([](const PackItem& a, const PackItem& b) {
      if (a.chosen.mandatory != b.chosen.mandatory) return a.chosen.mandatory;
      return a.rank < b.rank;
    });
  } else {
    const bool newestFirst = order == OrderRule::ReverseChronological;
    list.sort([&c, newestFirst](const PackItem& a, const PackItem& b) {
      const Passage& x = c[a.chosen.index];
      const Passage& y = c[b.chosen.index];
      if (x.timestamp != y.timestamp) return newestFirst ? x.timestamp > y.timestamp : x.timestamp < y.timestamp;
      return x.passageId < y.passageId;
    });
  }
  const std::size_t bytes = list.bytes();
  list.forEach([&s](const PackItem& it) { s.ordered.push_back(it.chosen); });
  return bytes;
}

void runBaseline(const std::vector<Passage>& c, Policy policy, const Params& prm, Packer& packer,
                 const std::vector<int>& mand, const std::vector<int>& opt, Selection& s) {
  const Clock::time_point t0 = Clock::now();
  const int n = static_cast<int>(c.size());
  std::vector<double> key(n);
  for (int i = 0; i < n; ++i) key[i] = baselineKey(c[i], policy);
  for (const Keyed& k : orderedMandatory(c, mand, key, s.metrics.comparisons)) packer.tryAccept(k.index, k.key);

  std::size_t aux = key.capacity() * sizeof(double);
  if (prm.engine == Engine::Sort) {
    // Sort all optional candidates once, then a single first-fit scan.
    std::vector<Keyed> order;
    order.reserve(opt.size());
    for (int i : opt) order.push_back({key[i], i});
    s.metrics.prepMicros = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
    s.metrics.comparisons += mergeSort(order, KeyedBetter{&c});
    aux += 2 * order.capacity() * sizeof(Keyed);  // array + merge buffer
    for (const Keyed& k : order) packer.tryAccept(k.index, k.key);
  } else {
    // Heapify in O(n), pop best-first, stop once nothing left can fit.
    std::vector<Keyed> items;
    items.reserve(opt.size());
    int minCost = 0;
    for (std::size_t j = 0; j < opt.size(); ++j) {
      const int i = opt[j];
      items.push_back({key[i], i});
      if (j == 0 || c[i].cost < minCost) minCost = c[i].cost;
    }
    s.metrics.prepMicros = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
    BinaryHeap<Keyed, KeyedBetter> heap(KeyedBetter{&c});
    heap.build(std::move(items));
    aux += heap.bytes();
    while (!heap.empty() && packer.remaining() >= minCost) {
      Keyed k = heap.pop();
      ++s.metrics.heapPops;
      packer.tryAccept(k.index, k.key);
    }
    for (const Keyed& k : heap.items()) packer.omitUnchecked(k.index);
    s.metrics.comparisons += heap.comparisons();
  }
  s.metrics.auxBytes = aux + packer.bytes();
}

void runHybrid(const std::vector<Passage>& c, int ctx, const Params& prm, Packer& packer,
               const std::vector<int>& mand, const std::vector<int>& opt, Selection& s) {
  const Clock::time_point t0 = Clock::now();
  HybridModel model(c, prm);
  std::vector<double> w(c.size());
  for (std::size_t i = 0; i < c.size(); ++i) w[i] = model.weight(static_cast<int>(i));
  for (const Keyed& k : orderedMandatory(c, mand, w, s.metrics.comparisons)) {
    packer.tryAccept(k.index, k.key);
    model.accept(k.index);
  }

  std::vector<Keyed> items;
  items.reserve(opt.size());
  int minCost = 0;
  bool any = false;
  for (int i : opt) {
    if (prm.excludeStale && model.staleBy(i) >= 0) {
      packer.omit(i, OmitReason::Superseded, model.staleBy(i));
      continue;
    }
    if (c[i].cost > ctx) {
      packer.omit(i, OmitReason::ExceedsBudget);
      continue;
    }
    items.push_back({model.priority(i), i});
    if (!any || c[i].cost < minCost) minCost = c[i].cost;
    any = true;
  }

  // Lazy greedy: stored keys are upper bounds because gains never increase. Pop the best
  // bound, recompute its true priority; accept it if it still beats the next bound,
  // otherwise push it back with the updated key.
  s.metrics.prepMicros = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
  KeyedBetter better{&c};
  BinaryHeap<Keyed, KeyedBetter> heap(better);
  heap.build(std::move(items));
  const std::size_t heapBytes = heap.bytes();
  while (!heap.empty() && packer.remaining() >= minCost) {
    Keyed top = heap.pop();
    ++s.metrics.heapPops;
    if (c[top.index].cost > packer.remaining()) {
      packer.omit(top.index, OmitReason::NoRemainingCapacity);
      continue;
    }
    if (prm.dropRedundant && model.redundant(top.index)) {
      packer.omit(top.index, OmitReason::Redundant);
      continue;
    }
    Keyed fresh{model.priority(top.index), top.index};
    if (fresh.key < top.key && !heap.empty() && better(heap.top(), fresh)) {
      heap.push(fresh);
      ++s.metrics.reinserts;
      continue;
    }
    packer.tryAccept(top.index, fresh.key);
    model.accept(top.index);
  }
  for (const Keyed& k : heap.items()) packer.omitUnchecked(k.index);
  s.metrics.comparisons += heap.comparisons();
  s.metrics.hashProbes = model.probes();
  s.metrics.auxBytes = model.bytes() + heapBytes + w.capacity() * sizeof(double) + packer.bytes();
}

}  // namespace

Selection selectContext(const std::string& question, const std::vector<Passage>& candidates, int budgetCtx,
                        Policy policy, const Params& params) {
  (void)question;
  const Clock::time_point t0 = Clock::now();
  Selection s;
  s.policy = policy;
  s.budgetCtx = budgetCtx < 0 ? 0 : budgetCtx;
  const std::vector<Passage>& c = candidates;

  std::vector<int> mand, opt;
  splitMandatory(c, mand, opt);
  if (mandatoryFeasible(c, mand, s.budgetCtx, s)) {
    Packer packer(c, s.budgetCtx, s);
    if (policy == Policy::Hybrid) runHybrid(c, s.budgetCtx, params, packer, mand, opt, s);
    else runBaseline(c, policy, params, packer, mand, opt, s);
    s.metrics.auxBytes += packageInOrder(c, packer.accepted(), params.order, s);

    // Budget validator: recompute the total independently of the packer.
    long long total = 0;
    for (const Chosen& ch : s.ordered) total += c[ch.index].cost;
    if (total > s.budgetCtx || total != s.tokenTotal) {
      s.status = Status::BudgetViolation;
      s.diagnostic = "selected cost " + std::to_string(total) + " exceeds B_CTX " + std::to_string(s.budgetCtx);
    }
  }
  s.metrics.selectMicros = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
  return s;
}

std::vector<int> hybridEagerReference(const std::vector<Passage>& c, int ctx, const Params& prm) {
  std::vector<int> out;
  std::vector<int> mand, opt;
  splitMandatory(c, mand, opt);
  long long need = 0;
  for (int i : mand) need += c[i].cost;
  if (need > ctx) return out;
  HybridModel model(c, prm);
  std::vector<double> w(c.size());
  for (std::size_t i = 0; i < c.size(); ++i) w[i] = model.weight(static_cast<int>(i));
  long long dummy = 0;
  long long remaining = ctx;
  for (const Keyed& k : orderedMandatory(c, mand, w, dummy)) {
    out.push_back(k.index);
    remaining -= c[k.index].cost;
    model.accept(k.index);
  }
  std::vector<int> pool;
  for (int i : opt)
    if (!(prm.excludeStale && model.staleBy(i) >= 0) && c[i].cost <= ctx) pool.push_back(i);
  KeyedBetter better{&c};
  while (!pool.empty()) {
    std::size_t bestAt = 0;
    Keyed best{model.priority(pool[0]), pool[0]};
    for (std::size_t j = 1; j < pool.size(); ++j) {
      Keyed k{model.priority(pool[j]), pool[j]};
      if (better(k, best)) {
        best = k;
        bestAt = j;
      }
    }
    pool.erase(pool.begin() + static_cast<long>(bestAt));
    if (c[best.index].cost > remaining) continue;
    if (prm.dropRedundant && model.redundant(best.index)) continue;
    out.push_back(best.index);
    remaining -= c[best.index].cost;
    model.accept(best.index);
  }
  return out;
}
