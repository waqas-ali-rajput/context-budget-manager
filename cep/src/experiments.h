#pragma once
#include <ostream>
#include <string>
#include <vector>

#include "passage.h"
#include "selector.h"

// Independent post-hoc measurements of one selection (Section 8 measures).
struct SelStats {
  long long tokens = 0;
  bool violation = false;
  int goldHit = 0, goldTotal = 0;
  double coverage = -1.0;  // -1 = query has no gold facts (reported separately)
  int redundancy = 0, sources = 0, stale = 0, selected = 0;
  double relevanceSum = 0.0;
};
SelStats measureSelection(const Scenario& s, const Selection& sel);

void printSelection(std::ostream& o, const Scenario& s, const Selection& sel, bool showOmitted);

int runEval(const std::vector<Scenario>& scenarios, const std::string& resultsDir);
int runBench(const std::string& resultsDir, int reps);
// MUET document set: 4 budgets x 4 policies x 20 questions -> results/muet_summary.txt
int runMuet(const std::vector<Scenario>& scenarios, const std::string& resultsDir);
int runEdge(const std::string& dataDir, const std::string& resultsDir);
int runTests(const std::string& dataDir, std::ostream& out);
