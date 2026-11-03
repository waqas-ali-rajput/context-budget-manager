#include "experiments.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

#include "agent.h"
#include "dataset.h"
#include "hash_map.h"
#include "loader.h"
#include "merge_sort.h"

namespace {

using Clock = std::chrono::steady_clock;

std::string fmt(double v, int prec) {
  char b[64];
  std::snprintf(b, sizeof b, "%.*f", prec, v);
  return b;
}
std::string lpad(const std::string& s, std::size_t w) { return s.size() >= w ? s : s + std::string(w - s.size(), ' '); }
std::string rpad(const std::string& s, std::size_t w) { return s.size() >= w ? s : std::string(w - s.size(), ' ') + s; }

const Policy kPolicies[4] = {Policy::Recency, Policy::Relevance, Policy::Density, Policy::Hybrid};
const char* shortName(Policy p) {
  switch (p) {
    case Policy::Recency: return "recency";
    case Policy::Relevance: return "relevance";
    case Policy::Density: return "density";
    case Policy::Hybrid: return "hybrid";
  }
  return "?";
}
const int kBudgets[3] = {256, 512, 1024};
// Fixed reserves; B_total is chosen so that B_CTX takes the three required values.
Budget budgetFor(int ctx) { return Budget{ctx + 800, 300, 500}; }

double medianOf(std::vector<double> v, double* lo = nullptr, double* hi = nullptr) {
  if (v.empty()) return 0;
  mergeSort(v, [](double a, double b) { return a < b; });
  if (lo) *lo = v.front();
  if (hi) *hi = v.back();
  const std::size_t n = v.size();
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

struct Timing {
  double median = 0, lo = 0, hi = 0;
};

// One warm-up run, then `reps` timed runs (selectContext times itself with steady_clock).
Timing timeSelection(const Scenario& s, int ctx, Policy p, const Params& prm, int reps, Selection& last) {
  last = selectContext(s.question, s.candidates, ctx, p, prm);
  std::vector<double> t;
  for (int r = 0; r < reps; ++r) {
    last = selectContext(s.question, s.candidates, ctx, p, prm);
    t.push_back(last.metrics.selectMicros);
  }
  Timing out;
  out.median = medianOf(t, &out.lo, &out.hi);
  return out;
}

int categoryIndex(const std::string& c) { return c == "policy" ? 0 : c == "multisource" ? 1 : c == "memory" ? 2 : 3; }
const char* kCategories[4] = {"policy", "multisource", "memory", "unanswerable"};

bool hasTag(const Passage& p, const std::string& tag) {
  for (const std::string& t : p.tags)
    if (t == tag) return true;
  return false;
}

}  // namespace

SelStats measureSelection(const Scenario& s, const Selection& sel) {
  SelStats st;
  const std::vector<Passage>& c = s.candidates;
  HashMap groups(16), sources(16);
  std::vector<char> chosen(c.size(), 0);
  for (const Chosen& ch : sel.ordered) {
    const Passage& p = c[ch.index];
    chosen[ch.index] = 1;
    st.tokens += p.cost;
    ++st.selected;
    st.relevanceSum += p.relevance;
    groups.insert(p.dupGroup, 1);
    sources.insert(p.sourceId, 1);
    // Stale = superseded by a candidate, or a near-duplicate copy of a superseded passage.
    bool stale = false;
    for (const Passage& o : c) stale = stale || o.supersedes == p.passageId;
    if (!stale && p.supersedes.empty())
      for (const Passage& k : c)
        if (k.passageId != p.passageId && k.dupGroup == p.dupGroup)
          for (const Passage& o : c) stale = stale || o.supersedes == k.passageId;
    if (stale) ++st.stale;
  }
  st.redundancy = st.selected - static_cast<int>(groups.size());
  st.sources = static_cast<int>(sources.size());
  bool mandatoryMissing = false;
  if (sel.status == Status::Ok)
    for (std::size_t i = 0; i < c.size(); ++i) mandatoryMissing = mandatoryMissing || (c[i].mandatory && !chosen[i]);
  st.violation = st.tokens > sel.budgetCtx || sel.status == Status::BudgetViolation || mandatoryMissing ||
                 (sel.status == Status::Infeasible && st.selected > 0);
  st.goldTotal = static_cast<int>(s.gold.size());
  for (const std::string& g : s.gold) {
    bool hit = false;
    for (const Chosen& ch : sel.ordered) hit = hit || hasTag(c[ch.index], g);
    if (hit) ++st.goldHit;
  }
  st.coverage = st.goldTotal ? static_cast<double>(st.goldHit) / st.goldTotal : -1.0;
  return st;
}

void printSelection(std::ostream& o, const Scenario& s, const Selection& sel, bool showOmitted) {
  const std::vector<Passage>& c = s.candidates;
  o << "[" << s.queryId << "] policy=" << policyName(sel.policy) << "  status=" << statusName(sel.status)
    << "  B_CTX=" << sel.budgetCtx << "  used=" << sel.tokenTotal << " ("
    << fmt(sel.budgetCtx ? 100.0 * sel.tokenTotal / sel.budgetCtx : 0.0, 1) << "%)  unspent="
    << sel.budgetCtx - sel.tokenTotal << "  candidates=" << c.size() << "  selected=" << sel.ordered.size() << "\n";
  if (!sel.diagnostic.empty()) o << "  " << sel.diagnostic << "\n";
  int pos = 0;
  for (const Chosen& ch : sel.ordered) {
    const Passage& p = c[ch.index];
    std::string tags;
    for (const std::string& t : p.tags) tags += (tags.empty() ? "" : ";") + t;
    o << "  " << rpad(std::to_string(++pos), 2) << ". " << lpad(p.passageId, 9) << " " << lpad(p.sourceId, 15) << " "
      << formatTimestamp(p.timestamp) << " rel=" << fmt(p.relevance, 2) << " c=" << rpad(std::to_string(p.cost), 3)
      << " key=" << fmt(ch.priority, 4) << (ch.mandatory ? " MANDATORY" : "") << "  " << tags << "\n";
  }
  if (showOmitted) {
    for (const Omission& om : sel.omitted) {
      const Passage& p = c[om.index];
      o << "  omitted " << lpad(p.passageId, 9) << " c=" << rpad(std::to_string(p.cost), 3) << "  "
        << omitName(om.reason);
      if (om.supersededBy >= 0) o << " (" << c[om.supersededBy].passageId << ")";
      o << "\n";
    }
  }
}

int runEval(const std::vector<Scenario>& sc, const std::string& dir) {
  std::ofstream csv(dir + "/eval_runs.csv"), log(dir + "/run_log.jsonl"), sum(dir + "/eval_summary.txt");
  csv << "query,category,b_ctx,policy,status,selected,tokens,pct_used,unspent,violation,gold_hit,gold_total,"
         "coverage,redundancy,sources,stale_selected,median_us,min_us,max_us,comparisons,heap_pops,reinserts,"
         "aux_bytes\n";
  const int nq = static_cast<int>(sc.size());
  const Params base;
  // sel[b][q][p] keeps every selection for the follow-up analyses.
  std::vector<std::vector<std::vector<Selection>>> sel(3, std::vector<std::vector<Selection>>(nq, std::vector<Selection>(4)));
  std::vector<std::vector<std::vector<SelStats>>> st(3, std::vector<std::vector<SelStats>>(nq, std::vector<SelStats>(4)));
  std::vector<std::vector<std::vector<double>>> medians(3, std::vector<std::vector<double>>(4));

  for (int b = 0; b < 3; ++b) {
    const int ctx = budgetFor(kBudgets[b]).context();
    for (int q = 0; q < nq; ++q) {
      for (int pi = 0; pi < 4; ++pi) {
        Timing t = timeSelection(sc[q], ctx, kPolicies[pi], base, 5, sel[b][q][pi]);
        const Selection& x = sel[b][q][pi];
        SelStats m = measureSelection(sc[q], x);
        st[b][q][pi] = m;
        medians[b][pi].push_back(t.median);
        csv << sc[q].queryId << ',' << sc[q].category << ',' << ctx << ',' << shortName(kPolicies[pi]) << ','
            << statusName(x.status) << ',' << m.selected << ',' << m.tokens << ',' << fmt(100.0 * m.tokens / ctx, 1) << ','
            << ctx - m.tokens << ',' << (m.violation ? 1 : 0) << ',' << m.goldHit << ',' << m.goldTotal << ','
            << (m.coverage < 0 ? std::string("NA") : fmt(m.coverage, 3)) << ',' << m.redundancy << ',' << m.sources
            << ',' << m.stale << ',' << fmt(t.median, 2) << ',' << fmt(t.lo, 2) << ',' << fmt(t.hi, 2) << ','
            << x.metrics.comparisons << ',' << x.metrics.heapPops << ',' << x.metrics.reinserts << ','
            << x.metrics.auxBytes << '\n';
        log << "{\"query\":\"" << sc[q].queryId << "\",\"policy\":\"" << policyName(kPolicies[pi])
            << "\",\"params\":{\"alpha\":" << base.alpha << ",\"lambda\":" << base.lambda
            << ",\"dup_penalty\":" << base.dupPenalty << ",\"exclude_stale\":" << (base.excludeStale ? "true" : "false")
            << ",\"order\":\"priority\"},\"b_total\":" << budgetFor(kBudgets[b]).total
            << ",\"r_fixed\":300,\"r_answer\":500,\"b_ctx\":" << ctx << ",\"candidates\":" << sc[q].candidates.size()
            << ",\"dataset_seed\":2026,\"reps\":5,\"median_us\":" << fmt(t.median, 2)
            << ",\"comparisons\":" << x.metrics.comparisons << ",\"heap_pops\":" << x.metrics.heapPops
            << ",\"reinserts\":" << x.metrics.reinserts << ",\"aux_bytes\":" << x.metrics.auxBytes << ",\"status\":\""
            << statusName(x.status) << "\",\"tokens\":" << m.tokens << ",\"selected_ids\":[";
        for (std::size_t k = 0; k < x.ordered.size(); ++k)
          log << (k ? "," : "") << '"' << sc[q].candidates[x.ordered[k].index].passageId << '"';
        log << "]}\n";
      }
    }
  }

  // ---- 1. Main comparison table ----
  sum << "CONTEXT BUDGET MANAGER - EVALUATION SUMMARY\n";
  sum << "Dataset: " << nq << " scenarios, ";
  int totalPassages = 0, answerable = 0;
  for (const Scenario& s : sc) {
    totalPassages += static_cast<int>(s.candidates.size());
    answerable += s.gold.empty() ? 0 : 1;
  }
  sum << totalPassages << " candidate passages, " << answerable << " answerable. Budgets: B_total = B_CTX + 800, "
      << "R_fixed = 300, R_answer = 500. Timing: 1 warm-up + 5 timed runs per configuration.\n\n";
  sum << "1. MAIN COMPARISON (coverage averaged over answerable queries; INFEASIBLE runs count as 0 coverage)\n";
  sum << "B_CTX policy     infeas viol  mean_tok  %B_CTX  coverage  redund  sources  stale(sel/queries)  "
         "median_us [min-max of medians]  mean_cmps  mean_aux_B\n";
  for (int b = 0; b < 3; ++b) {
    for (int pi = 0; pi < 4; ++pi) {
      int infeas = 0, viol = 0, ok = 0, red = 0, stale = 0, staleQ = 0, covN = 0;
      double tok = 0, pct = 0, cov = 0, srcs = 0, cmps = 0, aux = 0;
      for (int q = 0; q < nq; ++q) {
        const SelStats& m = st[b][q][pi];
        const Selection& x = sel[b][q][pi];
        if (x.status == Status::Infeasible) ++infeas;
        if (m.violation) ++viol;
        if (x.status == Status::Ok) {
          ++ok;
          tok += m.tokens;
          pct += 100.0 * m.tokens / x.budgetCtx;
          srcs += m.sources;
        }
        red += m.redundancy;
        stale += m.stale;
        staleQ += m.stale > 0 ? 1 : 0;
        if (m.coverage >= 0) {
          cov += m.coverage;
          ++covN;
        }
        cmps += static_cast<double>(x.metrics.comparisons);
        aux += static_cast<double>(x.metrics.auxBytes);
      }
      double lo = 0, hi = 0;
      const double med = medianOf(medians[b][pi], &lo, &hi);
      sum << rpad(std::to_string(kBudgets[b]), 5) << " " << lpad(shortName(kPolicies[pi]), 10) << " "
          << rpad(std::to_string(infeas), 6) << " " << rpad(std::to_string(viol), 4) << " " << rpad(fmt(tok / ok, 1), 9)
          << " " << rpad(fmt(pct / ok, 1), 7) << " " << rpad(fmt(cov / covN, 3), 9) << " " << rpad(std::to_string(red), 7)
          << " " << rpad(fmt(srcs / ok, 2), 8) << " " << rpad(std::to_string(stale) + "/" + std::to_string(staleQ), 19)
          << " " << rpad(fmt(med, 2), 9) << " [" << fmt(lo, 2) << "-" << fmt(hi, 2) << "]"
          << rpad(fmt(cmps / nq, 0), 12) << rpad(fmt(aux / nq, 0), 12) << "\n";
    }
  }

  // ---- 2. Coverage and staleness by category ----
  sum << "\n2. COVERAGE / STALE SELECTIONS / REDUNDANCY BY CATEGORY (mean coverage; stale = superseded passages "
         "selected; redund = selected beyond the first per duplicate group)\n";
  sum << "B_CTX category     | recency            | relevance          | density            | hybrid\n";
  for (int b = 0; b < 3; ++b) {
    for (int cat = 0; cat < 4; ++cat) {
      sum << rpad(std::to_string(kBudgets[b]), 5) << " " << lpad(kCategories[cat], 12) << " |";
      for (int pi = 0; pi < 4; ++pi) {
        double cov = 0;
        int n = 0, stale = 0, red = 0;
        for (int q = 0; q < nq; ++q) {
          if (categoryIndex(sc[q].category) != cat) continue;
          const SelStats& m = st[b][q][pi];
          if (m.coverage >= 0) {
            cov += m.coverage;
            ++n;
          }
          stale += m.stale;
          red += m.redundancy;
        }
        sum << " cov=" << (n ? fmt(cov / n, 3) : std::string("  NA ")) << " st=" << rpad(std::to_string(stale), 2)
            << " rd=" << rpad(std::to_string(red), 2) << " |";
      }
      sum << "\n";
    }
  }

  // ---- 3. Long passage analysis (multisource) ----
  sum << "\n3. LONG HIGHLY RELEVANT PASSAGE (multisource queries, cost > 250, >= 3 gold facts): times selected "
         "/ mean coverage\n";
  for (int b = 0; b < 3; ++b) {
    sum << "B_CTX " << kBudgets[b] << ":";
    for (int pi = 0; pi < 4; ++pi) {
      int taken = 0, n = 0;
      double cov = 0;
      for (int q = 0; q < nq; ++q) {
        if (sc[q].category != "multisource") continue;
        ++n;
        cov += st[b][q][pi].coverage;
        for (const Chosen& ch : sel[b][q][pi].ordered) {
          const Passage& p = sc[q].candidates[ch.index];
          if (p.cost > 250 && p.tags.size() >= 3) ++taken;
        }
      }
      sum << "  " << shortName(kPolicies[pi]) << " " << taken << "/" << n << " cov=" << fmt(cov / n, 3);
    }
    sum << "\n";
  }

  // ---- 4. Counterexamples: density keeps more total relevance but less gold evidence ----
  sum << "\n4. GREEDY RELEVANCE-PER-TOKEN COUNTEREXAMPLES (density has >= total relevance of the other policy "
         "but lower gold coverage)\n";
  int found = 0, printed = 0;
  for (int b = 0; b < 3; ++b)
    for (int q = 0; q < nq; ++q) {
      const SelStats& d = st[b][q][2];
      if (d.coverage < 0) continue;
      for (int other : {1, 3}) {
        const SelStats& o = st[b][q][other];
        if (d.relevanceSum >= o.relevanceSum && d.coverage < o.coverage) {
          ++found;
          if (printed < 2) {
            ++printed;
            sum << "-- " << sc[q].queryId << " (" << sc[q].category << ") B_CTX=" << kBudgets[b] << ": density sum_rel="
                << fmt(d.relevanceSum, 3) << " coverage=" << d.goldHit << "/" << d.goldTotal << " vs "
                << shortName(kPolicies[other]) << " sum_rel=" << fmt(o.relevanceSum, 3) << " coverage=" << o.goldHit
                << "/" << o.goldTotal << "\n   gold: ";
            for (const std::string& g : sc[q].gold) sum << g << "  ";
            sum << "\n";
            std::ostringstream a, bb;
            printSelection(a, sc[q], sel[b][q][2], false);
            printSelection(bb, sc[q], sel[b][q][other], false);
            sum << a.str() << bb.str();
          }
        }
      }
    }
  sum << "Total counterexample cases found: " << found << "\n";

  // ---- 5. Answer-quality proxy ----
  std::ofstream aq(dir + "/answer_quality.txt");
  FirstMentionAgent fm;
  AuthorityAgent au;
  const QaAgent* agents[2] = {&fm, &au};
  aq << "ANSWER-QUALITY PROXY (deterministic extractive agents; NOT observed LLM accuracy)\n"
     << "Score per question = checklist facts answered with the gold value / facts asked. "
     << "contra = facts answered with a non-gold (stale or wrong) value.\n\n";
  aq << "A. 12 fixed questions at B_CTX = 512, packaging order = priority\n";
  aq << "query category     agent         | recency      | relevance    | density      | hybrid\n";
  const char* fixed[12] = {"Q01", "Q02", "Q03", "Q04", "Q11", "Q12", "Q13", "Q14", "Q21", "Q22", "Q23", "Q24"};
  int fixedTot[2][4][3] = {};
  for (const char* id : fixed) {
    int q = -1;
    for (int k = 0; k < nq; ++k)
      if (sc[k].queryId == id) q = k;
    if (q < 0) continue;
    for (int a = 0; a < 2; ++a) {
      aq << lpad(id, 5) << " " << lpad(sc[q].category, 12) << " " << lpad(agents[a]->name(), 13) << " |";
      for (int pi = 0; pi < 4; ++pi) {
        ContextPackage pkg = buildPackage(sc[q], sel[1][q][pi], OrderRule::Priority);
        AnswerScore r = scoreAnswer(agents[a]->answer(pkg, sc[q].askKeys), sc[q]);
        fixedTot[a][pi][0] += r.correct;
        fixedTot[a][pi][1] += r.contradictory;
        fixedTot[a][pi][2] += r.asked;
        aq << " " << r.correct << "/" << r.asked << " contra=" << r.contradictory << "  |";
      }
      aq << "\n";
    }
  }
  for (int a = 0; a < 2; ++a) {
    aq << "TOTAL " << lpad(agents[a]->name(), 26) << " |";
    for (int pi = 0; pi < 4; ++pi)
      aq << " " << fixedTot[a][pi][0] << "/" << fixedTot[a][pi][2] << " contra=" << fixedTot[a][pi][1] << " |";
    aq << "\n";
  }
  aq << "\nB. All 30 answerable questions (facts correct / asked, contradictory claims, conflicts flagged) and "
        "abstention on the 2 no-gold questions\n";
  for (int b = 0; b < 3; ++b)
    for (int a = 0; a < 2; ++a) {
      aq << "B_CTX " << rpad(std::to_string(kBudgets[b]), 4) << " " << lpad(agents[a]->name(), 13) << " |";
      for (int pi = 0; pi < 4; ++pi) {
        int c = 0, asked = 0, contra = 0, conf = 0, abst = 0, weak = 0;
        for (int q = 0; q < nq; ++q) {
          ContextPackage pkg = buildPackage(sc[q], sel[b][q][pi], OrderRule::Priority);
          AnswerScore r = scoreAnswer(agents[a]->answer(pkg, sc[q].askKeys), sc[q]);
          if (r.answerable) {
            c += r.correct;
            asked += r.asked;
            contra += r.contradictory;
            conf += r.conflictsFlagged;
            weak += r.weakOnly;
          } else if (r.score == 1.0) {
            ++abst;
          }
        }
        aq << " " << shortName(kPolicies[pi]) << " " << c << "/" << asked << " contra=" << contra << " conflicts=" << conf
           << " weakC=" << weak << " abstain=" << abst << "/2 |";
      }
      aq << "\n";
    }
  aq << "\nC. Example answers, Q01 at B_CTX = 512 (first-mention agent)\n";
  for (int pi = 0; pi < 4; ++pi) {
    ContextPackage pkg = buildPackage(sc[0], sel[1][0][pi], OrderRule::Priority);
    aq << lpad(shortName(kPolicies[pi]), 10) << ": " << fm.answer(pkg, sc[0].askKeys).text << "\n";
  }

  // ---- 6. Ordering experiment: same selections, different packaging order ----
  std::ofstream oe(dir + "/order_experiment.txt");
  oe << "ORDERING EXPERIMENT: selection fixed, only the packaging order changes. 30 answerable queries.\n"
     << "Cells: facts correct / asked, contradictory claims.\n";
  const OrderRule orders[3] = {OrderRule::Priority, OrderRule::Chronological, OrderRule::ReverseChronological};
  const char* orderShort[3] = {"priority", "oldest-first", "newest-first"};
  for (int b = 1; b < 3; ++b)
    for (int pi = 0; pi < 4; ++pi)
      for (int a = 0; a < 2; ++a) {
        oe << "B_CTX " << rpad(std::to_string(kBudgets[b]), 4) << " " << lpad(shortName(kPolicies[pi]), 10) << " "
           << lpad(agents[a]->name(), 13) << " |";
        for (int o = 0; o < 3; ++o) {
          Params prm;
          prm.order = orders[o];
          int c = 0, asked = 0, contra = 0;
          for (int q = 0; q < nq; ++q) {
            if (sc[q].gold.empty()) continue;
            Selection x = selectContext(sc[q].question, sc[q].candidates, budgetFor(kBudgets[b]).context(), kPolicies[pi], prm);
            AnswerScore r = scoreAnswer(agents[a]->answer(buildPackage(sc[q], x, orders[o]), sc[q].askKeys), sc[q]);
            c += r.correct;
            asked += r.asked;
            contra += r.contradictory;
          }
          oe << " " << orderShort[o] << " " << c << "/" << asked << " contra=" << contra << " |";
        }
        oe << "\n";
      }

  // ---- 7. Sensitivity of the hybrid policy ----
  std::ofstream se(dir + "/sensitivity.txt");
  se << "HYBRID SENSITIVITY (all 32 queries; coverage over 30 answerable; FM = first-mention agent facts correct)\n";
  struct Variant {
    const char* name;
    Params p;
  };
  std::vector<Variant> vars;
  vars.push_back({"default a=0.5 lambda=0.2 dup=0.3 exclude-stale", Params()});
  Params v;
  v.alpha = 0.0;
  vars.push_back({"alpha=0 (no cost normalisation)", v});
  v = Params();
  v.alpha = 1.0;
  vars.push_back({"alpha=1 (full per-token density)", v});
  v = Params();
  v.excludeStale = false;
  vars.push_back({"stale penalty x0.25 instead of exclusion", v});
  v = Params();
  v.dropRedundant = false;
  vars.push_back({"keep redundant copies (dropRedundant=false)", v});
  v = Params();
  v.dupPenalty = 1.0;
  v.lambda = 1.0;
  v.dropRedundant = false;
  vars.push_back({"no coverage/redundancy terms (lambda=1, dup=1)", v});
  for (const Variant& var : vars) {
    se << var.name << "\n";
    for (int b = 0; b < 3; ++b) {
      double cov = 0, covMulti = 0, pct = 0;
      int n = 0, nm = 0, stale = 0, red = 0, ok = 0, fmc = 0, asked = 0, longTaken = 0;
      for (int q = 0; q < nq; ++q) {
        Selection x = selectContext(sc[q].question, sc[q].candidates, budgetFor(kBudgets[b]).context(), Policy::Hybrid, var.p);
        SelStats m = measureSelection(sc[q], x);
        if (m.coverage >= 0) {
          cov += m.coverage;
          ++n;
        }
        if (sc[q].category == "multisource") {
          covMulti += m.coverage;
          ++nm;
          for (const Chosen& ch : x.ordered)
            if (sc[q].candidates[ch.index].cost > 250 && sc[q].candidates[ch.index].tags.size() >= 3) ++longTaken;
        }
        stale += m.stale;
        red += m.redundancy;
        if (x.status == Status::Ok) {
          ++ok;
          pct += 100.0 * m.tokens / x.budgetCtx;
        }
        AnswerScore r = scoreAnswer(fm.answer(buildPackage(sc[q], x, OrderRule::Priority), sc[q].askKeys), sc[q]);
        if (r.answerable) {
          fmc += r.correct;
          asked += r.asked;
        }
      }
      se << "  B_CTX " << rpad(std::to_string(kBudgets[b]), 4) << ": coverage=" << fmt(cov / n, 3)
         << " multisource=" << fmt(covMulti / nm, 3) << " long_taken=" << longTaken << "/" << nm << " stale=" << stale
         << " redundancy=" << red << " %B_CTX=" << fmt(pct / ok, 1) << " FM=" << fmc << "/" << asked << "\n";
    }
  }

  // ---- 8. Sample context package handed to the agent ----
  std::ofstream sp(dir + "/sample_package.txt");
  for (int pi : {1, 3}) {
    ContextPackage pkg = buildPackage(sc[0], sel[1][0][pi], OrderRule::Priority);
    sp << renderPrompt(pkg) << "Agent (first-mention): " << fm.answer(pkg, sc[0].askKeys).text << "\n\n";
    printSelection(sp, sc[0], sel[1][0][pi], true);
    sp << "\n";
  }
  std::cout << "eval: wrote eval_runs.csv, eval_summary.txt, answer_quality.txt, order_experiment.txt, "
               "sensitivity.txt, sample_package.txt, run_log.jsonl to "
            << dir << "\n";
  return 0;
}

int runBench(const std::string& dir, int reps) {
  std::ofstream csv(dir + "/bench_scaling.csv"), txt(dir + "/bench_summary.txt");
  csv << "n,b_ctx,policy,engine,inner_calls,reps,median_us,min_us,max_us,selected_k,tokens,comparisons,heap_pops,"
         "reinserts,aux_bytes,same_as_sort\n";
  txt << "SCALING BENCHMARK (synthetic workloads, seed = 20260000 + n). Per configuration: 1 warm-up batch + " << reps
      << " timed batches; each batch runs the selection `inner` times and the per-call time is batch/inner "
         "(steady_clock).\n";
  txt << "     n  B_CTX  policy     engine      median_us   [min - max]          prep_us     k   comparisons  pops   reins  aux_KB  same\n";
  struct Cfg {
    Policy p;
    Engine e;
  };
  const Cfg cfgs[7] = {{Policy::Recency, Engine::Sort},   {Policy::Recency, Engine::Heap},
                       {Policy::Relevance, Engine::Sort}, {Policy::Relevance, Engine::Heap},
                       {Policy::Density, Engine::Sort},   {Policy::Density, Engine::Heap},
                       {Policy::Hybrid, Engine::Heap}};
  for (int n : {100, 1000, 10000, 100000}) {
    std::vector<Passage> w = makeScalingWorkload(n, 20260000ULL + static_cast<unsigned>(n));
    std::vector<int> budgets = {1024};
    if (n == 10000) budgets = {256, 1024, 4096, 16384, 65536};
    for (int ctx : budgets) {
      std::vector<int> sortOrder[3];
      for (const Cfg& cfg : cfgs) {
        Params prm;
        prm.engine = cfg.e;
        const int inner = n >= 200000 ? 1 : 200000 / n;
        Selection last;
        double prepSum = 0;
        auto batch = [&]() {
          prepSum = 0;
          const Clock::time_point t0 = Clock::now();
          for (int k = 0; k < inner; ++k) {
            last = selectContext("bench", w, ctx, cfg.p, prm);
            prepSum += last.metrics.prepMicros;
          }
          return std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / inner;
        };
        batch();  // warm-up
        std::vector<double> t, prep;
        for (int r = 0; r < reps; ++r) {
          t.push_back(batch());
          prep.push_back(prepSum / inner);
        }
        double lo = 0, hi = 0;
        const double med = medianOf(t, &lo, &hi);
        const double prepMed = medianOf(prep);
        std::vector<int> ids;
        for (const Chosen& ch : last.ordered) ids.push_back(ch.index);
        std::string same = "-";
        if (cfg.p != Policy::Hybrid) {
          const int slot = static_cast<int>(cfg.p);
          if (cfg.e == Engine::Sort) sortOrder[slot] = ids;
          else same = ids == sortOrder[slot] ? "yes" : "NO";
        }
        csv << n << ',' << ctx << ',' << shortName(cfg.p) << ',' << engineName(cfg.e) << ',' << inner << ',' << reps
            << ',' << fmt(med, 3) << ',' << fmt(lo, 3) << ',' << fmt(hi, 3) << ',' << fmt(prepMed, 3) << ','
            << last.ordered.size() << ','
            << last.tokenTotal << ',' << last.metrics.comparisons << ',' << last.metrics.heapPops << ','
            << last.metrics.reinserts << ',' << last.metrics.auxBytes << ',' << same << '\n';
        txt << rpad(std::to_string(n), 6) << " " << rpad(std::to_string(ctx), 6) << "  " << lpad(shortName(cfg.p), 10)
            << " " << lpad(engineName(cfg.e), 10) << " " << rpad(fmt(med, 2), 10) << "   [" << fmt(lo, 2) << " - "
            << fmt(hi, 2) << "]" << rpad(fmt(prepMed, 2), 11) << rpad(std::to_string(last.ordered.size()), 6)
            << rpad(std::to_string(last.metrics.comparisons), 13) << rpad(std::to_string(last.metrics.heapPops), 7)
            << rpad(std::to_string(last.metrics.reinserts), 7) << rpad(fmt(last.metrics.auxBytes / 1024.0, 1), 9)
            << "  " << same << "\n";
        std::cout << "bench n=" << n << " B=" << ctx << " " << shortName(cfg.p) << "/" << engineName(cfg.e) << " "
                  << fmt(med, 2) << " us\n";
      }
    }
  }
  return 0;
}

int runEdge(const std::string& dataDir, const std::string& dir) {
  std::ofstream o(dir + "/edge_cases.txt");
  LoadReport rep;
  std::vector<Scenario> sc =
      loadDataset(dataDir + "/fixtures/edge_scenarios.txt", dataDir + "/fixtures/edge_passages.txt", rep);
  o << "EDGE AND STRESS CASES (fixtures in data/fixtures). Every case is run under all four policies.\n";
  o << "Fixture load: " << rep.accepted << " passages accepted, " << rep.errors.size() << " errors\n\n";
  struct Case {
    const char* id;
    Budget budget;
  };
  const Case cases[] = {{"E01", {1312, 300, 500}}, {"E02", {700, 300, 500}},  {"E03", {1056, 300, 500}},
                        {"E04", {1056, 300, 500}}, {"E05", {1056, 300, 500}}, {"E06", {920, 300, 500}},
                        {"E07", {960, 300, 500}},  {"E08", {1000, 300, 500}}, {"E09", {1000, 300, 500}},
                        {"E10", {1056, 300, 500}}, {"E12", {800, 300, 500}},  {"E13", {900, 300, 500}}};
  for (const Case& cs : cases) {
    for (const Scenario& s : sc) {
      if (s.queryId != cs.id) continue;
      o << "==== " << s.queryId << ": " << s.question << "   (B_total=" << cs.budget.total
        << ", R_fixed=" << cs.budget.reserveFixed << ", R_answer=" << cs.budget.reserveAnswer
        << " -> B_CTX=" << cs.budget.context() << ")\n";
      for (Policy p : kPolicies) {
        Selection x = selectContext(s.question, s.candidates, cs.budget.context(), p, Params());
        SelStats m = measureSelection(s, x);
        printSelection(o, s, x, true);
        o << "  -> coverage=" << (m.coverage < 0 ? std::string("NA") : std::to_string(m.goldHit) + "/" + std::to_string(m.goldTotal))
          << " stale=" << m.stale << " redundancy=" << m.redundancy << " violation=" << (m.violation ? "YES" : "no") << "\n";
      }
      o << "\n";
    }
  }
  for (const Scenario& s : sc) {
    if (s.queryId != "E11") continue;
    o << "==== E11: " << s.question << " (identical input at B_CTX = 256 / 512 / 1024)\n";
    for (int ctx : kBudgets)
      for (Policy p : kPolicies) {
        Selection x = selectContext(s.question, s.candidates, ctx, p, Params());
        SelStats m = measureSelection(s, x);
        o << "  B_CTX=" << rpad(std::to_string(ctx), 4) << " " << lpad(shortName(p), 10) << " selected=" << m.selected
          << " tokens=" << m.tokens << " coverage=" << m.goldHit << "/" << m.goldTotal
          << " violation=" << (m.violation ? "YES" : "no") << "\n";
      }
    o << "\n";
  }
  LoadReport bad;
  std::vector<Passage> ok = loadPassages(dataDir + "/fixtures/malformed_passages.txt", bad);
  o << "==== Malformed metadata and duplicate ids (data/fixtures/malformed_passages.txt)\n";
  o << "accepted " << bad.accepted << " record(s):";
  for (const Passage& p : ok) o << " " << p.passageId;
  o << "\nrejected " << bad.errors.size() << " record(s):\n";
  for (const LoadError& e : bad.errors) o << "  line " << rpad(std::to_string(e.line), 2) << ": " << e.message << "\n";
  std::cout << "edge: wrote " << dir << "/edge_cases.txt\n";
  return 0;
}

int runMuet(const std::vector<Scenario>& sc, const std::string& dir) {
  std::ofstream out(dir + "/muet_summary.txt");
  const int budgets[4] = {64, 128, 256, 512};
  const int nq = static_cast<int>(sc.size());
  const Params base;
  FirstMentionAgent fm;
  AuthorityAgent au;
  out << "MUET DOCUMENT SET - EVALUATION (cep/data/muet, from the PBL corpus data/passages.txt)\n"
      << "20 questions, every question has all 155 MUET passages as candidates (3,100 records). Costs are estimates\n"
      << "(ceil(1.3 x words) + 6). B_total = B_CTX + 800, R_fixed = 300, R_answer = 500. Timing: 1 warm-up + 5 runs.\n"
      << "Coverage is averaged over the 17 answerable questions. FM / AU = first-mention / authority agent:\n"
      << "facts correct of facts asked, contra = facts answered with a non-gold (stale or wrong) value.\n\n";
  out << "1. MAIN COMPARISON\n"
      << "B_CTX policy     viol  mean_tok  %B_CTX  coverage  redund  stale(sel/q)  untrusted  median_us   FM correct  "
         "contra   AU correct  contra  abstain\n";
  int totalViol = 0;
  for (int ctx : budgets) {
    const int c = budgetFor(ctx).context();
    for (Policy p : kPolicies) {
      int viol = 0, redund = 0, staleSel = 0, staleQ = 0, untrusted = 0, answerable = 0, abstain = 0;
      int fmOk = 0, fmBad = 0, auOk = 0, auBad = 0, asked = 0;
      double tok = 0, cov = 0;
      std::vector<double> times;
      for (int q = 0; q < nq; ++q) {
        Selection x;
        Timing t = timeSelection(sc[q], c, p, base, 5, x);
        times.push_back(t.median);
        SelStats m = measureSelection(sc[q], x);
        viol += m.violation ? 1 : 0;
        tok += m.tokens;
        redund += m.redundancy;
        staleSel += m.stale;
        staleQ += m.stale > 0 ? 1 : 0;
        for (const Chosen& ch : x.ordered) untrusted += sc[q].candidates[ch.index].reliability == 'C' ? 1 : 0;
        ContextPackage pkg = buildPackage(sc[q], x, base.order);
        AnswerScore a = scoreAnswer(fm.answer(pkg, sc[q].askKeys), sc[q]);
        AnswerScore b = scoreAnswer(au.answer(pkg, sc[q].askKeys), sc[q]);
        if (m.coverage >= 0) {
          ++answerable;
          cov += m.coverage;
          fmOk += a.correct;
          fmBad += a.contradictory;
          auOk += b.correct;
          auBad += b.contradictory;
          asked += a.asked;
        } else {
          abstain += b.abstained ? 1 : 0;
        }
      }
      totalViol += viol;
      out << rpad(std::to_string(ctx), 5) << " " << lpad(shortName(p), 10) << " " << rpad(std::to_string(viol), 4)
          << " " << rpad(fmt(tok / nq, 1), 9) << " " << rpad(fmt(100.0 * tok / nq / c, 1), 7) << " "
          << rpad(fmt(cov / answerable, 3), 9) << " " << rpad(std::to_string(redund), 7) << " "
          << rpad(std::to_string(staleSel) + "/" + std::to_string(staleQ), 13) << " " << rpad(std::to_string(untrusted), 10)
          << " " << rpad(fmt(medianOf(times), 2), 10) << "   " << rpad(std::to_string(fmOk) + "/" + std::to_string(asked), 10)
          << " " << rpad(std::to_string(fmBad), 6) << "   " << rpad(std::to_string(auOk) + "/" + std::to_string(asked), 10)
          << " " << rpad(std::to_string(auBad), 6) << "  " << abstain << "/" << nq - answerable << "\n";
    }
  }

  out << "\n2. PER QUESTION AT B_CTX = 128: gold facts covered / stale passages selected\n"
      << "query category      | recency  | relevance | density  | hybrid   | question\n";
  for (int q = 0; q < nq; ++q) {
    out << lpad(sc[q].queryId, 5) << " " << lpad(sc[q].category, 12) << " |";
    for (Policy p : kPolicies) {
      SelStats m = measureSelection(sc[q], selectContext(sc[q].question, sc[q].candidates, 128, p, base));
      out << " " << lpad(m.coverage < 0 ? "NA" : std::to_string(m.goldHit) + "/" + std::to_string(m.goldTotal), 4)
          << " st=" << m.stale << " |";
    }
    out << " " << sc[q].question << "\n";
  }

  out << "\n3. VERSION-SENSITIVE QUESTIONS AT B_CTX = 128 (authority agent's answer)\n";
  for (int q = 0; q < nq; ++q) {
    if (sc[q].category != "policy") continue;
    out << sc[q].queryId << " " << sc[q].question << "   gold:";
    for (const std::string& g : sc[q].gold) out << " " << g << ";";
    out << "\n";
    for (Policy p : kPolicies) {
      Selection x = selectContext(sc[q].question, sc[q].candidates, 128, p, base);
      AgentAnswer a = au.answer(buildPackage(sc[q], x, base.order), sc[q].askKeys);
      out << "   " << lpad(shortName(p), 10) << " " << (a.abstained ? "insufficient evidence" : a.text) << "\n";
    }
  }

  for (int q = 0; q < nq; ++q) {
    if (sc[q].queryId != "M09") continue;
    out << "\n4. SAMPLE: M09 at B_CTX = 128, relevance-first vs hybrid\n";
    for (Policy p : {Policy::Relevance, Policy::Hybrid}) {
      Selection x = selectContext(sc[q].question, sc[q].candidates, 128, p, base);
      printSelection(out, sc[q], x, false);
    }
  }
  out << "\nbudget violations in all " << 4 * 4 * nq << " runs: " << totalViol << "\n";
  std::cout << "muet: wrote " << dir << "/muet_summary.txt (" << 4 * 4 * nq << " runs, " << totalViol
            << " budget violations)\n";
  return totalViol == 0 ? 0 : 1;
}
