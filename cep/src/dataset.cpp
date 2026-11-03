#include "dataset.h"

#include <cstdio>
#include <fstream>

#include "hash_map.h"

namespace {

long long ts(int y, int mo, int d, int h, int mi) {
  return y * 100000000LL + mo * 1000000LL + d * 10000LL + h * 100LL + mi;
}

// Realistic passage-cost mix: 70 % short, 23 % medium, 7 % long, plus label overhead.
int drawCost(Rng& r) {
  const double u = r.real(0.0, 1.0);
  const int body = u < 0.70 ? r.range(14, 90) : u < 0.93 ? r.range(91, 180) : r.range(181, 320);
  return body + kPassageOverhead;
}

long long drawDate(Rng& r, int y0, int m0, int y1, int m1) {
  const int a = y0 * 12 + (m0 - 1), b = y1 * 12 + (m1 - 1);
  const int m = r.range(a, b);
  return ts(m / 12, m % 12 + 1, r.range(1, 28), r.range(8, 17), r.range(0, 59));
}

// Passage under construction; handles are positions in the spec list, resolved after shuffling.
struct Spec {
  std::string source;
  long long timestamp = 0;
  int cost = 0;
  double relevance = 0;
  char reliability = 'B';
  int group = -1;       // duplicate-group handle (-1 = own group)
  int supersedes = -1;  // spec handle
  bool mandatory = false;
  std::vector<std::string> tags;
  std::string text;
};

struct Builder {
  Rng& r;
  std::string qid;
  std::vector<Spec> specs;
  int groups = 0;

  int add(const Spec& s) {
    specs.push_back(s);
    return static_cast<int>(specs.size()) - 1;
  }
  int newGroup() { return groups++; }

  void distractors(int count, double relLo, double relHi, int y0, int m0, int y1, int m1, const char* const* sources,
                   int nSources) {
    static const char* const kMisc[] = {
        "the sports gala schedule", "the canteen menu", "the convocation dress code", "the bus route change",
        "the blood donation camp", "the robotics club meeting", "the alumni talk", "the career fair",
        "the photography contest", "the hostel maintenance work", "the Wi-Fi outage", "the seminar on AI ethics",
        "the debate competition", "the library book fair", "the vaccination drive", "the road repairs",
        "the cricket trials", "the coding contest", "the art exhibition", "the water supply schedule"};
    for (int j = 0; j < count; ++j) {
      Spec s;
      s.source = sources[r.range(0, nSources - 1)];
      s.reliability = "ABC"[r.range(0, 2)];
      s.timestamp = drawDate(r, y0, m0, y1, m1);
      s.cost = drawCost(r);
      s.relevance = r.real(relLo, relHi);
      const int m = static_cast<int>(specs.size());
      const std::string val = "M" + std::to_string(m);
      s.tags.push_back("misc" + std::to_string(m) + ":" + val);
      s.text = "Update on " + std::string(kMisc[r.range(0, 19)]) + " (ref " + val + ").";
      const int h = add(s);
      if (j + 1 < count && r.chance(0.3)) {  // near-duplicate repost of the same update
        Spec d = specs[h];
        specs[h].group = d.group = newGroup();
        d.source = sources[r.range(0, nSources - 1)];
        const int delta = r.range(-4, 4);
        if (d.cost + delta > kPassageOverhead) d.cost += delta;
        d.relevance = r.real(relLo, relHi);
        d.text = "Repost: " + d.text;
        add(d);
        ++j;
      }
    }
  }

  // Shuffle (Fisher-Yates) so file order carries no label information, then assign ids.
  void write(std::ofstream& out) {
    const int n = static_cast<int>(specs.size());
    std::vector<int> perm(n);
    for (int i = 0; i < n; ++i) perm[i] = i;
    for (int i = n - 1; i > 0; --i) {
      const int j = r.range(0, i);
      const int t = perm[i];
      perm[i] = perm[j];
      perm[j] = t;
    }
    std::vector<std::string> idOf(n);
    for (int pos = 0; pos < n; ++pos) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%s-P%03d", qid.c_str(), pos + 1);
      idOf[perm[pos]] = buf;
    }
    for (int pos = 0; pos < n; ++pos) {
      const Spec& s = specs[perm[pos]];
      std::string tags;
      for (const std::string& t : s.tags) tags += (tags.empty() ? "" : ";") + t;
      char rel[16];
      std::snprintf(rel, sizeof rel, "%.3f", s.relevance);
      out << qid << '|' << idOf[perm[pos]] << '|' << s.source << '|' << formatTimestamp(s.timestamp) << '|'
          << s.cost << '|' << rel << '|' << s.reliability << '|'
          << (s.group >= 0 ? qid + "-G" + std::to_string(s.group) : "-") << '|' << (tags.empty() ? "-" : tags)
          << '|' << (s.mandatory ? 1 : 0) << '|' << (s.supersedes >= 0 ? idOf[s.supersedes] : "-") << '|'
          << s.text << '\n';
    }
  }
};

