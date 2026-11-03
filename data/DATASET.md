# Dataset provenance

## Corpus (`passages.txt`)

- **Setting:** the Department of Computer Systems Engineering, Mehran University of Engineering
  and Technology (MUET), Jamshoro, batch 25CS.
- **Synthetic except D24.**
  - The team wrote every rule in D01–D23 and D25–D31 for testing. **None of them is an
    official MUET policy.** The file header says so, and every MUET-context document title
    ends in "(synthetic)", as brief §10 requires.
  - D24 is the only real source: its five passages restate facts from the CS-251 PBL task
    brief (`PBL - DSA.pdf`). Its date is the date the brief was received.
- There are no student records, private identifiers or restricted documents.
- **31 documents, 155 passages, 5 topics:**

| Topic | Documents |
|---|---|
| course | D01 CS-251 outline, D06 CS-252 outline, D11 MT-201 outline, D16 SW-301 outline, **D24 PBL task brief (real)**, **D26 MUET CSE FYP guidelines** |
| lab | D02 computer-lab rules, D07 electronics lab, D12 lab reports, D17 networking lab, D23 department repost of lab rules, **D25 MUET CSE lab timetable for 25CS** |
| exam | D03 exam regulations, D08 unfair means, D13 supplementary exams, D18 grading/GPA, **D31 MUET CSE viva guidelines** |
| notice | D04 attendance 2025 (**superseded**), D09 attendance 2026, D14 library, D19 date sheet original (**superseded**), D20 date sheet revised, D22 societies, **D27 MUET CSE internship policy**, **D28 MUET registration notice original (superseded)**, **D29 MUET registration notice revised**, **D30 MUET CSE department office** |
| admissions | D05 undergraduate admission, D10 fees/scholarships, D15 hostel, D21 migration |

- **Splitting:** each document was written as 5 self-contained statements, and each statement
  is one passage (`Dnn-Pk`). Every passage carries `passage_id`, `document_id`, title, category,
  version, date and status (`current` / `superseded`).
- **Built-in test material:**
  - **Old and new versions:** D04 vs D09 (attendance 70 % → 75 %), D19 vs D20 (exam dates
    moved) and D28 vs D29 (registration deadline 10 → 17 September).
  - **Exact duplicates:** D23-P1/P2 repeat D02-P1/P2 word for word.
  - **Near-duplicate:** D23-P3 vs D02-P3 (unauthorized/unauthorised, lab/laboratory).
  - **Planted injection:** D22-P3 contains a prompt-injection sentence, to show that retrieved
    text is treated as data.
- **Order:** documents are interleaved by topic. The 25-, 50- and 100-passage corpora used in
  the evaluation are the first 25 / 50 / 100 passages of the file, so each already covers at
  least 4 topics. The full corpus has 155 passages.

## Questions (`queries.txt`)

There are 20 questions. Q15–Q20 are about the MUET CSE documents.

| Type | Count | Questions |
|---|---|---|
| Keyword | 8 | Q01–Q05, Q15, Q16, Q19 |
| Paraphrase / synonym | 4 | Q06–Q08, Q17 |
| Ambiguous / version-sensitive | 5 | Q09–Q12, Q18 |
| Unanswerable | 3 | Q13, Q14, Q20 |

**How gold labels were assigned:** every passage judged relevant was labelled by reading the
whole corpus, not by looking at engine output. Gold sets contain only **current** passages;
superseded versions are deliberately left out of them. Unanswerable questions have an empty
gold set (`-`).

**Subset rule:** when a 25/50/100-passage corpus does not contain any gold passage for a question,
that question is reported as `n/a` for Recall@3 at that size and is used for timing only.

**Duplicate rule:** a gold passage also counts as retrieved when its exact duplicate is
returned, because duplicates are folded into one result. This matters for Q01 and Q06.

## Label cross-check (team to complete)

| Question | Labeller | Checker | Disagreement | Resolution |
|---|---|---|---|---|
| Q07 | Basit Azam | Gaffar | Should D08-P1 (definition of unfair means) be gold? | Kept out: it does not say what happens to the student |
| Q11 | Basit Azam | Gaffar | Is D11-P3 (MT-201 assessment) relevant? | Kept out: it gives no marks |
| Q12 | Basit Azam | Gaffar | Is D10-P2 (late *fee*) a "submission penalty"? | Kept out: it is a payment fine |
