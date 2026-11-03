// cbm - Context Budget Manager for LLM-powered agents (CS-251 CEP).
// Command dispatch only; all logic lives in the modules.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "agent.h"
#include "dataset.h"
#include "experiments.h"
#include "loader.h"
#include "menu.h"
#include "selector.h"

namespace {

const char* kUsage =
    "usage: cbm <command> [options]   (run from the cep/ folder)\n"
    "  menu                                interactive demo (same as running cbm with no command)\n"
    "  gen [--seed N]                      write data/scenarios.txt and data/passages.txt\n"
    "  validate                            check dataset labels -> results/dataset_validation.txt\n"
    "  select --query Qnn [--policy recency|relevance|density|hybrid] [--budget B_CTX |\n"
    "         --total B --rfixed R --ranswer A] [--order priority|chrono|newest] [--engine sort|heap]\n"
    "  compare --query Qnn [--budget B_CTX]   all four policies side by side\n"
    "  eval                                full evaluation -> results/\n"
    "  bench [--reps N]                    scaling benchmark -> results/bench_*\n"
    "  edge                                edge and stress cases -> results/edge_cases.txt\n"
    "  muet                                Mehran University document set -> results/muet_summary.txt\n"
    "  test                                automated tests -> results/tests.txt\n";

bool parseInt(const std::string& s, int& out) {
  if (s.empty() || s.size() > 9) return false;
  for (char ch : s)
    if (ch < '0' || ch > '9') return false;
  out = std::atoi(s.c_str());
  return true;
}

struct Options {
  std::string query;
  Policy policy = Policy::Hybrid;
  Budget budget{1312, 300, 500};
  Params params;
  int reps = 7;
  long long seed = 2026;
};

bool parseOptions(int argc, char** argv, Options& o) {
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (i + 1 >= argc) return false;
    const std::string v = argv[++i];
    int n = 0;
    if (a == "--query") o.query = v;
    else if (a == "--policy") {
      if (!parsePolicy(v, o.policy)) return false;
    } else if (a == "--order") {
      if (!parseOrder(v, o.params.order)) return false;
    } else if (a == "--engine") {
      if (v != "sort" && v != "heap") return false;
      o.params.engine = v == "sort" ? Engine::Sort : Engine::Heap;
    } else if (a == "--budget" && parseInt(v, n)) o.budget = Budget{n + 800, 300, 500};
    else if (a == "--total" && parseInt(v, n)) o.budget.total = n;
    else if (a == "--rfixed" && parseInt(v, n)) o.budget.reserveFixed = n;
    else if (a == "--ranswer" && parseInt(v, n)) o.budget.reserveAnswer = n;
    else if (a == "--reps" && parseInt(v, n) && n >= 5) o.reps = n;
    else if (a == "--seed" && parseInt(v, n)) o.seed = n;
    else return false;
  }
  return true;
}

std::vector<Scenario> loadOrDie() {
  LoadReport rep;
  std::vector<Scenario> sc = loadDataset("data/scenarios.txt", "data/passages.txt", rep);
  for (const LoadError& e : rep.errors) std::cerr << e.file << ":" << e.line << ": " << e.message << "\n";
  if (sc.empty()) {
    std::cerr << "no scenarios loaded; run 'cbm gen' first\n";
    std::exit(1);
  }
  return sc;
}

