#pragma once
#include <string>
#include <vector>

#include "passage.h"

// FR-2: ingestion and validation. A malformed or duplicate record is rejected with its
// line number and reason; valid records on other lines are still loaded.
struct LoadError {
  std::string file;
  int line = 0;
  std::string message;
};

struct LoadReport {
  int accepted = 0;
  std::vector<LoadError> errors;
};

bool parseTimestamp(const std::string& s, long long& out);
// Parses one passage record (12 '|' fields). Returns false and sets err on any invalid field.
bool parsePassageLine(const std::string& line, Passage& p, std::string& err);

std::vector<Passage> loadPassages(const std::string& path, LoadReport& report);
// Loads scenarios and attaches each passage to its scenario by query id.
std::vector<Scenario> loadDataset(const std::string& scenarioPath, const std::string& passagePath,
                                  LoadReport& report);

std::vector<std::string> splitString(const std::string& s, char sep);
