#pragma once
#include <string>
#include <vector>

// One candidate passage (FR-1). All fields are filled by the loader or the generator.
struct Passage {
  std::string queryId;
  std::string passageId;          // unique within the query
  std::string sourceId;           // provenance
  long long timestamp = 0;        // YYYYMMDDhhmm (freshness indicator)
  int cost = 0;                   // c_i from the fixture, includes label/separator overhead
  double relevance = 0.0;         // retriever score in [0, 1]
  char reliability = 'B';         // A = official, B = secondary, C = informal
  std::string dupGroup;           // near-duplicate group; "-" in the file -> own passage id
  std::vector<std::string> tags;  // evidence tags "key:value"; "-" in the file -> none
  bool mandatory = false;
  std::string supersedes;         // passage id this passage replaces; "" if none
  std::string text;
};

// One labelled query scenario (Section 8 of the brief).
struct Scenario {
  std::string queryId;
  std::string category;               // policy | multisource | memory | unanswerable
  std::string question;
  std::vector<std::string> askKeys;   // facts the question asks for
  std::vector<std::string> gold;      // gold evidence tags "key:value" (empty = no gold facts)
  std::vector<Passage> candidates;
};

inline double reliabilityWeight(char r) { return r == 'A' ? 1.0 : r == 'B' ? 0.85 : 0.7; }

inline std::string tagKey(const std::string& tag) {
  std::string::size_type p = tag.find(':');
  return p == std::string::npos ? tag : tag.substr(0, p);
}
inline std::string tagValue(const std::string& tag) {
  std::string::size_type p = tag.find(':');
  return p == std::string::npos ? std::string() : tag.substr(p + 1);
}

std::string formatTimestamp(long long ts);  // "YYYY-MM-DDThh:mm"