const char* const kNoticeSources[] = {"notices_office", "dept_email", "faq_portal", "student_forum", "lms_portal",
                                      "handbook", "registrar"};
const int kNoticeSourceCount = 7;

struct PolicyTopic {
  const char *question, *phrase1, *key1, *oldV, *newV, *phrase2, *key2, *val2;
};
const PolicyTopic kPolicy[10] = {
    {"What is the last date to drop a course this semester, and which form is needed?",
     "the last date to drop a course is", "drop_deadline", "27 Feb 2026", "6 Mar 2026",
     "course drops are submitted on", "drop_form", "Form AC-12"},
    {"What lab attendance is required to sit the final lab exam, and where are appeals made?",
     "the minimum lab attendance is", "lab_attendance_min", "75 percent", "80 percent",
     "attendance appeals go to", "attendance_appeal", "the Lab Coordinator Office"},
    {"When is the semester fee due and what is the late charge?", "the semester fee deadline is", "fee_deadline",
     "15 Aug 2026", "29 Aug 2026", "the late payment charge is", "fee_late_charge", "PKR 500 per day"},
    {"What is the re-sit examination fee and which form is used?", "the re-sit examination fee is", "resit_fee",
     "PKR 2000", "PKR 3000", "re-sit requests use", "resit_form", "Form EX-7"},
    {"What are the library hours during exams and what is needed to enter?", "exam-period library hours are",
     "library_hours", "8am to 8pm", "8am to 10pm", "library entry requires", "library_entry",
     "a valid student ID card"},
    {"What similarity limit applies to project reports and which report is accepted?",
     "the similarity limit for reports is", "plagiarism_limit", "20 percent", "15 percent",
     "the accepted similarity report is", "plagiarism_tool", "a Turnitin report"},
    {"Where are final-year project reports submitted and in what format?", "FYP reports are submitted on",
     "project_portal", "LMS v1", "the LMS v2 portal", "the required report format is", "project_format",
     "a single PDF file"},
    {"What is the hostel curfew and until when are guests allowed?", "the hostel curfew is", "hostel_curfew", "10pm",
     "11pm", "hostel guests are allowed until", "hostel_guests", "7pm"},
    {"How long is the grade appeal window and what is the appeal fee?", "the grade appeal window is",
     "appeal_window", "7 days", "10 days", "the grade appeal fee is", "appeal_fee", "PKR 1000"},
    {"What CGPA is required for the merit scholarship and which documents are needed?",
     "the merit scholarship cut-off is", "scholarship_cgpa", "3.3 CGPA", "3.5 CGPA",
     "scholarship applicants must attach", "scholarship_docs", "an income certificate"},
};

