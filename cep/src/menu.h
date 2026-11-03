#pragma once
#include <istream>
#include <ostream>

// Interactive, menu-driven front end for demonstrations (viva): choose a question, budget,
// policy and packaging order, then run one selection or compare all four policies.
// Reads one line per answer from `in`; returns at "0" or end of input.
int runMenu(std::istream& in, std::ostream& out);
