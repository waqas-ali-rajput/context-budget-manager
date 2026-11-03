#include "menu.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "agent.h"
#include "dataset.h"
#include "experiments.h"
#include "loader.h"
#include "selector.h"

namespace {

struct Dataset {
  const char* name;
  const char* scenarios;
  const char* passages;
};

const int kDatasetCount = 3;
const Dataset kDatasets[kDatasetCount] = {
    {"evaluation set (32 labelled questions)", "data/scenarios.txt", "data/passages.txt"},
    {"edge and stress cases E01-E13", "data/fixtures/edge_scenarios.txt", "data/fixtures/edge_passages.txt"},
    {"MUET documents (20 questions x 155 passages)", "data/muet/scenarios.txt", "data/muet/passages.txt"},
};

const Policy kPolicies[4] = {Policy::Recency, Policy::Relevance, Policy::Density, Policy::Hybrid};
const OrderRule kOrders[3] = {OrderRule::Priority, OrderRule::Chronological, OrderRule::ReverseChronological};

struct State {
  int dataset = 0;
  std::vector<Scenario> scenarios;
  int query = 0;
  Budget budget{1312, 300, 500};
  Policy policy = Policy::Hybrid;
  Params params;
};

std::string trim(const std::string& s) {
  std::string::size_type b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
  return s.substr(b, e - b);
}

// Prompts and reads one line; false at end of input.
bool ask(std::istream& in, std::ostream& out, const std::string& prompt, std::string& line) {
  out << prompt << std::flush;
  if (!std::getline(in, line)) return false;
  line = trim(line);
  return true;
}

// Nonnegative integer up to 9 digits; rejects anything else.
bool toInt(const std::string& s, int& n) {
  if (s.empty() || s.size() > 9) return false;
  n = 0;
  for (char ch : s) {
    if (ch < '0' || ch > '9') return false;
    n = n * 10 + (ch - '0');
  }
  return true;
}

// Asks until the answer is an integer in [lo, hi]; false at end of input.
bool askInt(std::istream& in, std::ostream& out, const std::string& prompt, int lo, int hi, int& n) {
  std::string line;
  while (ask(in, out, prompt, line)) {
    if (toInt(line, n) && n >= lo && n <= hi) return true;
    out << "  please enter a whole number from " << lo << " to " << hi << "\n";
  }
  return false;
}

bool loadData(State& st, int which, std::ostream& out) {
  LoadReport rep;
  std::vector<Scenario> sc = loadDataset(kDatasets[which].scenarios, kDatasets[which].passages, rep);
  for (const LoadError& e : rep.errors) out << "  " << e.file << ":" << e.line << ": " << e.message << "\n";
  if (sc.empty()) {
    out << "  could not load " << kDatasets[which].scenarios << " (run from the cep/ folder)\n";
    return false;
  }
  st.dataset = which;
  st.scenarios = sc;
  st.query = 0;
  return true;
}

void printSettings(const State& st, std::ostream& out) {
  const Scenario& s = st.scenarios[st.query];
  out << "\n  dataset : " << kDatasets[st.dataset].name << "\n"
      << "  question: " << s.queryId << " (" << s.category << ") " << s.question << "\n"
      << "  budget  : B_total " << st.budget.total << " - R_fixed " << st.budget.reserveFixed << " - R_answer "
      << st.budget.reserveAnswer << " = B_CTX " << st.budget.context() << " tokens\n"
      << "  policy  : " << policyName(st.policy) << "   order: " << orderName(st.params.order)
      << "   baseline engine: " << engineName(st.params.engine) << "\n";
}

void listQuestions(const State& st, std::ostream& out) {
  for (std::size_t i = 0; i < st.scenarios.size(); ++i) {
    const Scenario& s = st.scenarios[i];
    char num[8];
    std::snprintf(num, sizeof num, "%3d", static_cast<int>(i + 1));
    out << "  " << num << ". " << s.queryId << "  " << s.category << "  (" << s.candidates.size() << " passages)  "
        << s.question << "\n";
  }
}

void printAnswer(const char* who, const AgentAnswer& a, std::ostream& out) {
  out << "  agent (" << who << "): " << (a.abstained ? "insufficient evidence" : a.text) << "\n";
}

void runOne(const State& st, std::ostream& out) {
  const Scenario& s = st.scenarios[st.query];
  Selection sel = selectContext(s.question, s.candidates, st.budget.context(), st.policy, st.params);
  ContextPackage pkg = buildPackage(s, sel, st.params.order);
  out << "\n--- context package sent to the agent ---\n" << renderPrompt(pkg) << "\n--- selector trace ---\n";
  printSelection(out, s, sel, true);
  SelStats m = measureSelection(s, sel);
  out << "\n  tokens " << m.tokens << "/" << sel.budgetCtx << (m.violation ? "  BUDGET VIOLATION" : "  (within budget)")
      << ", coverage " << m.goldHit << "/" << m.goldTotal << ", stale " << m.stale << ", redundancy " << m.redundancy
      << ", sources " << m.sources << "\n"
      << "  time " << sel.metrics.selectMicros << " us, comparisons " << sel.metrics.comparisons << ", heap pops "
      << sel.metrics.heapPops << ", re-insertions " << sel.metrics.reinserts << "\n";
  printAnswer("first-mention", FirstMentionAgent().answer(pkg, s.askKeys), out);
  printAnswer("authority", AuthorityAgent().answer(pkg, s.askKeys), out);
  if (!s.gold.empty()) {
    out << "  gold evidence:";
    for (const std::string& g : s.gold) out << " " << g << ";";
    out << "\n";
  }
}

void compareAll(const State& st, std::ostream& out) {
  const Scenario& s = st.scenarios[st.query];
  out << "\n  " << s.queryId << " at B_CTX = " << st.budget.context() << "\n";
  out << "  policy                status      tokens  coverage  stale  redund  sources   time_us  authority answer\n";
  for (Policy p : kPolicies) {
    Selection sel = selectContext(s.question, s.candidates, st.budget.context(), p, st.params);
    SelStats m = measureSelection(s, sel);
    ContextPackage pkg = buildPackage(s, sel, st.params.order);
    AgentAnswer a = AuthorityAgent().answer(pkg, s.askKeys);
    char row[160];
    std::snprintf(row, sizeof row, "  %-20s  %-10s  %6lld  %4d/%-4d  %5d  %6d  %7d  %8.2f  ", policyName(p),
                  statusName(sel.status), m.tokens, m.goldHit, m.goldTotal, m.stale, m.redundancy, m.sources,
                  sel.metrics.selectMicros);
    out << row << (a.abstained ? "insufficient evidence" : a.text) << "\n";
    if (sel.status == Status::Infeasible) out << "      " << sel.diagnostic << "\n";
  }
}

bool chooseQuestion(State& st, std::istream& in, std::ostream& out) {
  listQuestions(st, out);
  std::string line;
  if (!ask(in, out, "  question number or id (e.g. 1 or Q01): ", line)) return false;
  int n = 0;
  if (toInt(line, n) && n >= 1 && n <= static_cast<int>(st.scenarios.size())) {
    st.query = n - 1;
    return true;
  }
  for (std::size_t i = 0; i < st.scenarios.size(); ++i)
    if (st.scenarios[i].queryId == line) {
      st.query = static_cast<int>(i);
      return true;
    }
  out << "  no such question; keeping " << st.scenarios[st.query].queryId << "\n";
  return true;
}

bool chooseBudget(State& st, std::istream& in, std::ostream& out) {
  int mode = 0, n = 0;
  out << "  1. set B_CTX directly (reserves fixed at R_fixed 300, R_answer 500)\n"
      << "  2. set B_total, R_fixed and R_answer\n";
  if (!askInt(in, out, "  choice: ", 1, 2, mode)) return false;
  if (mode == 1) {
    if (!askInt(in, out, "  B_CTX tokens (0-200000): ", 0, 200000, n)) return false;
    st.budget = Budget{n + 800, 300, 500};
    return true;
  }
  Budget b;
  if (!askInt(in, out, "  B_total (0-1000000): ", 0, 1000000, b.total)) return false;
  if (!askInt(in, out, "  R_fixed (0-1000000): ", 0, 1000000, b.reserveFixed)) return false;
  if (!askInt(in, out, "  R_answer (0-1000000): ", 0, 1000000, b.reserveAnswer)) return false;
  st.budget = b;
  if (b.context() == 0) out << "  note: reserves use the whole budget, so B_CTX = 0\n";
  return true;
}

bool choosePolicy(State& st, std::istream& in, std::ostream& out) {
  for (int i = 0; i < 4; ++i) out << "  " << i + 1 << ". " << policyName(kPolicies[i]) << "\n";
  int n = 0;
  if (!askInt(in, out, "  policy: ", 1, 4, n)) return false;
  st.policy = kPolicies[n - 1];
  if (st.policy != Policy::Hybrid) {
    out << "  baseline engine: 1. merge sort + scan   2. binary heap\n";
    if (!askInt(in, out, "  engine: ", 1, 2, n)) return false;
    st.params.engine = n == 1 ? Engine::Sort : Engine::Heap;
  }
  return true;
}

bool chooseOrder(State& st, std::istream& in, std::ostream& out) {
  for (int i = 0; i < 3; ++i) out << "  " << i + 1 << ". " << orderName(kOrders[i]) << "\n";
  int n = 0;
  if (!askInt(in, out, "  packaging order: ", 1, 3, n)) return false;
  st.params.order = kOrders[n - 1];
  return true;
}

bool chooseDataset(State& st, std::istream& in, std::ostream& out) {
  for (int i = 0; i < kDatasetCount; ++i) out << "  " << i + 1 << ". " << kDatasets[i].name << "\n";
  int n = 0;
  if (!askInt(in, out, "  dataset: ", 1, kDatasetCount, n)) return false;
  loadData(st, n - 1, out);
  return true;
}

}  // namespace