void policyScenario(Builder& b, const PolicyTopic& t, bool infeasibleCase) {
  Rng& r = b.r;
  const int gOld = b.newGroup(), gNew = b.newGroup();
  Spec olds;
  olds.source = "notices_office";
  olds.reliability = 'A';
  olds.timestamp = drawDate(r, 2025, 1, 2025, 9);
  olds.cost = r.range(60, 140) + kPassageOverhead;
  olds.relevance = r.real(0.82, 0.95);
  olds.group = gOld;
  olds.tags = {std::string(t.key1) + ":" + t.oldV};
  olds.text = "Official notice: " + std::string(t.phrase1) + " " + t.oldV + ".";
  const int hOld = b.add(olds);

  Spec news;
  news.source = "notices_office";
  news.reliability = 'A';
  news.timestamp = drawDate(r, 2026, 1, 2026, 6);
  news.cost = infeasibleCase ? 290 : r.range(34, 84) + kPassageOverhead;
  news.relevance = r.real(0.62, 0.80);
  news.group = gNew;
  news.mandatory = true;
  news.supersedes = hOld;
  news.tags = {std::string(t.key1) + ":" + t.newV};
  news.text = "Revised official notice (replaces the earlier one): " + std::string(t.phrase1) + " " + t.newV + ".";
  b.add(news);

  for (int k = 0; k < 3; ++k) {  // forum reposts of the old notice
    Spec rp;
    rp.source = "student_forum";
    rp.reliability = 'C';
    rp.timestamp = drawDate(r, 2025, 10, 2025, 12);
    rp.cost = r.range(24, 54) + kPassageOverhead;
    rp.relevance = r.real(0.78, 0.90);
    rp.group = gOld;
    rp.tags = olds.tags;
    rp.text = "Forum repost: " + std::string(t.phrase1) + " " + t.oldV + ".";
    b.add(rp);
  }

  Spec mail;
  mail.source = "dept_email";
  mail.reliability = 'B';
  mail.timestamp = news.timestamp + 10000;  // a day later
  mail.cost = r.range(20, 50) + kPassageOverhead;
  mail.relevance = r.real(0.55, 0.70);
  mail.group = gNew;
  mail.tags = news.tags;
  mail.text = "Department email: please note " + std::string(t.phrase1) + " " + t.newV + ".";
  b.add(mail);

  Spec second;
  second.tags = {std::string(t.key2) + ":" + t.val2};
  second.text = std::string(infeasibleCase ? "Scholarship annex (mandatory reading): " : "FAQ: ") + t.phrase2 + " " +
                t.val2 + ".";
  if (infeasibleCase) {
    second.source = "scholarship_office";
    second.reliability = 'A';
    second.cost = 330;
    second.mandatory = true;
    second.relevance = r.real(0.60, 0.70);
  } else {
    second.source = "faq_portal";
    second.reliability = 'B';
    second.cost = r.range(24, 64) + kPassageOverhead;
    second.relevance = r.real(0.50, 0.70);
  }
  second.timestamp = drawDate(r, 2026, 2, 2026, 7);
  b.add(second);

  b.distractors(8, 0.05, 0.35, 2026, 9, 2026, 10, kNoticeSources, kNoticeSourceCount);  // newest, irrelevant
  b.distractors(27, 0.20, 0.65, 2024, 1, 2026, 8, kNoticeSources, kNoticeSourceCount);
}

