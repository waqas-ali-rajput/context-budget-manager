#pragma once
// FR-9 agent interface. The selector produces a Selection; buildPackage turns it into a
// model-independent ContextPackage; any QaAgent consumes only the package.
// No LLM is called: the two agents below are deterministic, extractive stand-ins used for an
// evidence-based answerability PROXY (not observed LLM answer accuracy).
#include <string>
#include <vector>

#include "passage.h"
#include "selector.h"

struct PackagedPassage {
  int position = 0;
  std::string passageId, sourceId, text;
  long long timestamp = 0;
  char reliability = 'B';
  int cost = 0;
  std::vector<std::string> tags;
};

struct ContextPackage {
  std::string queryId, question, policy, orderRule, status;
  int budgetCtx = 0, tokenTotal = 0;
  std::vector<PackagedPassage> items;
};

ContextPackage buildPackage(const Scenario& s, const Selection& sel, OrderRule order);
std::string renderPrompt(const ContextPackage& pkg);  // the text an LLM would receive

struct Claim {
  std::string key, value, passageId, sourceId;
  char reliability = 'B';
  bool conflict = false;  // another passage in the context gives a different value
};

struct AgentAnswer {
  bool abstained = false;
  std::vector<Claim> claims;
  std::string text;
};

class QaAgent {
 public:
  virtual ~QaAgent() = default;
  virtual const char* name() const = 0;
  virtual AgentAnswer answer(const ContextPackage& pkg, const std::vector<std::string>& askKeys) const = 0;
};

// Position-biased: uses the first passage (in context order) that states each asked fact.
class FirstMentionAgent : public QaAgent {
 public:
  const char* name() const override { return "first-mention"; }
  AgentAnswer answer(const ContextPackage& pkg, const std::vector<std::string>& askKeys) const override;
};

// Order-independent: prefers the most reliable source, then the newest timestamp.
class AuthorityAgent : public QaAgent {
 public:
  const char* name() const override { return "authority"; }
  AgentAnswer answer(const ContextPackage& pkg, const std::vector<std::string>& askKeys) const override;
};

// Checklist scoring against the disclosed gold tags.
struct AnswerScore {
  int asked = 0, correct = 0, contradictory = 0, missing = 0, conflictsFlagged = 0, weakOnly = 0;
  bool answerable = true, abstained = false;
  double score = 0.0;  // correct / asked; for no-gold questions 1 if the agent abstained
};

AnswerScore scoreAnswer(const AgentAnswer& a, const Scenario& s);
