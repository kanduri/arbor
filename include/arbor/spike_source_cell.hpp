#pragma once

#include <arbor/time_sequence.hpp>

namespace arb {

// Cell description returned by recipe::cell_description(gid) for cells with
// recipe::cell_kind(gid) returning cell_kind::spike_source

struct spike_source_cell {
    time_seq seq;
};

} // namespace arb