struct Fact {
  const char *source, *phrase, *key, *value;
};
struct MultiTopic {
  const char* question;
  Fact f[4];
  const char* wrong3;  // contradicting informal value for fact 3
  bool longHasAll;
};
const MultiTopic kMulti[10] = {
    {"How do I register for the final-year project?",
     {{"handbook", "FYP groups must have", "fyp_group_size", "3 to 4 students"},
      {"dept_office", "the supervisor consent form is", "fyp_form", "Form FYP-1"},
      {"lms_portal", "FYP proposals are due in", "fyp_proposal_due", "week 4"},
      {"exam_office", "FYP marks are split as", "fyp_marks_split", "40 internal and 60 external"}},
     "week 6", true},
    {"What do I need for a lab equipment loan?",
     {{"lab_wiki", "equipment loans use", "loan_form", "Form LB-3"},
      {"store_office", "the loan deposit is", "loan_deposit", "PKR 1500"},
      {"handbook", "the maximum loan period is", "loan_period", "14 days"},
      {"dept_email", "loans are signed off by", "loan_signoff", "the lab engineer"}},
     "30 days", false},
    {"How does internship credit work?",
     {{"handbook", "the minimum internship length is", "intern_weeks", "6 weeks"},
      {"industry_cell", "registration requires", "intern_letter", "an offer letter"},
      {"exam_office", "an internship is worth", "intern_credits", "3 credit hours"},
      {"lms_portal", "students must upload", "intern_report", "a 10-page report"}},
     "1 credit hour", false},
    {"How do I apply for a transcript?",
     {{"registrar", "the transcript fee is", "transcript_fee", "PKR 800"},
      {"faq_portal", "transcripts are issued in", "transcript_days", "5 working days"},
      {"handbook", "transcript requests use", "transcript_form", "Form RG-2"},
      {"dept_email", "transcripts are collected from", "transcript_pickup", "the Registrar counter"}},
     "Form RG-9", true},
    {"What are the rules for using the HPC cluster?",
     {{"it_services", "the HPC storage quota is", "hpc_quota", "500 GB"},
      {"lab_wiki", "the longest HPC job is", "hpc_max_job", "48 hours"},
      {"handbook", "HPC accounts need", "hpc_access", "a supervisor request"},
      {"faq_portal", "HPC problems go to", "hpc_support", "the HPC help desk"}},
     "a self-registration", false},
    {"How do I get an exam clash resolved?",
     {{"exam_office", "clash requests close", "clash_deadline", "3 days before the exam"},
      {"registrar", "clash requests use", "clash_form", "Form EX-3"},
      {"dept_office", "the clash contact is", "clash_contact", "the batch advisor"},
      {"faq_portal", "clash requests must include", "clash_proof", "a printed timetable"}},
     "the course teacher", false},
    {"How are elective courses allotted?",
     {{"registrar", "electives are allotted in", "elective_basis", "CGPA order"},
      {"dept_office", "each elective is capped at", "elective_cap", "40 students"},
      {"lms_portal", "elective choices open in", "elective_window", "the first week"},
      {"handbook", "students must take at least", "elective_min", "2 electives"}},
     "the second week", true},
    {"What is needed to start a student society?",
     {{"student_affairs", "a new society needs", "society_members", "15 members"},
      {"handbook", "every society must have", "society_advisor", "a faculty advisor"},
      {"dept_email", "society registration uses", "society_form", "Form SA-1"},
      {"faq_portal", "the first-year society budget is", "society_budget", "PKR 20000"}},
     "Form SA-4", false},
    {"How do I book a seminar room?",
     {{"admin_office", "rooms must be booked", "room_lead", "2 days ahead"},
      {"lms_portal", "bookings are made on", "room_system", "the room booking page"},
      {"handbook", "seminar rooms are open", "room_hours", "9am to 5pm"},
      {"it_services", "projectors need", "room_projector", "a separate projector request"}},
     "8am to 8pm", false},
    {"What is the procedure for medical leave?",
     {{"medical_centre", "medical leave needs", "medleave_cert", "a medical certificate"},
      {"registrar", "leave must be filed", "medleave_days", "within 7 days"},
      {"dept_office", "medical leave uses", "medleave_form", "Form ML-1"},
      {"handbook", "the longest medical leave is", "medleave_max", "15 days"}},
     "Form ML-5", false},
};

char reliabilityOf(const std::string& source) {
  if (source == "handbook" || source == "registrar" || source == "exam_office" || source == "medical_centre")
    return 'A';
  if (source == "student_forum") return 'C';
  return 'B';
}

void multiScenario(Builder& b, const MultiTopic& t) {
  Rng& r = b.r;
  const int g1 = b.newGroup();
  for (int k = 0; k < 4; ++k) {
    Spec s;
    s.source = t.f[k].source;
    s.reliability = reliabilityOf(s.source);
    s.timestamp = drawDate(r, 2025, 6, 2026, 8);
    s.cost = r.range(29, 74) + kPassageOverhead;
    s.relevance = r.real(0.50, 0.75);
    s.tags = {std::string(t.f[k].key) + ":" + t.f[k].value};
    s.text = std::string(t.f[k].phrase) + " " + t.f[k].value + ".";
    if (k == 0) s.group = g1;
    b.add(s);
  }
  Spec lng;  // one long, highly relevant overview passage
  lng.source = "student_guide";
  lng.reliability = 'A';
  lng.timestamp = drawDate(r, 2025, 1, 2025, 12);
  lng.cost = r.range(254, 374) + kPassageOverhead;
  lng.relevance = r.real(0.93, 0.98);
  lng.text = "Student guide overview:";
  for (int k = 0; k < (t.longHasAll ? 4 : 3); ++k) {
    lng.tags.push_back(std::string(t.f[k].key) + ":" + t.f[k].value);
    lng.text += std::string(" ") + t.f[k].phrase + " " + t.f[k].value + ";";
  }
  lng.text += " further background and examples follow.";
  b.add(lng);

  const char* const copySources[] = {"faq_portal", "student_forum", "dept_email", "lms_portal", "student_forum"};
  for (int k = 0; k < 5; ++k) {  // near-duplicates of fact 1
    Spec d;
    d.source = copySources[k];
    d.reliability = reliabilityOf(d.source);
    d.timestamp = drawDate(r, 2025, 6, 2026, 9);
    d.cost = r.range(24, 54) + kPassageOverhead;
    d.relevance = r.real(0.80, 0.90);
    d.group = g1;
    d.tags = {std::string(t.f[0].key) + ":" + t.f[0].value};
    d.text = "Reminder: " + std::string(t.f[0].phrase) + " " + t.f[0].value + ".";
    b.add(d);
  }
  Spec bad;  // informal post that contradicts fact 3
  bad.source = "student_forum";
  bad.reliability = 'C';
  bad.timestamp = drawDate(r, 2026, 9, 2026, 10);
  bad.cost = r.range(20, 40) + kPassageOverhead;
  bad.relevance = r.real(0.68, 0.74);
  bad.tags = {std::string(t.f[2].key) + ":" + t.wrong3};
  bad.text = "Forum post (unverified): I heard " + std::string(t.f[2].phrase) + " " + t.wrong3 + ".";
  b.add(bad);
  b.distractors(30, 0.20, 0.70, 2024, 6, 2026, 10, kNoticeSources, kNoticeSourceCount);
}

