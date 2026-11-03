#pragma once
// Context Budget Manager core: select_context(question, candidates, B_CTX, policy, params).
// Independent of any model API; the agent adapter consumes its output (FR-9).
#include <cstddef>
#include <string>
#include <vector>

#include "passage.h"

enum class Policy { Recency, Relevance, Density, Hybrid };
enum class Engine { Sort, Heap };  // baselines: merge sort + scan, or heap + pop
enum class OrderRule { Priority, Chronological, ReverseChronological };
enum class Status { Ok, Infeasible, BudgetViolation };
enum class OmitReason { ExceedsBudget, NoRemainingCapacity, Superseded, Redundant, InfeasibleMandatory };

// Token budgeting model of Section 5: B_CTX = max(0, B_total - R_fixed - R_answer).
struct Budget {
  int total = 0;
  int reserveFixed = 0;
  int reserveAnswer = 0;
  int context() const {
    long long v = static_cast<long long>(total) - reserveFixed - reserveAnswer;
    return v > 0 ? static_cast<int>(v) : 0;
  }
};

struct Params {
  Engine engine = Engine::Sort;          // baselines only (hybrid always uses the lazy heap)
  OrderRule order = OrderRule::Priority; // packaging order
  double alpha = 0.5;                    // hybrid: priority = gain / cost^alpha
  double lambda = 0.2;                   // hybrid: share of gain kept by a passage with no new evidence
  double dupPenalty = 0.3;               // hybrid: gain multiplier once its duplicate group is chosen
  bool excludeStale = true;              // hybrid: drop passages superseded by a candidate
  double stalePenalty = 0.25;            // hybrid: weight multiplier when excludeStale is false
  bool dropRedundant = true;             // hybrid: omit a copy whose group is chosen and adds no new tag
};

struct Chosen {
  int index = 0;        // into the candidate array
  double priority = 0;  // key at the moment of acceptance (rationale)
  bool mandatory = false;
};

struct Omission {
  int index = 0;
  OmitReason reason = OmitReason::NoRemainingCapacity;
  int supersededBy = -1;  // candidate index of the newer passage (Superseded only)
};

struct Metrics {
  double selectMicros = 0;
  double prepMicros = 0;      // building keys / hash index / candidate array, before sort or heap work
  long long comparisons = 0;  // element comparisons in sort / heap
  long long heapPops = 0;
  long long reinserts = 0;    // lazy re-evaluations pushed back (hybrid)
  long long hashProbes = 0;
  std::size_t auxBytes = 0;   // peak auxiliary memory estimate
};

struct Selection {
  Policy policy = Policy::Hybrid;
  Status status = Status::Ok;
  int budgetCtx = 0;
  int tokenTotal = 0;
  std::vector<Chosen> ordered;      // final context order
  std::vector<Omission> omitted;    // every unselected candidate with its reason
  std::string diagnostic;           // INFEASIBLE explanation
  Metrics metrics;
};

const char* policyName(Policy p);
const char* engineName(Engine e);
const char* orderName(OrderRule o);
const char* statusName(Status s);
const char* omitName(OmitReason r);
bool parsePolicy(const std::string& s, Policy& out);
bool parseOrder(const std::string& s, OrderRule& out);

// The question text is carried for logging and the agent; ranking uses the retriever's
// relevance scores, so selection itself does not re-read the question.
Selection selectContext(const std::string& question, const std::vector<Passage>& candidates,
                        int budgetCtx, Policy policy, const Params& params);

// Eager O(n^2) greedy that recomputes every marginal gain each step. Test oracle only:
// the lazy heap version must accept exactly the same passages in the same order.
std::vector<int> hybridEagerReference(const std::vector<Passage>& candidates, int budgetCtx,
                                      const Params& params);
