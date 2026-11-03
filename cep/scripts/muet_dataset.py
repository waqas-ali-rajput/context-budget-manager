"""Converts the MUET document set of the PBL project (../data) into CEP candidate passages.

Input  (repository root, read only): data/passages.txt, data/queries.txt, data/synonyms.txt
Output (cep/):                       data/muet/scenarios.txt, data/muet/passages.txt

For every one of the 20 labelled questions, all 155 MUET passages are candidates, as if a
retriever had scored the whole corpus:
  relevance   BM25 (k1 = 1.2, b = 0.75) over title + text with the PBL synonym list,
              divided by the best score for that question -> [0, 1]
  token_cost  ceil(1.3 x words) + 6 label/separator tokens. ESTIMATE: no tokenizer is installed.
  source_id   the document id (D01..D31); timestamp = document date at 09:00
  reliability A for department documents, B for the department repost D23,
              C for the forum text pasted into D22-P3
  dup_group   D02-Pk and D23-Pk (k = 1..3) are reposts of the same rule
  supersedes  each passage of a revised notice replaces the passage in the same position of the
              original (D09 -> D04, D20 -> D19, D29 -> D28); the documents are marked superseded
  evidence    hand-written key:value facts (FACTS below); each value is copied from the passage
              text. Other passages get one unique fact, so they still count as evidence of
              something, but never of an asked fact.
  gold        the facts stated by the PBL gold passages of each question (labels from
              data/queries.txt, current passages only)
Standard library only. Run from cep/:  python scripts/muet_dataset.py
"""
import math
import os
import re

ROOT = os.path.join("..", "data")
OUT = os.path.join("data", "muet")

FACTS = {
    "D01-P2": "cs251_contents:arrays, linked lists, stacks, queues, hash tables, heaps, trees, graphs",
    "D01-P3": "cs251_final_marks:final examination worth 50 marks",
    "D01-P4": "assignment_late_penalty:10 percent per day",
    "D02-P1": "lab_food_drinks:strictly prohibited",
    "D23-P1": "lab_food_drinks:strictly prohibited",
    "D02-P2": "lab_logout:log out of their workstation",
    "D23-P2": "lab_logout:log out of their workstation",
    "D02-P3": "lab_software:not allowed",
    "D23-P3": "lab_software:not allowed",
    "D03-P2": "exam_phones:not permitted",
    "D04-P1": "attendance_min:70 percent",
    "D04-P2": "attendance_marking:marked by the course teacher",
    "D04-P3": "attendance_shortfall:not allowed to appear in the final examination",
    "D04-P4": "medical_leave:counted as present",
    "D04-P5": "attendance_scope:all undergraduate batches from January 2025",
    "D06-P4": "cs252_final_marks:final examination of CS-252 carries 50 marks",
    "D08-P2": "unfair_means_penalty:zero marks in that paper",
    "D08-P3": "repeated_cheating:expulsion from the university",
    "D09-P1": "attendance_min:75 percent",
    "D09-P2": "attendance_marking:biometric system",
    "D09-P3": "attendance_shortfall:detained and must repeat the course",
    "D09-P4": "medical_leave:within three days with a valid medical certificate",
    "D09-P5": "attendance_condonation:cannot be condoned by the course teacher",
    "D10-P2": "late_fee_fine:100 rupees per day",
    "D10-P3": "merit_scholarship_gpa:3.5 or higher",
    "D12-P3": "lab_report_late_penalty:20 percent of the marks",
    "D14-P2": "library_loan:up to three books for fourteen days",
    "D19-P1": "exam_start:14 December 2026",
    "D19-P2": "cs251_final_date:16 December 2026 at 9 am",
    "D19-P3": "mt201_final_date:18 December 2026 at 9 am",
    "D19-P4": "seating_plan:department notice board",
    "D19-P5": "roll_number_check:one day before each paper",
    "D20-P1": "exam_start:21 December 2026",
    "D20-P2": "cs251_final_date:22 December 2026 at 10 am",
    "D20-P3": "mt201_final_date:24 December 2026 at 10 am",
    "D20-P4": "seating_plan:department notice board and the student portal",
    "D20-P5": "date_sheet_queries:controller of examinations",
    "D25-P1": "dsa_lab_timing:Mondays and Wednesdays from 2 pm to 5 pm",
    "D26-P1": "fyp_group_size:groups of three to four students",
    "D27-P1": "internship_duration:at least six weeks",
    "D28-P1": "registration_close:10 September 2026",
    "D28-P2": "registration_dues:clear all previous dues",
    "D28-P3": "late_registration:15 September 2026",
    "D28-P4": "probation_credit_limit:maximum of 15 credit hours",
    "D28-P5": "add_drop:first week of classes",
    "D29-P1": "registration_close:17 September 2026",
    "D29-P2": "registration_dues:outstanding dues must be cleared",
    "D29-P3": "late_registration:24 September 2026",
    "D29-P4": "probation_credit_limit:maximum of 15 credit hours",
    "D29-P5": "add_drop:first two weeks of classes",
    "D31-P1": "viva_individual:individual questions about their own contribution",
    "D31-P4": "viva_topics:time complexity, data structure choices and test results",
}
DUP_GROUPS = {"D02-P1": "G-FOOD", "D23-P1": "G-FOOD", "D02-P2": "G-LOGOUT", "D23-P2": "G-LOGOUT",
              "D02-P3": "G-SOFTWARE", "D23-P3": "G-SOFTWARE"}