int runMenu(std::istream& in, std::ostream& out) {
  State st;
  out << "CONTEXT BUDGET MANAGER - interactive demo (CS-251 CEP)\n";
  if (!loadData(st, 0, out)) {
    out << "  generating the evaluation set (seed 2026)...\n";
    generateDataset(kDatasets[0].scenarios, kDatasets[0].passages, 2026);
    if (!loadData(st, 0, out)) return 1;
  }
  for (;;) {
    printSettings(st, out);
    out << "\n  1. run selection for this question      6. change packaging order\n"
        << "  2. compare all four policies            7. switch dataset (evaluation / edge / MUET)\n"
        << "  3. choose question                      8. run the automated tests\n"
        << "  4. set token budget                     9. list questions\n"
        << "  5. choose policy                        0. exit\n";
    int choice = 0;
    if (!askInt(in, out, "\nchoice: ", 0, 9, choice)) return 0;
    bool more = true;
    switch (choice) {
      case 0: return 0;
      case 1: runOne(st, out); break;
      case 2: compareAll(st, out); break;
      case 3: more = chooseQuestion(st, in, out); break;
      case 4: more = chooseBudget(st, in, out); break;
      case 5: more = choosePolicy(st, in, out); break;
      case 6: more = chooseOrder(st, in, out); break;
      case 7: more = chooseDataset(st, in, out); break;
      case 8: {
        std::ofstream file("results/tests.txt");
        int rc = runTests("data", file);
        file.close();
        std::ifstream res("results/tests.txt");
        out << res.rdbuf() << (rc == 0 ? "" : "  some tests FAILED\n");
        break;
      }
      case 9: listQuestions(st, out); break;
    }
    if (!more) return 0;
  }
}