struct MemoryTopic {
  const char *langOld, *langNew, *group, *reminder;
};
const MemoryTopic kMemory[10] = {
    {"Python", "C++", "B2", "Friday 3pm"},      {"Java", "Kotlin", "A1", "Monday 9am"},
    {"C", "Rust", "C3", "Tuesday 11am"},        {"MATLAB", "Python", "B1", "Wednesday 2pm"},
    {"JavaScript", "TypeScript", "A3", "Thursday 4pm"}, {"C++", "Go", "C1", "Saturday 10am"},
    {"Python", "Julia", "B4", "Sunday 6pm"},    {"R", "Python", "A2", "Monday 1pm"},
    {"Go", "C++", "C2", "Friday 9am"},          {"Kotlin", "Swift", "B3", "Tuesday 5pm"},
};

void memoryScenario(Builder& b, const MemoryTopic& t, int day) {
  Rng& r = b.r;
  auto turnTs = [day](int minute) { return ts(2026, 10, day, 9 + minute / 60, minute % 60); };
  auto turn = [&](int minute, const char* source, double lo, double hi, int c0, int c1, const std::string& tag,
                  const std::string& text) {
    Spec s;
    s.source = source;
    s.reliability = std::string(source) == "chat_assistant" ? 'C' : 'B';
    s.timestamp = turnTs(minute);
    s.cost = r.range(c0, c1) + kPassageOverhead;
    s.relevance = r.real(lo, hi);
    if (!tag.empty()) s.tags = {tag};
    s.text = text;
    return b.add(s);
  };
  const std::string langOld = std::string("project_language:") + t.langOld;
  const std::string langNew = std::string("project_language:") + t.langNew;
  const std::string grp = std::string("lab_group:") + t.group;
  const std::string rem = std::string("reminder_time:") + t.reminder;
  turn(6, "chat_user", 0.55, 0.65, 14, 30, grp, std::string("User: I am in lab group ") + t.group + ".");
  const int hOld = turn(12, "chat_user", 0.82, 0.90, 14, 34, langOld,
                        std::string("User: my project will be written in ") + t.langOld + ".");
  const int hEcho = turn(15, "chat_assistant", 0.78, 0.86, 14, 34, langOld,
                         std::string("Assistant: noted, your project language is ") + t.langOld + ".");
  const int gOld = b.newGroup();
  b.specs[hOld].group = b.specs[hEcho].group = gOld;
  // Profile summary written before the change: carries the old language without any supersede link.
  Spec prof;
  prof.source = "profile_memory";
  prof.reliability = 'B';
  prof.timestamp = turnTs(30);
  prof.cost = r.range(40, 70) + kPassageOverhead;
  prof.relevance = r.real(0.78, 0.84);
  prof.tags = {langOld, grp};
  prof.text = std::string("Profile summary: language ") + t.langOld + ", lab group " + t.group + ".";
  b.add(prof);
  turn(54, "chat_user", 0.50, 0.62, 14, 30, rem, std::string("User: remind me on ") + t.reminder + ".");
  const int hNew = turn(90, "chat_user", 0.66, 0.76, 14, 34, langNew,
                        std::string("User: change of plan, I switched the project to ") + t.langNew + ".");
  b.specs[hNew].supersedes = hOld;
  const int hNewEcho = turn(93, "chat_assistant", 0.60, 0.70, 14, 34, langNew,
                            std::string("Assistant: updated, your project language is now ") + t.langNew + ".");
  const int gNew = b.newGroup();
  b.specs[hNew].group = b.specs[hNewEcho].group = gNew;

  // 34 chit-chat turns of low relevance. Scenario `day` puts 3*(day-1) of them AFTER the key
  // turns (0..27 trailing turns), so recency-first is tested from easy to hard.
  static const char* const kChat[] = {"thanks", "ok got it", "what about lunch", "can you explain heaps again",
                                      "nice", "is the lab open", "show me an example", "that helps",
                                      "what is a linked list", "and sorting?"};
  const int trailing = 3 * (day - 1);
  for (int k = 0; k < 34; ++k) {
    const int minute = k < trailing ? 95 + 2 * k : r.range(1, 88);
    const int m = static_cast<int>(b.specs.size());
    const std::string val = "C" + std::to_string(m);
    turn(minute, minute % 2 ? "chat_user" : "chat_assistant", 0.05, 0.40, 10, 60,
         "chat" + std::to_string(m) + ":" + val,
         std::string(minute % 2 ? "User: " : "Assistant: ") + kChat[r.range(0, 9)] + " (" + val + ")");
  }
}

}  // namespace

