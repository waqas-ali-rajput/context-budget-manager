// Automated test suite: `cbm test`. Writes one PASS/FAIL line per test.
#include <cstdio>
#include <fstream>
#include <sstream>

#include "agent.h"
#include "binary_heap.h"
#include "dataset.h"
#include "experiments.h"
#include "hash_map.h"
#include "linked_list.h"
#include "loader.h"
#include "merge_sort.h"

namespace {

struct Suite {
  std::ostream& out;
  int passed = 0, failed = 0;
  void check(const char* id, const char* name, bool ok, const std::string& detail = "") {
    out << id << "  " << (ok ? "PASS" : "FAIL") << "  " << name;
    if (!detail.empty()) out << "  [" << detail << "]";
    out << "\n";
    (ok ? passed : failed)++;
  }
};

const Policy kAll[4] = {Policy::Recency, Policy::Relevance, Policy::Density, Policy::Hybrid};

Passage mk(const std::string& id, int cost, double rel, long long ts = 202601011000LL, bool mandatory = false,
           const std::string& tag = "", const std::string& group = "", const std::string& supersedes = "",
           char reliability = 'B') {
  Passage p;
  p.queryId = "T";
  p.passageId = id;
  p.sourceId = "src-" + id;
  p.timestamp = ts;
  p.cost = cost;
  p.relevance = rel;
  p.reliability = reliability;
  p.dupGroup = group.empty() ? id : group;
  if (!tag.empty()) p.tags = {tag};
  p.mandatory = mandatory;
  p.supersedes = supersedes;
  p.text = tag;
  return p;
}

std::vector<std::string> ids(const std::vector<Passage>& c, const Selection& s) {
  std::vector<std::string> out;
  for (const Chosen& ch : s.ordered) out.push_back(c[ch.index].passageId);
  return out;
}

// Random instance with ties, duplicates, mandatory items, zero costs and supersede links.
std::vector<Passage> randomInstance(Rng& r, int n) {
  std::vector<Passage> c;
  for (int i = 0; i < n; ++i) {
    Passage p = mk("R" + std::to_string(i), r.chance(0.05) ? 0 : r.range(1, 400), r.range(0, 20) / 20.0,
                   202600000000LL + r.range(1, 5) * 10000LL, r.chance(0.08),
                   "k" + std::to_string(r.range(0, n / 2 + 1)) + ":v");
    if (i > 0 && r.chance(0.25)) p.dupGroup = c[r.range(0, i - 1)].dupGroup;
    if (i > 0 && r.chance(0.1)) p.supersedes = c[r.range(0, i - 1)].passageId;
    p.reliability = "ABC"[r.range(0, 2)];
    c.push_back(p);
  }
  return c;
}

bool readFile(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

}  // namespace

int runTests(const std::string& dataDir, std::ostream& out) {
  Suite t{out};
  Params prm;

  // ---------- budget model and edge cases ----------
  {
    bool ok = true;
    for (Policy p : kAll) {
      Selection s = selectContext("q", {}, 512, p, prm);
      ok = ok && s.status == Status::Ok && s.ordered.empty() && s.omitted.empty() && s.tokenTotal == 0;
    }
    t.check("T01", "empty candidate set -> OK, empty package, all policies", ok);
  }
  t.check("T02", "B_CTX = max(0, B_total - R_fixed - R_answer)",
          Budget{1312, 300, 500}.context() == 512 && Budget{700, 300, 500}.context() == 0 &&
              Budget{800, 300, 500}.context() == 0 && Budget{1824, 300, 500}.context() == 1024);
  {
    std::vector<Passage> c = {mk("A", 20, 0.9), mk("B", 30, 0.5)};
    bool ok = true;
    for (Policy p : kAll) {
      Selection s = selectContext("q", c, 0, p, prm);
      ok = ok && s.status == Status::Ok && s.ordered.empty() && s.omitted.size() == 2 &&
           s.omitted[0].reason == OmitReason::ExceedsBudget;
    }
    t.check("T03", "zero B_CTX without mandatory items -> OK, nothing selected, reasons given", ok);
  }
  {
    std::vector<Passage> c = {mk("M", 50, 0.9, 202601011000LL, true), mk("B", 30, 0.5)};
    bool ok = true;
    for (Policy p : kAll) {
      Selection s = selectContext("q", c, 0, p, prm);
      ok = ok && s.status == Status::Infeasible && s.ordered.empty() && !s.diagnostic.empty();
    }
    t.check("T04", "zero B_CTX with a mandatory item -> INFEASIBLE with diagnostic", ok);
  }
  {
    std::vector<Passage> c = {mk("A", 300, 0.9), mk("B", 400, 0.8), mk("C", 500, 0.7)};
    bool ok = true;
    for (Policy p : kAll) {
      Selection s = selectContext("q", c, 256, p, prm);
      ok = ok && s.ordered.empty() && s.omitted.size() == 3;
      for (const Omission& o : s.omitted) ok = ok && o.reason == OmitReason::ExceedsBudget;
    }
    t.check("T05", "no item fits -> empty package, every item 'exceeds B_CTX'", ok);
  }
  {
    std::vector<Passage> c = {mk("A", 40, 0.5, 202601011000LL, false, "a:1"), mk("B", 60, 0.6),
                              mk("BIG", 900, 0.99, 202602011000LL)};
    bool ok = true;
    for (Policy p : kAll) {
      Selection s = selectContext("q", c, 256, p, prm);
      ok = ok && s.ordered.size() == 2 && s.tokenTotal == 100;
      for (const Omission& o : s.omitted) ok = ok && c[o.index].passageId == "BIG" && o.reason == OmitReason::ExceedsBudget;
    }
    t.check("T06", "one item larger than B_CTX is omitted, the rest still selected", ok);
  }
  {
    std::vector<Passage> c = {mk("M1", 150, 0.6, 202601011000LL, true), mk("M2", 180, 0.7, 202601011000LL, true),
                              mk("O", 30, 0.9)};
    bool ok = true;
    for (Policy p : kAll) {
      Selection s = selectContext("q", c, 256, p, prm);
      ok = ok && s.status == Status::Infeasible && s.ordered.empty() &&
           s.diagnostic.find("M1") != std::string::npos && s.diagnostic.find("M2") != std::string::npos &&
           s.diagnostic.find("330") != std::string::npos && s.omitted.size() == 3;
    }
    t.check("T07", "two mandatory items over budget -> INFEASIBLE naming both and their total", ok);
  }
  {
    std::vector<Passage> c = {mk("O1", 100, 0.99), mk("M1", 60, 0.1, 202501011000LL, true), mk("O2", 50, 0.95),
                              mk("M2", 40, 0.2, 202501011000LL, true)};
    bool ok = true;
    for (Policy p : kAll) {
      Selection s = selectContext("q", c, 160, p, prm);
      ok = ok && s.status == Status::Ok && s.ordered.size() >= 2 && s.ordered[0].mandatory && s.ordered[1].mandatory;
      ok = ok && s.tokenTotal <= 160;
    }
    t.check("T08", "feasible mandatory items are always selected and placed first", ok);
  }
  {
    std::vector<Passage> c = {mk("T3", 50, 0.7), mk("T1", 50, 0.7), mk("T4", 50, 0.7), mk("T2", 50, 0.7)};
    bool ok = true;
    Rng r(7);
    for (Policy p : kAll) {
      Selection ref = selectContext("q", c, 120, p, prm);
      std::vector<std::string> want = {"T1", "T2"};
      ok = ok && ids(c, ref) == want;
      for (int k = 0; k < 20; ++k) {  // input permutations must not change the result
        std::vector<Passage> d = c;
        for (int i = 3; i > 0; --i) {
          const int j = r.range(0, i);
          Passage tmp = d[i];
          d[i] = d[j];
          d[j] = tmp;
        }
        ok = ok && ids(d, selectContext("q", d, 120, p, prm)) == want;
      }
    }
    t.check("T09", "tied relevance and timestamps -> passage-id tie-break, independent of input order", ok);
  }

  // ---------- ingestion ----------
  {
    LoadReport rep;
    std::vector<Passage> got = loadPassages(dataDir + "/fixtures/malformed_passages.txt", rep);
    bool dup = false;
    for (const LoadError& e : rep.errors) dup = dup || (e.message.find("duplicate passage id") != std::string::npos && e.line > 0);
    t.check("T10", "loader rejects a duplicate id with its line number; valid records still load",
            dup && rep.accepted == 2 && got.size() == 2,
            std::to_string(rep.accepted) + " accepted, " + std::to_string(rep.errors.size()) + " rejected");
    const char* expect[] = {"negative token cost", "not a nonnegative integer", "not a number in [0,1]",
                            "invalid timestamp", "not A, B or C", "not key:value", "not 0 or 1", "expected 12 fields",
                            "missing passage_id", "missing source_id", "supersedes itself"};
    bool all = rep.errors.size() == 15;
    for (const char* e : expect) {
      bool seen = false;
      for (const LoadError& x : rep.errors) seen = seen || x.message.find(e) != std::string::npos;
      all = all && seen;
    }
    t.check("T11", "loader gives a specific reason for each malformed field (15 bad records)", all);
  }

  // ---------- redundancy, freshness, provenance ----------
  {
    std::vector<Passage> c;
    for (int k = 0; k < 5; ++k) c.push_back(mk("D" + std::to_string(k), 30, 0.9 - 0.01 * k, 202601011000LL, false, "a:1", "G1"));
    c.push_back(mk("U", 40, 0.5, 202601011000LL, false, "b:2"));
    Scenario s;
    s.gold = {"a:1", "b:2"};
    s.candidates = c;
    SelStats rel = measureSelection(s, selectContext("q", c, 160, Policy::Relevance, prm));
    SelStats hyb = measureSelection(s, selectContext("q", c, 160, Policy::Hybrid, prm));
    t.check("T12", "near-duplicate group: hybrid keeps one copy and covers both facts; relevance-first fills with copies",
            hyb.redundancy == 0 && hyb.goldHit == 2 && rel.redundancy == 4 && rel.goldHit == 1,
            "relevance redundancy=" + std::to_string(rel.redundancy) + " hybrid redundancy=" + std::to_string(hyb.redundancy));
  }
  {
    std::vector<Passage> c = {mk("OLD", 80, 0.92, 202503011000LL, false, "deadline:27 Feb", "GO"),
                              mk("NEW", 60, 0.70, 202602011000LL, false, "deadline:6 Mar", "GN", "OLD"),
                              mk("REPOST", 40, 0.88, 202504011000LL, false, "deadline:27 Feb", "GO")};
    Scenario s;
    s.gold = {"deadline:6 Mar"};
    s.candidates = c;
    Selection h = selectContext("q", c, 200, Policy::Hybrid, prm);
    SelStats hs = measureSelection(s, h);
    SelStats rs = measureSelection(s, selectContext("q", c, 200, Policy::Relevance, prm));
    int superseded = 0;
    for (const Omission& o : h.omitted)
      if (o.reason == OmitReason::Superseded && c[o.supersededBy].passageId == "NEW") ++superseded;
    t.check("T13", "contradictory old/new policy: hybrid omits the old notice and its repost as superseded",
            hs.stale == 0 && hs.goldHit == 1 && superseded == 2 && rs.stale == 2);
  }

  // ---------- randomized properties ----------
  {
    Rng r(99);
    int instances = 0, viol = 0, wrongStatus = 0, partition = 0, nondet = 0;
    for (int it = 0; it < 3000; ++it) {
      std::vector<Passage> c = randomInstance(r, r.range(0, 60));
      const int ctx = r.range(0, 1500);
      long long need = 0;
      for (const Passage& p : c) need += p.mandatory ? p.cost : 0;
      for (Policy p : kAll)
        for (Engine e : {Engine::Sort, Engine::Heap}) {
          Params q;
          q.engine = e;
          Selection a = selectContext("q", c, ctx, p, q);
          Selection b = selectContext("q", c, ctx, p, q);
          ++instances;
          long long tot = 0;
          for (const Chosen& ch : a.ordered) tot += c[ch.index].cost;
          if (tot > ctx || a.status == Status::BudgetViolation) ++viol;
          if ((need > ctx) != (a.status == Status::Infeasible)) ++wrongStatus;
          std::vector<int> seen(c.size(), 0);
          for (const Chosen& ch : a.ordered) ++seen[ch.index];
          for (const Omission& o : a.omitted) ++seen[o.index];
          for (int x : seen) partition += x != 1;
          if (ids(c, a) != ids(c, b)) ++nondet;
        }
    }
    t.check("T14", "budget safety: 3000 random instances x 4 policies x 2 engines never exceed B_CTX",
            viol == 0 && wrongStatus == 0 && partition == 0 && nondet == 0,
            std::to_string(instances) + " runs, violations=" + std::to_string(viol) + ", wrong status=" +
                std::to_string(wrongStatus) + ", not partitioned=" + std::to_string(partition) + ", nondeterministic=" +
                std::to_string(nondet));
  }
  {
    Rng r(123);
    int mismatch = 0, runs = 0;
    for (int it = 0; it < 1000; ++it) {
      std::vector<Passage> c = randomInstance(r, r.range(0, 80));
      const int ctx = r.range(0, 2000);
      for (Policy p : {Policy::Recency, Policy::Relevance, Policy::Density}) {
        Params a, b;
        a.engine = Engine::Sort;
        b.engine = Engine::Heap;
        ++runs;
        if (ids(c, selectContext("q", c, ctx, p, a)) != ids(c, selectContext("q", c, ctx, p, b))) ++mismatch;
      }
    }
    t.check("T15", "heap engine selects exactly what the merge-sort engine selects (baselines)", mismatch == 0,
            std::to_string(runs) + " runs, mismatches=" + std::to_string(mismatch));
  }
  {
    Rng r(321);
    int mismatch = 0;
    long long reinserts = 0;
    for (int it = 0; it < 1000; ++it) {
      std::vector<Passage> c = randomInstance(r, r.range(0, 80));
      const int ctx = r.range(0, 2000);
      Params q;
      q.alpha = r.range(0, 2) * 0.5;
      q.excludeStale = r.chance(0.5);
      Selection s = selectContext("q", c, ctx, Policy::Hybrid, q);
      reinserts += s.metrics.reinserts;
      std::vector<int> lazy;
      for (const Chosen& ch : s.ordered) lazy.push_back(ch.index);
      if (s.status == Status::Infeasible) lazy.clear();
      if (lazy != hybridEagerReference(c, ctx, q)) ++mismatch;
    }
    t.check("T16", "lazy-heap hybrid == eager O(n^2) greedy on 1000 random instances", mismatch == 0,
            "mismatches=" + std::to_string(mismatch) + ", lazy re-insertions exercised=" + std::to_string(reinserts));
  }

  // ---------- student-built structures ----------
  {
    Rng r(5);
    auto less = [](int a, int b) { return a < b; };
    BinaryHeap<int, decltype(less)> h(less);  // min-heap
    std::vector<int> ref;
    bool ok = true;
    for (int op = 0; op < 20000; ++op) {
      if (h.empty() || r.chance(0.6)) {
        int x = r.range(-1000, 1000);
        h.push(x);
        ref.push_back(x);
      } else {
        int best = 0;
        for (std::size_t i = 1; i < ref.size(); ++i)
          if (ref[i] < ref[best]) best = static_cast<int>(i);
        ok = ok && h.pop() == ref[best];
        ref.erase(ref.begin() + best);
      }
      if (op % 97 == 0) ok = ok && h.isHeap();
    }
    std::vector<int> v;
    for (int i = 0; i < 5000; ++i) v.push_back(r.range(0, 100000));
    BinaryHeap<int, decltype(less)> b(less);
    b.build(v);
    const long long buildCmps = b.comparisons();
    ok = ok && b.isHeap() && buildCmps <= 2 * 5000;
    t.check("T17", "binary heap: 20000 random push/pop match a reference; Floyd build keeps heap order in O(n)", ok,
            "build comparisons for n=5000: " + std::to_string(buildCmps));
  }
  {
    Rng r(6);
    bool ok = true;
    for (int it = 0; it < 300; ++it) {
      std::vector<std::pair<int, int>> v;
      const int n = r.range(0, 300);
      for (int i = 0; i < n; ++i) v.push_back({r.range(0, 20), i});
      std::vector<std::pair<int, int>> w = v;
      mergeSort(w, [](const std::pair<int, int>& a, const std::pair<int, int>& b) { return a.first < b.first; });
      for (std::size_t i = 1; i < w.size(); ++i)
        ok = ok && (w[i - 1].first < w[i].first || (w[i - 1].first == w[i].first && w[i - 1].second < w[i].second));
      ok = ok && w.size() == v.size();
    }
    t.check("T18", "merge sort: 300 random arrays sorted and stable (equal keys keep input order)", ok);
  }
  {
    Rng r(8);
    bool ok = true;
    for (int it = 0; it < 300; ++it) {
      LinkedList<std::pair<int, int>> list;
      const int n = r.range(0, 200);
      for (int i = 0; i < n; ++i) list.pushBack({r.range(0, 15), i});
      list.sort([](const std::pair<int, int>& a, const std::pair<int, int>& b) { return a.first < b.first; });
      std::vector<std::pair<int, int>> got;
      list.forEach([&got](const std::pair<int, int>& x) { got.push_back(x); });
      ok = ok && static_cast<int>(got.size()) == n && static_cast<int>(list.size()) == n;
      for (std::size_t i = 1; i < got.size(); ++i)
        ok = ok && (got[i - 1].first < got[i].first || (got[i - 1].first == got[i].first && got[i - 1].second < got[i].second));
      list.pushBack({-1, -1});  // tail pointer still valid after sort
      int last = 0;
      list.forEach([&last](const std::pair<int, int>& x) { last = x.first; });
      ok = ok && last == -1;
    }
    t.check("T19", "linked-list merge sort: sorted, stable, tail pointer valid afterwards", ok);
  }
  {
    HashMap m(4);
    bool ok = true;
    for (int i = 0; i < 20000; ++i) ok = ok && m.insert("key" + std::to_string(i), i);
    for (int i = 0; i < 20000; ++i) {
      const int* f = m.find("key" + std::to_string(i));
      ok = ok && f && *f == i;
    }
    ok = ok && !m.insert("key5", 1) && !m.contains("key20000") && !m.contains("") && m.size() == 20000;
    ok = ok && m.checkInvariants();
    t.check("T20", "hash map: 20000 inserts with growth, all found, duplicates refused, load <= 0.6", ok,
            "capacity " + std::to_string(m.capacity()));
  }

  // ---------- packaging, zero cost, agent ----------
  {
    std::vector<Passage> c = {mk("Z", 0, 0.3, 202601011000LL, false, "a:1"), mk("Y", 100, 0.9, 202602011000LL, false, "b:2")};
    bool ok = true;
    for (Policy p : kAll) {
      Selection s = selectContext("q", c, 100, p, prm);
      ok = ok && s.ordered.size() == 2 && s.tokenTotal == 100;
    }
    t.check("T21", "zero-cost passage is accepted without breaking the budget or the density key", ok);
  }
  {
    std::vector<Passage> c = {mk("A", 10, 0.9, 202603011000LL), mk("B", 10, 0.8, 202601011000LL),
                              mk("C", 10, 0.7, 202602011000LL)};
    Params a, b;
    a.order = OrderRule::Chronological;
    b.order = OrderRule::ReverseChronological;
    std::vector<std::string> chrono = {"B", "C", "A"}, newest = {"A", "C", "B"}, prio = {"A", "B", "C"};
    t.check("T22", "packaging order rules: priority, oldest-first, newest-first",
            ids(c, selectContext("q", c, 100, Policy::Relevance, prm)) == prio &&
                ids(c, selectContext("q", c, 100, Policy::Relevance, a)) == chrono &&
                ids(c, selectContext("q", c, 100, Policy::Relevance, b)) == newest);
  }
  {
    Scenario s;
    s.queryId = "T";
    s.question = "deadline?";
    s.askKeys = {"deadline"};
    s.gold = {"deadline:6 Mar"};
    s.candidates = {mk("OLD", 50, 0.9, 202503011000LL, false, "deadline:27 Feb", "", "", 'C'),
                    mk("NEW", 50, 0.5, 202602011000LL, false, "deadline:6 Mar", "", "", 'A')};
    Selection sel = selectContext("q", s.candidates, 200, Policy::Relevance, prm);
    ContextPackage pkg = buildPackage(s, sel, OrderRule::Priority);
    AnswerScore f = scoreAnswer(FirstMentionAgent().answer(pkg, s.askKeys), s);
    AnswerScore a = scoreAnswer(AuthorityAgent().answer(pkg, s.askKeys), s);
    t.check("T23", "agents: first-mention repeats the stale value; authority picks the official one and flags the conflict",
            f.contradictory == 1 && a.correct == 1 && a.conflictsFlagged == 1);
    Scenario u;
    u.question = "parking?";
    u.askKeys = {"parking_fee"};
    u.candidates = {mk("X", 20, 0.4, 202601011000LL, false, "misc:1")};
    AnswerScore z = scoreAnswer(AuthorityAgent().answer(buildPackage(u, selectContext("q", u.candidates, 100, Policy::Hybrid, prm),
                                                                     OrderRule::Priority), u.askKeys), u);
    t.check("T24", "agent abstains ('insufficient evidence') when no context passage states the asked fact",
            z.abstained && z.score == 1.0);
  }

  // ---------- dataset ----------
  {
    LoadReport rep;
    std::vector<Scenario> sc = loadDataset(dataDir + "/scenarios.txt", dataDir + "/passages.txt", rep);
    ValidationReport v = validateDataset(sc);
    t.check("T25", "evaluation set: >= 24 scenarios (>= 8 per category), >= 1000 passages, labels consistent",
            rep.errors.empty() && v.scenarios >= 24 && v.policy >= 8 && v.multisource >= 8 && v.memory >= 8 &&
                v.passages >= 1000 && v.problems.empty(),
            std::to_string(v.scenarios) + " scenarios, " + std::to_string(v.passages) + " passages, " +
                std::to_string(v.problems.size()) + " label problems");
    int viol = 0, runs = 0;
    for (const Scenario& s : sc)
      for (int ctx : {256, 512, 1024})
        for (Policy p : kAll) {
          ++runs;
          viol += measureSelection(s, selectContext(s.question, s.candidates, ctx, p, prm)).violation ? 1 : 0;
        }
    t.check("T26", "zero budget violations on the evaluation set (32 queries x 3 budgets x 4 policies)", viol == 0,
            std::to_string(runs) + " runs");
    generateDataset(dataDir + "/.check_s.txt", dataDir + "/.check_p.txt", 2026);
    std::string a1, a2, b1, b2;
    bool same = readFile(dataDir + "/.check_s.txt", a1) && readFile(dataDir + "/scenarios.txt", a2) &&
                readFile(dataDir + "/.check_p.txt", b1) && readFile(dataDir + "/passages.txt", b2) && a1 == a2 && b1 == b2;
    std::remove((dataDir + "/.check_s.txt").c_str());
    std::remove((dataDir + "/.check_p.txt").c_str());
    t.check("T27", "dataset regenerates byte-for-byte from seed 2026", same);
  }
  {
    std::vector<Passage> w1 = makeScalingWorkload(1000, 20261000), w2 = makeScalingWorkload(1000, 20261000);
    bool same = w1.size() == 1000;
    for (std::size_t i = 0; i < w1.size() && same; ++i)
      same = w1[i].cost == w2[i].cost && w1[i].relevance == w2[i].relevance && w1[i].dupGroup == w2[i].dupGroup;
    t.check("T28", "scaling workload is reproducible from its seed", same);
  }
  {
    LoadReport rep;
    std::vector<Scenario> sc = loadDataset(dataDir + "/muet/scenarios.txt", dataDir + "/muet/passages.txt", rep);
    ValidationReport v = validateDataset(sc);
    int viol = 0, staleHybrid = 0, runs = 0;
    for (const Scenario& s : sc)
      for (int ctx : {64, 128, 256, 512})
        for (Policy p : kAll) {
          ++runs;
          SelStats m = measureSelection(s, selectContext(s.question, s.candidates, ctx, p, prm));
          viol += m.violation ? 1 : 0;
          if (p == Policy::Hybrid) staleHybrid += m.stale;
        }
    t.check("T29", "MUET document set: 20 questions x 155 passages load, labels consistent, no violations, hybrid never stale",
            rep.errors.empty() && v.scenarios == 20 && v.passages == 3100 && v.problems.empty() && viol == 0 &&
                staleHybrid == 0,
            std::to_string(v.passages) + " passages, " + std::to_string(v.problems.size()) + " label problems, " +
                std::to_string(runs) + " runs, violations=" + std::to_string(viol));
  }

  out << "\n" << t.passed << "/" << (t.passed + t.failed) << " tests passed\n";
  return t.failed == 0 ? 0 : 1;
}
