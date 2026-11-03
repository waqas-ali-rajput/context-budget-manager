#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "passage.h"

// Student-written SplitMix64 generator: identical streams on every compiler/platform,
// so a seed fully determines the dataset (std:: distributions are implementation-defined).
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : s_(seed) {}
  std::uint64_t next() {
    std::uint64_t z = (s_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  int range(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<std::uint64_t>(hi - lo + 1)); }
  double real(double lo, double hi) { return lo + (hi - lo) * (static_cast<double>(next() >> 11) / 9007199254740992.0); }
  bool chance(double p) { return real(0.0, 1.0) < p; }

 private:
  std::uint64_t s_;
};

constexpr int kPassageOverhead = 6;  // tokens for the "[n] (id | source | date | rel)" label + separator

// Writes the labelled evaluation set (32 scenarios) to the two files.
void generateDataset(const std::string& scenarioPath, const std::string& passagePath, std::uint64_t seed);

// Synthetic runtime workload of n candidates (seed = 20260000 + n by convention).
std::vector<Passage> makeScalingWorkload(int n, std::uint64_t seed);

// Independent label checks (different code path from the generator).
struct ValidationReport {
  int scenarios = 0, passages = 0, policy = 0, multisource = 0, memory = 0, lookup = 0, unanswerable = 0;
  int goldTags = 0, taggedPassages = 0, supersedeLinks = 0, mandatoryPassages = 0;
  std::vector<std::string> problems;
  std::vector<std::string> notes;
};
ValidationReport validateDataset(const std::vector<Scenario>& scenarios);