void generateDataset(const std::string& scenarioPath, const std::string& passagePath, std::uint64_t seed) {
  Rng r(seed);
  std::ofstream sc(scenarioPath), ps(passagePath);
  sc << "# query_id|category|question|ask_keys|gold_evidence_tags   (generated, seed " << seed << ")\n";
  ps << "# query_id|passage_id|source_id|timestamp|token_cost|relevance|reliability|dup_group|evidence_tags|"
        "mandatory|supersedes|text   (generated, seed "
     << seed << "; token_cost includes " << kPassageOverhead << " label/separator tokens)\n";
  int q = 0;
  auto qid = [&q]() {
    char buf[8];
    std::snprintf(buf, sizeof buf, "Q%02d", ++q);
    return std::string(buf);
  };
  for (int i = 0; i < 10; ++i) {
    const PolicyTopic& t = kPolicy[i];
    Builder b{r, qid(), {}, 0};
    policyScenario(b, t, i == 9);
    sc << b.qid << "|policy|" << t.question << '|' << t.key1 << ';' << t.key2 << '|' << t.key1 << ':' << t.newV
       << ';' << t.key2 << ':' << t.val2 << '\n';
    b.write(ps);
  }
  for (int i = 0; i < 10; ++i) {
    const MultiTopic& t = kMulti[i];
    Builder b{r, qid(), {}, 0};
    multiScenario(b, t);
    sc << b.qid << "|multisource|" << t.question << '|';
    for (int k = 0; k < 4; ++k) sc << (k ? ";" : "") << t.f[k].key;
    sc << '|';
    for (int k = 0; k < 4; ++k) sc << (k ? ";" : "") << t.f[k].key << ':' << t.f[k].value;
    sc << '\n';
    b.write(ps);
  }
  for (int i = 0; i < 10; ++i) {
    const MemoryTopic& t = kMemory[i];
    Builder b{r, qid(), {}, 0};
    memoryScenario(b, t, i + 1);
    sc << b.qid
       << "|memory|From our earlier chat: which language is my project in, which lab group am I in, and when is my "
          "reminder?|project_language;lab_group;reminder_time|project_language:"
       << t.langNew << ";lab_group:" << t.group << ";reminder_time:" << t.reminder << '\n';
    b.write(ps);
  }
  const char* const unQ[2] = {"What is the parking permit fee for students?",
                              "What time does the campus gym open on Sundays?"};
  const char* const unK[2] = {"parking_fee", "gym_sunday_hours"};
  for (int i = 0; i < 2; ++i) {
    Builder b{r, qid(), {}, 0};
    b.distractors(30, 0.20, 0.60, 2024, 6, 2026, 10, kNoticeSources, kNoticeSourceCount);
    sc << b.qid << "|unanswerable|" << unQ[i] << '|' << unK[i] << "|-\n";
    b.write(ps);
  }
}

