#include <iostream>
#include <fstream>
#include <sstream>

#include "catypes.hpp"
#include "cell.hpp"
#include "cell_group.hpp"
#include "fvm_cell.hpp"
#include "mechanism_catalogue.hpp"
#include "threading/threading.hpp"
#include "profiling/profiler.hpp"
#include "communication/communicator.hpp"
#include "communication/global_policy.hpp"
#include "util/optional.hpp"

#include "io.hpp"

using namespace nest;

using real_type = double;
using index_type = mc::cell_gid_type;
using id_type = mc::cell_gid_type;
using numeric_cell = mc::fvm::fvm_cell<real_type, index_type>;
using cell_group   = mc::cell_group<numeric_cell>;

using global_policy = nest::mc::communication::global_policy;
using communicator_type =
    mc::communication::communicator<global_policy>;

using nest::mc::util::optional;

struct model {
    communicator_type communicator;
    std::vector<cell_group> cell_groups;

    unsigned num_groups() const {
        return cell_groups.size();
    }

    void run(double tfinal, double dt) {
        auto t = 0.;
        auto delta = std::min(double(communicator.min_delay()), tfinal);
        while (t<tfinal) {
            mc::threading::parallel_for::apply(
                0, num_groups(),
                [&](int i) {
                        mc::util::profiler_enter("stepping","events");
                    cell_groups[i].enqueue_events(communicator.queue(i));
                        mc::util::profiler_leave();

                    cell_groups[i].advance(std::min(t+delta, tfinal), dt);

                        mc::util::profiler_enter("events");
                    communicator.add_spikes(cell_groups[i].spikes());
                    cell_groups[i].clear_spikes();
                        mc::util::profiler_leave(2);
                }
            );

                mc::util::profiler_enter("stepping", "exchange");
            communicator.exchange();
                mc::util::profiler_leave(2);

            t += delta;
        }
    }

    void init_communicator() {
            mc::util::profiler_enter("setup", "communicator");

        // calculate the source and synapse distribution serially
        std::vector<id_type> target_counts(num_groups());
        std::vector<id_type> source_counts(num_groups());
        for (auto i=0u; i<num_groups(); ++i) {
            target_counts[i] = cell_groups[i].cell().synapses()->size();
            source_counts[i] = cell_groups[i].spike_sources().size();
        }

        target_map = mc::algorithms::make_index(target_counts);
        source_map = mc::algorithms::make_index(source_counts);

        //  create connections
        communicator = communicator_type(num_groups(), target_counts);

            mc::util::profiler_leave(2);
    }

    void update_gids() {
            mc::util::profiler_enter("setup", "globalize");
        auto com_policy = communicator.communication_policy();
        auto global_source_map = com_policy.make_map(source_map.back());
        auto domain_idx = communicator.domain_id();
        for (auto i=0u; i<num_groups(); ++i) {
            cell_groups[i].set_source_gids(
                source_map[i]+global_source_map[domain_idx]
            );
            cell_groups[i].set_target_gids(
                target_map[i]+communicator.target_gid_from_group_lid(0)
            );
        }
            mc::util::profiler_leave(2);
    }

    // TODO : only stored here because init_communicator() and update_gids() are split
    std::vector<id_type> source_map;
    std::vector<id_type> target_map;

    // traces from probes
    struct trace_data {
        struct sample_type {
            float time;
            double value;
        };
        std::string name;
        std::string units;
        mc::cell_gid_type id;
        std::vector<sample_type> samples;
    };

    // different traces may be written to by different threads;
    // during simulation, each trace_sampler will be responsible for its
    // corresponding element in the traces vector.

    std::vector<trace_data> traces;

    // make a sampler that records to traces
    struct simple_sampler_functor {
        std::vector<trace_data> &traces_;
        size_t trace_index_ = 0;
        float requested_sample_time_ = 0;
        float dt_ = 0;

        simple_sampler_functor(std::vector<trace_data> &traces, size_t index, float dt) :
            traces_(traces), trace_index_(index), dt_(dt)
        {}

        optional<float> operator()(float t, double v) {
            traces_[trace_index_].samples.push_back({t,v});
            return requested_sample_time_ += dt_;
        }
    };

    mc::sampler make_simple_sampler(
        mc::cell_member_type probe_id, const std::string& name, const std::string& units, float dt)
    {
        traces.push_back(trace_data{name, units, probe_id.gid});
        return {probe_id, simple_sampler_functor(traces, traces.size()-1, dt)};
    }

    void reset_traces() {
        // do not call during simulation: thread-unsafe access to traces.
        traces.clear();
    }

