#pragma once

#include <cstdint>

namespace embosser {

// Values 0–63 are dot masks. The dry run recognizes line and page boundaries
// in the same stream without requiring a motor, paper, or geometry settings.
constexpr int end_line = 64;
constexpr int end_page = 65;

enum Reply : int {
    accepted = 0, complete = 1, cancelled = 2, busy = -1, wrong_job = -2,
    wrong_sequence = -3, bad_cell = -4, incomplete = -5, invalid_size = -6
};

class DryRun {
public:
    int begin(std::int32_t job, std::int32_t count) {
        if (job <= 0 || count <= 0 || count > 2'000'000) return invalid_size;
        if (job == job_ && state_ != idle) {
            if (count != total_) return invalid_size;
            return state_ == active ? accepted : state_ == finished ? complete : cancelled;
        }
        if (state_ == active) return busy;
        job_ = job;
        total_ = count;
        next_ = 0;
        last_cell_ = -1;
        dots_ = cells_ = lines_ = pages_ = 0;
        state_ = active;
        return accepted;
    }

    // Return the next expected sequence number to acknowledge a cell. Replay of
    // the last request returns the same acknowledgement without counting twice.
    int cell(std::int32_t job, std::int32_t sequence, int operation) {
        if (job != job_ || state_ == idle) return wrong_job;
        if (state_ == stopped) return cancelled_response;
        if (operation < 0 || operation > end_page) return bad_cell;
        if (sequence == next_ - 1 && next_ > 0 && operation == last_cell_) return next_;
        if (state_ != active || sequence != next_ || next_ >= total_) return wrong_sequence;
        if (operation == end_line) ++lines_;
        else if (operation == end_page) ++pages_;
        else {
            ++cells_;
            for (unsigned mask = operation; mask; mask &= mask - 1) ++dots_;
        }
        last_cell_ = operation;
        return ++next_;
    }

    int finish(std::int32_t job) {
        if (job != job_ || state_ == idle) return wrong_job;
        if (state_ == stopped) return cancelled;
        if (next_ != total_) return incomplete;
        state_ = finished;
        return complete;
    }

    int cancel(std::int32_t job) {
        if (job != job_ || state_ == idle) return wrong_job;
        if (state_ == finished) return complete;
        state_ = stopped;
        return cancelled;
    }

    int cells() const { return cells_; }
    int dots() const { return dots_; }
    int lines() const { return lines_; }
    int page_breaks() const { return pages_; }
    static constexpr int cancelled_response = -7;

private:
    enum State { idle, active, finished, stopped } state_ = idle;
    std::int32_t job_ = 0, total_ = 0, next_ = 0;
    int last_cell_ = -1, dots_ = 0, cells_ = 0, lines_ = 0, pages_ = 0;
};

}
