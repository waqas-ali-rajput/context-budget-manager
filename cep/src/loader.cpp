#include "loader.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "hash_map.h"

std::vector<std::string> splitString(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : s) {
    if (ch == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += ch;
    }
  }
  out.push_back(cur);
  return out;
}

std::string formatTimestamp(long long ts) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04lld-%02lld-%02lldT%02lld:%02lld", ts / 100000000LL,
                ts / 1000000LL % 100, ts / 10000LL % 100, ts / 100LL % 100, ts % 100);
  return buf;
}

namespace {

std::string trim(const std::string& s) {
  std::string::size_type b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  std::string::size_type e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool allDigits(const std::string& s) {
  if (s.empty()) return false;
  for (char ch : s)
    if (ch < '0' || ch > '9') return false;
  return true;
}

bool parseDouble(const std::string& s, double& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return end && *end == '\0';
}

}  // namespace

bool parseTimestamp(const std::string& s, long long& out) {
  // Format YYYY-MM-DDThh:mm
  if (s.size() != 16 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':') return false;
  const std::string y = s.substr(0, 4), mo = s.substr(5, 2), d = s.substr(8, 2), h = s.substr(11, 2),
                    mi = s.substr(14, 2);
  if (!allDigits(y) || !allDigits(mo) || !allDigits(d) || !allDigits(h) || !allDigits(mi)) return false;
  const int Y = std::atoi(y.c_str()), M = std::atoi(mo.c_str()), D = std::atoi(d.c_str()),
            H = std::atoi(h.c_str()), N = std::atoi(mi.c_str());
  if (M < 1 || M > 12 || D < 1 || D > 31 || H > 23 || N > 59 || Y < 1970) return false;
  out = Y * 100000000LL + M * 1000000LL + D * 10000LL + H * 100LL + N;
  return true;
}

bool parsePassageLine(const std::string& line, Passage& p, std::string& err) {
  std::vector<std::string> f = splitString(line, '|');
  if (f.size() != 12) {
    err = "expected 12 fields, found " + std::to_string(f.size());
    return false;
  }
  for (std::string& x : f) x = trim(x);
  p = Passage();
  p.queryId = f[0];
  p.passageId = f[1];
  p.sourceId = f[2];
  if (p.queryId.empty()) return err = "missing query_id", false;
  if (p.passageId.empty() || p.passageId == "-") return err = "missing passage_id", false;
  if (p.sourceId.empty() || p.sourceId == "-") return err = "missing source_id", false;
  if (!parseTimestamp(f[3], p.timestamp))
    return err = "invalid timestamp '" + f[3] + "' (expected YYYY-MM-DDThh:mm)", false;
  if (!f[4].empty() && f[4][0] == '-') return err = "negative token cost '" + f[4] + "'", false;
  if (!allDigits(f[4]) || f[4].size() > 7)
    return err = "token cost '" + f[4] + "' is not a nonnegative integer", false;
  p.cost = std::atoi(f[4].c_str());
  if (!parseDouble(f[5], p.relevance) || !(p.relevance >= 0.0 && p.relevance <= 1.0))
    return err = "relevance '" + f[5] + "' is not a number in [0,1]", false;
  if (f[6].size() != 1 || (f[6][0] != 'A' && f[6][0] != 'B' && f[6][0] != 'C'))
    return err = "reliability '" + f[6] + "' is not A, B or C", false;
  p.reliability = f[6][0];
  p.dupGroup = (f[7].empty() || f[7] == "-") ? p.passageId : f[7];
  if (!f[8].empty() && f[8] != "-") {
    for (const std::string& t : splitString(f[8], ';')) {
      std::string tag = trim(t);
      std::string::size_type c = tag.find(':');
      if (c == std::string::npos || c == 0 || c + 1 == tag.size())
        return err = "evidence tag '" + tag + "' is not key:value", false;
      p.tags.push_back(tag);
    }
  }
  if (f[9] != "0" && f[9] != "1") return err = "mandatory flag '" + f[9] + "' is not 0 or 1", false;
  p.mandatory = f[9] == "1";
  if (!f[10].empty() && f[10] != "-") {
    if (f[10] == p.passageId) return err = "passage supersedes itself", false;
    p.supersedes = f[10];
  }
  p.text = f[11] == "-" ? "" : f[11];
  return true;
}

std::vector<Passage> loadPassages(const std::string& path, LoadReport& report) {
  std::vector<Passage> out;
  std::ifstream in(path);
  if (!in) {
    report.errors.push_back({path, 0, "cannot open file"});
    return out;
  }
  HashMap seen(2048);  // "query|passage" -> line of first occurrence
  std::string line;
  int lineNo = 0;
  while (std::getline(in, line)) {
    ++lineNo;
    std::string t = trim(line);
    if (t.empty() || t[0] == '#') continue;
    Passage p;
    std::string err;
    if (!parsePassageLine(t, p, err)) {
      report.errors.push_back({path, lineNo, err});
      continue;
    }
    const std::string key = p.queryId + "|" + p.passageId;
    if (const int* first = seen.find(key)) {
      report.errors.push_back({path, lineNo,
                               "duplicate passage id " + p.passageId + " in " + p.queryId +
                                   " (first seen on line " + std::to_string(*first) + ")"});
      continue;
    }
    seen.insert(key, lineNo);
    out.push_back(p);
    ++report.accepted;
  }
  return out;
}

std::vector<Scenario> loadDataset(const std::string& scenarioPath, const std::string& passagePath,
                                  LoadReport& report) {
  std::vector<Scenario> scenarios;
  std::ifstream in(scenarioPath);
  if (!in) {
    report.errors.push_back({scenarioPath, 0, "cannot open file"});
    return scenarios;
  }
  HashMap index(64);
  std::string line;
  int lineNo = 0;
  while (std::getline(in, line)) {
    ++lineNo;
    std::string t = trim(line);
    if (t.empty() || t[0] == '#') continue;
    std::vector<std::string> f = splitString(t, '|');
    if (f.size() != 5) {
      report.errors.push_back({scenarioPath, lineNo, "expected 5 fields"});
      continue;
    }
    Scenario s;
    s.queryId = trim(f[0]);
    s.category = trim(f[1]);
    s.question = trim(f[2]);
    if (s.category != "policy" && s.category != "multisource" && s.category != "memory" &&
        s.category != "lookup" && s.category != "unanswerable") {
      report.errors.push_back({scenarioPath, lineNo, "unknown category '" + s.category + "'"});
      continue;
    }
    for (const std::string& k : splitString(trim(f[3]), ';'))
      if (!trim(k).empty()) s.askKeys.push_back(trim(k));
    if (trim(f[4]) != "-")
      for (const std::string& g : splitString(trim(f[4]), ';'))
        if (!trim(g).empty()) s.gold.push_back(trim(g));
    if (!index.insert(s.queryId, static_cast<int>(scenarios.size()))) {
      report.errors.push_back({scenarioPath, lineNo, "duplicate query id " + s.queryId});
      continue;
    }
    scenarios.push_back(s);
  }
  std::vector<Passage> passages = loadPassages(passagePath, report);
  for (Passage& p : passages) {
    const int* at = index.find(p.queryId);
    if (!at) {
      report.errors.push_back({passagePath, 0, "passage " + p.passageId + " has unknown query " + p.queryId});
      continue;
    }
    scenarios[*at].candidates.push_back(p);
  }
  return scenarios;
}