    void dump_traces() {
        // do not call during simulation: thread-unsafe access to traces.
        for (const auto& trace: traces) {
            auto path = "trace_" + std::to_string(trace.id)
                      + "_" + trace.name + ".json";

            nlohmann::json jrep;
            jrep["name"] = trace.name;
            jrep["units"] = trace.units;
            jrep["id"] = trace.id;

            auto& jt = jrep["data"]["time"];
            auto& jy = jrep["data"][trace.name];

            for (const auto& sample: trace.samples) {
                jt.push_back(sample.time);
                jy.push_back(sample.value);
            }
            std::ofstream file(path);
            file << std::setw(1) << jrep << std::endl;
        }
    }
};

// define some global model parameters
namespace parameters {
namespace synapses {
    // synapse delay
    constexpr float delay  = 20.0;  // ms

    // connection weight
    constexpr double weight_per_cell = 0.3;  // uS
}
}

///////////////////////////////////////
// prototypes
///////////////////////////////////////

/// make a single abstract cell
mc::cell make_cell(int compartments_per_segment, int num_synapses, const std::string& syn_type);

/// do basic setup (initialize global state, print banner, etc)
void setup();

/// helper function for initializing cells
cell_group make_lowered_cell(int cell_index, const mc::cell& c);

/// models
void all_to_all_model(nest::mc::io::cl_options& opt, model& m);

///////////////////////////////////////
// main
///////////////////////////////////////
int main(int argc, char** argv) {
    nest::mc::communication::global_policy_guard global_guard(argc, argv);

    setup();

    // read parameters
    mc::io::cl_options options;
    try {
        options = mc::io::read_options(argc, argv);
        if (global_policy::id()==0) {
            std::cout << options << "\n";
        }
    }
    catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    }

    model m;
    all_to_all_model(options, m);

    //
    //  time stepping
    //
    auto tfinal = options.tfinal;
    auto dt     = options.dt;

    auto id = m.communicator.domain_id();

    if (id==0) {
        // use std::endl to force flush of output on cluster jobs
        std::cout << "\n";
        std::cout << ":: simulation to " << tfinal << " ms in "
                  << std::ceil(tfinal / dt) << " steps of "
                  << dt << " ms" << std::endl;
    }

    // add some spikes to the system to start it
    auto first = m.communicator.group_gid_first(id);
    if(first%20) {
        first += 20 - (first%20); // round up to multiple of 20
    }
    auto last  = m.communicator.group_gid_first(id+1);
    for (auto i=first; i<last; i+=20) {
        m.communicator.add_spike({i, 0});
    }

    m.run(tfinal, dt);

    mc::util::profiler_output(0.001);

    if (id==0) {
        std::cout << "there were " << m.communicator.num_spikes() << " spikes\n";
    }

    m.dump_traces();
}

///////////////////////////////////////
// models
///////////////////////////////////////