std::vector<Passage> makeScalingWorkload(int n, std::uint64_t seed) {
  Rng r(seed);
  std::vector<Passage> out;
  out.reserve(n);
  const int sources = n / 10 + 1;
  for (int i = 0; i < n; ++i) {
    Passage p;
    p.queryId = "S";
    char buf[24];
    std::snprintf(buf, sizeof buf, "S-%06d", i);
    p.passageId = buf;
    p.sourceId = "src" + std::to_string(r.range(0, sources - 1));
    p.timestamp = drawDate(r, 2024, 1, 2026, 10);
    p.cost = drawCost(r);
    p.relevance = r.real(0.0, 1.0);
    p.reliability = "ABC"[r.range(0, 2)];
    p.dupGroup = (i > 0 && r.chance(0.2)) ? out[r.range(0, i - 1)].dupGroup : p.passageId;
    p.tags = {"k" + std::to_string(r.range(0, n / 2)) + ":v"};
    if (i > 0 && r.chance(0.05)) p.supersedes = out[r.range(0, i - 1)].passageId;
    out.push_back(p);
  }
  return out;
}

ValidationReport validateDataset(const std::vector<Scenario>& scenarios) {
  ValidationReport v;
  for (const Scenario& s : scenarios) {
    ++v.scenarios;
    if (s.category == "policy") ++v.policy;
    else if (s.category == "multisource") ++v.multisource;
    else if (s.category == "memory") ++v.memory;
    else if (s.category == "lookup") ++v.lookup;
    else ++v.unanswerable;
    const std::vector<Passage>& c = s.candidates;
    v.passages += static_cast<int>(c.size());
    v.goldTags += static_cast<int>(s.gold.size());
    HashMap goldSeen(16);
    long long mandatoryCost = 0;
    for (const Passage& p : c) {
      if (p.mandatory) {
        ++v.mandatoryPassages;
        mandatoryCost += p.cost;
      }
      if (!p.tags.empty()) ++v.taggedPassages;
      // Text-vs-label check: every tagged value must be stated in the passage text.
      for (const std::string& t : p.tags)
        if (p.text.find(tagValue(t)) == std::string::npos)
          v.problems.push_back(s.queryId + " " + p.passageId + ": value of tag " + t + " not in text");
      if (!p.supersedes.empty()) {
        ++v.supersedeLinks;
        const Passage* old = nullptr;
        for (const Passage& o : c)
          if (o.passageId == p.supersedes) old = &o;
        if (!old) v.problems.push_back(s.queryId + " " + p.passageId + ": supersedes unknown " + p.supersedes);
        else if (old->timestamp >= p.timestamp)
          v.problems.push_back(s.queryId + " " + p.passageId + ": supersedes a passage that is not older");
      }
    }
    for (const std::string& g : s.gold) {
      if (!goldSeen.insert(g, 1)) v.problems.push_back(s.queryId + ": gold tag listed twice " + g);
      bool askKey = false;
      for (const std::string& k : s.askKeys) askKey = askKey || k == tagKey(g);
      if (!askKey) v.problems.push_back(s.queryId + ": gold key not asked " + g);
      // A gold fact must be stated by at least one passage that nothing supersedes.
      bool current = false;
      for (const Passage& p : c) {
        bool has = false;
        for (const std::string& t : p.tags) has = has || t == g;
        if (!has) continue;
        bool superseded = false;
        for (const Passage& o : c) superseded = superseded || o.supersedes == p.passageId;
        current = current || !superseded;
      }
      if (!current) v.problems.push_back(s.queryId + ": gold tag " + g + " has no current passage");
    }
    if (s.gold.empty()) {
      for (const Passage& p : c)
        for (const std::string& t : p.tags)
          for (const std::string& k : s.askKeys)
            if (tagKey(t) == k) v.problems.push_back(s.queryId + ": unanswerable query has evidence " + t);
    }
    if (mandatoryCost > 256)
      v.notes.push_back(s.queryId + ": mandatory passages cost " + std::to_string(mandatoryCost) +
                        " tokens (infeasible when B_CTX < " + std::to_string(mandatoryCost) + ")");
  }
  return v;
}