const Scenario* findQuery(const std::vector<Scenario>& sc, const std::string& id) {
  for (const Scenario& s : sc)
    if (s.queryId == id) return &s;
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return runMenu(std::cin, std::cout);
  const std::string cmd = argv[1];
  if (cmd == "menu") return runMenu(std::cin, std::cout);
  Options o;
  if (!parseOptions(argc, argv, o)) {
    std::cerr << "invalid option\n" << kUsage;
    return 1;
  }
  if (cmd == "gen") {
    generateDataset("data/scenarios.txt", "data/passages.txt", static_cast<std::uint64_t>(o.seed));
    std::cout << "wrote data/scenarios.txt and data/passages.txt (seed " << o.seed << ")\n";
    return 0;
  }
  if (cmd == "validate") {
    std::vector<Scenario> sc = loadOrDie();
    ValidationReport v = validateDataset(sc);
    std::ofstream out("results/dataset_validation.txt");
    out << "DATASET VALIDATION (independent checks)\n"
        << "scenarios " << v.scenarios << " (policy " << v.policy << ", multisource " << v.multisource << ", memory "
        << v.memory << ", unanswerable " << v.unanswerable << ")\n"
        << "candidate passages " << v.passages << ", tagged passages " << v.taggedPassages << ", gold tags "
        << v.goldTags << ", supersede links " << v.supersedeLinks << ", mandatory passages " << v.mandatoryPassages
        << "\nproblems found: " << v.problems.size() << "\n";
    for (const std::string& p : v.problems) out << "  PROBLEM " << p << "\n";
    for (const std::string& n : v.notes) out << "  note: " << n << "\n";
    std::cout << "validate: " << v.scenarios << " scenarios, " << v.passages << " passages, " << v.problems.size()
              << " problems\n";
    return v.problems.empty() ? 0 : 1;
  }
  if (cmd == "select" || cmd == "compare") {
    std::vector<Scenario> sc = loadOrDie();
    const Scenario* s = findQuery(sc, o.query);
    if (!s) {
      std::cerr << "unknown query '" << o.query << "'\n";
      return 1;
    }
    const int ctx = o.budget.context();
    std::cout << "Question: " << s->question << "\nB_total=" << o.budget.total << " R_fixed=" << o.budget.reserveFixed
              << " R_answer=" << o.budget.reserveAnswer << " -> B_CTX=" << ctx << "\n\n";
    const Policy all[4] = {Policy::Recency, Policy::Relevance, Policy::Density, Policy::Hybrid};
    for (Policy p : all) {
      if (cmd == "select" && p != o.policy) continue;
      Selection sel = selectContext(s->question, s->candidates, ctx, p, o.params);
      printSelection(std::cout, *s, sel, cmd == "select");
      SelStats m = measureSelection(*s, sel);
      ContextPackage pkg = buildPackage(*s, sel, o.params.order);
      std::cout << "  coverage " << m.goldHit << "/" << m.goldTotal << ", stale " << m.stale << ", redundancy "
                << m.redundancy << ", sources " << m.sources << ", " << sel.metrics.selectMicros << " us\n"
                << "  agent (authority): " << AuthorityAgent().answer(pkg, s->askKeys).text << "\n\n";
    }
    return 0;
  }
  if (cmd == "eval") return runEval(loadOrDie(), "results");
  if (cmd == "bench") return runBench("results", o.reps);
  if (cmd == "edge") return runEdge("data", "results");
  if (cmd == "muet") {
    LoadReport rep;
    std::vector<Scenario> sc = loadDataset("data/muet/scenarios.txt", "data/muet/passages.txt", rep);
    for (const LoadError& e : rep.errors) std::cerr << e.file << ":" << e.line << ": " << e.message << "\n";
    if (sc.empty()) {
      std::cerr << "no MUET data; run 'python scripts/muet_dataset.py' first\n";
      return 1;
    }
    ValidationReport v = validateDataset(sc);
    for (const std::string& p : v.problems) std::cerr << "  PROBLEM " << p << "\n";
    std::cout << "muet: " << v.scenarios << " questions, " << v.passages << " candidate records, " << v.problems.size()
              << " label problems\n";
    return v.problems.empty() ? runMuet(sc, "results") : 1;
  }
  if (cmd == "test") {
    std::ofstream out("results/tests.txt");
    int rc = runTests("data", out);
    out.close();
    std::ifstream in("results/tests.txt");
    std::cout << in.rdbuf();
    return rc;
  }
  std::cerr << "unknown command '" << cmd << "'\n" << kUsage;
  return 1;
}