void all_to_all_model(nest::mc::io::cl_options& options, model& m) {
    //
    //  make cells
    //

    auto synapses_per_cell = options.synapses_per_cell;
    auto is_all_to_all = options.all_to_all;

    // make a basic cell
    auto basic_cell =
        make_cell(options.compartments_per_segment, synapses_per_cell, options.syn_type);

    auto num_domains = global_policy::size();
    auto domain_id = global_policy::id();

    // make a vector for storing all of the cells
    id_type ncell_global = options.cells;
    id_type ncell_local  = ncell_global / num_domains;
    int remainder = ncell_global - (ncell_local*num_domains);
    if (domain_id<remainder) {
        ncell_local++;
    }

    m.cell_groups = std::vector<cell_group>(ncell_local);

    // initialize the cells in parallel
    mc::threading::parallel_for::apply(
        0, ncell_local,
        [&](int i) {
                mc::util::profiler_enter("setup", "cells");
            m.cell_groups[i] = make_lowered_cell(i, basic_cell);
                mc::util::profiler_leave(2);
        }
    );

    //
    //  network creation
    //
    m.init_communicator();

        mc::util::profiler_enter("setup", "connections");

    // RNG distributions for connection delays and source cell ids
    auto weight_distribution = std::exponential_distribution<float>(0.75);
    auto source_distribution =
        std::uniform_int_distribution<uint32_t>(0u, options.cells-1);

    // calculate the weight of synaptic connections, which is chosen so that
    // the sum of all synaptic weights on a cell is
    // parameters::synapses::weight_per_cell
    float weight = parameters::synapses::weight_per_cell / synapses_per_cell;

    // loop over each local cell and build the list of synapse connections
    // that terminate on the cell
    for (auto lid=0u; lid<ncell_local; ++lid) {
        auto target = m.communicator.target_gid_from_group_lid(lid);
        auto gid = m.communicator.group_gid_from_group_lid(lid);
        auto gen = std::mt19937(gid);  // seed with cell gid for reproducability
        // add synapses to cell
        auto i = 0u;
        auto cells_added = 0u;
        while (cells_added < synapses_per_cell) {
            auto source = is_all_to_all ?  i : source_distribution(gen);
            if (gid!=source) {
                m.communicator.add_connection({
                    source, target++, weight,
                    parameters::synapses::delay + weight_distribution(gen)
                });
                cells_added++;
            }
            ++i;
        }
    }

    m.communicator.construct();

    //for (auto con : m.communicator.connections()) std::cout << con << "\n";

    m.update_gids();

    //
    //  setup probes
    //

    mc::util::profiler_leave();
    mc::util::profiler_enter("probes");

    // monitor soma and dendrite on a few cells
    float sample_dt = 0.1;
    index_type monitor_group_gids[] = { 0, 1, 2 };
    for (auto gid : monitor_group_gids) {
        if (!m.communicator.is_local_group(gid)) {
            continue;
        }

        auto lid = m.communicator.group_lid(gid);
        mc::cell_member_type probe_soma = {gid, m.cell_groups[lid].probe_gid_range().first};
        mc::cell_member_type probe_dend = {gid, probe_soma.index+1};
        mc::cell_member_type probe_dend_current = {gid, probe_soma.index+2};

        m.cell_groups[lid].add_sampler(
            m.make_simple_sampler(probe_soma, "vsoma", "mV", sample_dt)
        );
        m.cell_groups[lid].add_sampler(
            m.make_simple_sampler(probe_dend, "vdend", "mV", sample_dt)
        );
        m.cell_groups[lid].add_sampler(
            m.make_simple_sampler(probe_dend_current, "idend", "mA/cm²", sample_dt)
        );
    }

        mc::util::profiler_leave(2);
}

///////////////////////////////////////
// function definitions
///////////////////////////////////////

void setup() {
    // print banner
    if (global_policy::id()==0) {
        std::cout << "====================\n";
        std::cout << "  starting miniapp\n";
        std::cout << "  - " << mc::threading::description() << " threading support\n";
        std::cout << "  - communication policy: " << global_policy::name() << "\n";
        std::cout << "====================\n";
    }
}

// make a high level cell description for use in simulation
mc::cell make_cell(int compartments_per_segment, int num_synapses, const std::string& syn_type) {
    nest::mc::cell cell;

    // Soma with diameter 12.6157 um and HH channel
    auto soma = cell.add_soma(12.6157/2.0);
    soma->add_mechanism(mc::hh_parameters());

    // add dendrite of length 200 um and diameter 1 um with passive channel
    std::vector<mc::cable_segment*> dendrites;
    dendrites.push_back(cell.add_cable(0, mc::segmentKind::dendrite, 0.5, 0.5, 200));
    dendrites.push_back(cell.add_cable(1, mc::segmentKind::dendrite, 0.5, 0.25,100));
    dendrites.push_back(cell.add_cable(1, mc::segmentKind::dendrite, 0.5, 0.25,100));

    for (auto d : dendrites) {
        d->add_mechanism(mc::pas_parameters());
        d->set_compartments(compartments_per_segment);
        d->mechanism("membrane").set("r_L", 100);
    }

    cell.add_detector({0,0}, 20);

    auto gen = std::mt19937();
    auto distribution = std::uniform_real_distribution<float>(0.f, 1.0f);
    // distribute the synapses at random locations the terminal dendrites in a
    // round robin manner
    nest::mc::parameter_list syn_default(syn_type);
    for (auto i=0; i<num_synapses; ++i) {
        cell.add_synapse({2+(i%2), distribution(gen)}, syn_default);
    }

    // add probes: 
    auto probe_soma = cell.add_probe({0, 0}, mc::probeKind::membrane_voltage);
    auto probe_dendrite = cell.add_probe({1, 0.5}, mc::probeKind::membrane_voltage);
    auto probe_dendrite_current = cell.add_probe({1, 0.5}, mc::probeKind::membrane_current);

    EXPECTS(probe_soma==0);
    EXPECTS(probe_dendrite==1);
    EXPECTS(probe_dendrite_current==2);
    (void)probe_soma, (void)probe_dendrite, (void)probe_dendrite_current;

    return cell;
}

cell_group make_lowered_cell(int cell_index, const mc::cell& c) {
    return cell_group(c);
}
