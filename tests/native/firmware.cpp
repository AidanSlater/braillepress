#include "cells.hpp"

#include <stdexcept>

void check(bool passed) {
    if (!passed) throw std::runtime_error("Wrong dry-run reply or counter");
}

int main() {
    using namespace embosser;
    DryRun print;
    check(print.begin(23, 4) == accepted);
    check(print.begin(24, 4) == busy);
    check(print.cell(23, 1, 1) == wrong_sequence);
    check(print.cell(24, 0, 1) == wrong_job);
    check(print.cell(23, 0, 66) == bad_cell);
    check(print.cell(23, 0, 63) == 1);
    check(print.cell(23, 0, 63) == 1);
    check(print.cells() == 1 && print.dots() == 6);
    check(print.finish(23) == incomplete);
    check(print.cell(23, 1, end_line) == 2);
    check(print.cell(23, 2, end_page) == 3);
    check(print.cell(23, 3, 0) == 4);
    check(print.finish(23) == complete);
    check(print.finish(23) == complete && print.begin(23, 4) == complete);
    check(print.cells() == 2 && print.dots() == 6 && print.lines() == 1 && print.page_breaks() == 1);
    check(print.begin(24, 2) == accepted && print.cancel(24) == cancelled);
    check(print.cell(24, 0, 1) == DryRun::cancelled_response && print.finish(24) == cancelled);
}