REVISIONS = {"D09": "D04", "D20": "D19", "D29": "D28"}  # revised document -> original
TYPE_TO_CATEGORY = {"keyword": "lookup", "paraphrase": "lookup", "version": "policy",
                    "ambiguous": "multisource", "unanswerable": "unanswerable"}
ASK_KEYS_UNANSWERABLE = {"Q13": "parking_permit", "Q14": "cafeteria_prices", "Q20": "bus_routes"}


def records(path, fields):
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line and not line.startswith("#"):
                parts = line.split("|")
                if len(parts) >= fields:
                    yield parts


def words(text):
    return re.findall(r"[a-z0-9]+", text.lower())


def main():
    passages = [dict(zip(["pid", "doc", "title", "cat", "ver", "date", "status", "text"], r))
                for r in records(os.path.join(ROOT, "passages.txt"), 8)]
    queries = list(records(os.path.join(ROOT, "queries.txt"), 4))
    synonyms = {}
    for r in records(os.path.join(ROOT, "synonyms.txt"), 1):
        if ":" in r[0]:
            k, v = r[0].split(":", 1)
            synonyms[k.strip()] = v.split()

    docs = [words(p["title"] + " " + p["text"]) for p in passages]
    avgdl = sum(len(d) for d in docs) / len(docs)
    df = {}
    for d in docs:
        for w in set(d):
            df[w] = df.get(w, 0) + 1

    def bm25(terms, d):
        score = 0.0
        for w in terms:
            tf = d.count(w)
            if tf:
                idf = math.log(1 + (len(docs) - df[w] + 0.5) / (df[w] + 0.5))
                score += idf * tf * 2.2 / (tf + 1.2 * (0.25 + 0.75 * len(d) / avgdl))
        return score

    def fact_of(p):
        if p["pid"] in FACTS:
            return FACTS[p["pid"]]
        lead = " ".join(p["text"].split()[:4]).rstrip(",;:.")
        return "info_" + p["pid"].lower().replace("-", "") + ":" + lead

    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "scenarios.txt"), "w", encoding="utf-8", newline="\n") as sc, \
         open(os.path.join(OUT, "passages.txt"), "w", encoding="utf-8", newline="\n") as ps:
        sc.write("# MUET document set (data/passages.txt of the PBL project) as CEP questions; "
                 "made by scripts/muet_dataset.py\n# query_id|category|question|ask_keys|gold_evidence_tags\n")
        ps.write("# query_id|passage_id|source_id|timestamp|token_cost|relevance|reliability|dup_group|"
                 "evidence_tags|mandatory|supersedes|text\n")
        for qid, qtype, question, gold_ids in (q[:4] for q in queries):
            gold = []
            for g in gold_ids.split(","):
                g = g.strip()
                if g != "-" and FACTS[g] not in gold:
                    gold.append(FACTS[g])
            keys = []
            for g in gold:
                if g.split(":")[0] not in keys:
                    keys.append(g.split(":")[0])
            if not keys:
                keys = [ASK_KEYS_UNANSWERABLE[qid]]
            mid = "M" + qid[1:]
            sc.write(f"{mid}|{TYPE_TO_CATEGORY[qtype]}|{question}|{';'.join(keys)}|{';'.join(gold) or '-'}\n")

            terms = []
            for w in words(question):
                terms.append(w)
                terms.extend(synonyms.get(w, []))
            scores = [bm25(terms, d) for d in docs]
            best = max(scores) or 1.0
            for p, s in zip(passages, scores):
                doc, k = p["pid"].split("-")
                sup = REVISIONS[doc] + "-" + k if doc in REVISIONS else "-"
                rel = "C" if p["pid"] == "D22-P3" else "B" if doc == "D23" else "A"
                cost = math.ceil(1.3 * len(p["text"].split())) + 6
                ps.write("|".join([mid, p["pid"], doc, p["date"] + "T09:00", str(cost), f"{s / best:.3f}", rel,
                                   DUP_GROUPS.get(p["pid"], "-"), fact_of(p), "0", sup, p["text"]]) + "\n")
    print(f"wrote {OUT}/scenarios.txt ({len(queries)} questions) and {OUT}/passages.txt "
          f"({len(queries) * len(passages)} candidate records)")


if __name__ == "__main__":
    main()
