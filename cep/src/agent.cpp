#include "agent.h"

#include <sstream>

ContextPackage buildPackage(const Scenario& s, const Selection& sel, OrderRule order) {
  ContextPackage pkg;
  pkg.queryId = s.queryId;
  pkg.question = s.question;
  pkg.policy = policyName(sel.policy);
  pkg.orderRule = orderName(order);
  pkg.status = statusName(sel.status);
  pkg.budgetCtx = sel.budgetCtx;
  pkg.tokenTotal = sel.tokenTotal;
  int pos = 0;
  for (const Chosen& ch : sel.ordered) {
    const Passage& p = s.candidates[ch.index];
    pkg.items.push_back({++pos, p.passageId, p.sourceId, p.text, p.timestamp, p.reliability, p.cost, p.tags});
  }
  return pkg;
}

std::string renderPrompt(const ContextPackage& pkg) {
  std::ostringstream o;
  o << "Question: " << pkg.question << "\nContext (" << pkg.tokenTotal << "/" << pkg.budgetCtx
    << " tokens, " << pkg.policy << "):\n";
  for (const PackagedPassage& p : pkg.items)
    o << "[" << p.position << "] (" << p.passageId << " | " << p.sourceId << " | "
      << formatTimestamp(p.timestamp) << " | " << p.reliability << ") " << p.text << "\n";
  return o.str();
}

namespace {

// Collects every (position, value) stating `key` in context order.
struct Mention {
  const PackagedPassage* p;
  std::string value;
};

std::vector<Mention> mentions(const ContextPackage& pkg, const std::string& key) {
  std::vector<Mention> out;
  for (const PackagedPassage& p : pkg.items)
    for (const std::string& t : p.tags)
      if (tagKey(t) == key) out.push_back({&p, tagValue(t)});
  return out;
}

bool hasConflict(const std::vector<Mention>& m) {
  for (std::size_t i = 1; i < m.size(); ++i)
    if (m[i].value != m[0].value) return true;
  return false;
}

AgentAnswer finish(AgentAnswer a, const ContextPackage& pkg) {
  std::ostringstream o;
  if (a.claims.empty()) {
    a.abstained = true;
    o << "Insufficient evidence in the provided context to answer \"" << pkg.question << "\".";
  } else {
    for (const Claim& c : a.claims) {
      o << c.key << " = " << c.value << " [" << c.passageId << ", " << c.sourceId << "]";
      if (c.conflict) o << " (sources disagree)";
      o << "; ";
    }
  }
  a.text = o.str();
  return a;
}

}  // namespace

AgentAnswer FirstMentionAgent::answer(const ContextPackage& pkg, const std::vector<std::string>& askKeys) const {
  AgentAnswer a;
  for (const std::string& key : askKeys) {
    std::vector<Mention> m = mentions(pkg, key);
    if (m.empty()) continue;
    a.claims.push_back({key, m[0].value, m[0].p->passageId, m[0].p->sourceId, m[0].p->reliability, hasConflict(m)});
  }
  return finish(a, pkg);
}

AgentAnswer AuthorityAgent::answer(const ContextPackage& pkg, const std::vector<std::string>& askKeys) const {
  AgentAnswer a;
  for (const std::string& key : askKeys) {
    std::vector<Mention> m = mentions(pkg, key);
    if (m.empty()) continue;
    std::size_t best = 0;
    for (std::size_t i = 1; i < m.size(); ++i) {
      const PackagedPassage& x = *m[i].p;
      const PackagedPassage& b = *m[best].p;
      if (x.reliability < b.reliability || (x.reliability == b.reliability && x.timestamp > b.timestamp)) best = i;
    }
    a.claims.push_back({key, m[best].value, m[best].p->passageId, m[best].p->sourceId, m[best].p->reliability,
                        hasConflict(m)});
  }
  return finish(a, pkg);
}

AnswerScore scoreAnswer(const AgentAnswer& a, const Scenario& s) {
  AnswerScore r;
  r.asked = static_cast<int>(s.askKeys.size());
  r.answerable = !s.gold.empty();
  r.abstained = a.abstained;
  for (const std::string& key : s.askKeys) {
    std::string goldValue;
    bool hasGold = false;
    for (const std::string& g : s.gold)
      if (tagKey(g) == key) {
        goldValue = tagValue(g);
        hasGold = true;
      }
    const Claim* claim = nullptr;
    for (const Claim& c : a.claims)
      if (c.key == key) claim = &c;
    if (!claim) {
      ++r.missing;
      continue;
    }
    if (claim->conflict) ++r.conflictsFlagged;
    if (hasGold && claim->value == goldValue) {
      ++r.correct;
      if (claim->reliability == 'C') ++r.weakOnly;
    } else {
      ++r.contradictory;
    }
  }
  if (r.answerable) r.score = r.asked ? static_cast<double>(r.correct) / r.asked : 0.0;
  else r.score = (a.abstained && r.contradictory == 0) ? 1.0 : 0.0;
  return r;
}
