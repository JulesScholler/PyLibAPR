

#ifndef PYLIBAPR_PYAPRSEGMENTATION_HPP
#define PYLIBAPR_PYAPRSEGMENTATION_HPP

#include "data_structures/APR/APR.hpp"
#include "data_structures/Mesh/ImagePatch.hpp"
#include "numerics/APRTreeNumerics.hpp"
#include "numerics/APRFilter.hpp"
#include "numerics/APRNumerics.hpp"
#include "algorithm/LocalIntensityScale.hpp"

#include "data_containers/PyAPR.hpp"
#include "data_containers/PyParticleData.hpp"
#include "maxflow-v3.04.src/graph.h"
#include <math.h>
#include <stdlib.h>
#include <algorithm>

namespace py = pybind11;


void get_terminal_energies(PyAPR& apr, PyParticleData<uint16_t>& input_parts, PyParticleData<float>& s, PyParticleData<float>& t,
                           float alpha, int num_tree_smooth = 1, int num_part_smooth = 1, int push_depth = 0, float intensity_threshold=0.0f,
                           float min_var = 0.0f, int std_window_size=7, float max_factor=3.0, int num_levels=3) {

    s.parts.init(apr.total_number_particles());
    t.parts.init(apr.total_number_particles());

    APRTimer timer(true);

    timer.start_timer("compute adaptive min");
    ParticleData<float> loc_min;
    APRNumerics::adaptive_min(apr.apr, input_parts.parts, loc_min, num_tree_smooth, push_depth, num_part_smooth);
    timer.stop_timer();

    timer.start_timer("compute local scale");
    ParticleData<float> local_scale;
    std::vector<int> size = {std_window_size, std_window_size, std_window_size};
    APRNumerics::local_std(apr.apr, input_parts.parts, local_scale, size);
    timer.stop_timer();

    // Loop over particles and edd edges
    auto it = apr.apr.random_iterator();
    auto neighbour_iterator = apr.apr.random_iterator();

    const int level_start = std::min(std::max(apr.level_max()-num_levels, apr.level_min()+1), apr.level_max());

#ifdef PYAPR_HAVE_OPENMP
#pragma omp parallel for schedule(static)
#endif
    for(size_t i = 0; i < it.particles_level_end(level_start-1); ++i) {
        s[i] = 0;
        t[i] = 0;
    }

#ifdef PYAPR_HAVE_OPENMP
#pragma omp parallel for schedule(static)
#endif
    for(size_t i = it.particles_level_begin(level_start); i < it.particles_level_end(apr.level_max()); ++i) {

        float val = input_parts[i];
        float ls = local_scale[i];
        float cap_t, cap_s;
        if(ls <= min_var) {                                             // flat region -> no prior
            cap_s = 0;
            cap_t = 0;
        } else if(val < std::max(intensity_threshold, loc_min[i])) {    // very dim region -> background
            cap_s = 0;
            cap_t = alpha;
        } else if(val > loc_min[i] + max_factor*ls) {                   // very bright region -> foreground
            cap_s = alpha;
            cap_t = 0;
        } else {
            cap_s = alpha * (val - loc_min[i]) / (max_factor*ls);
            cap_t = std::max(alpha - cap_s, 0.0f);
        }

        s[i] = cap_s;
        t[i] = cap_t;
    }
}


template<typename T>
void segment_apr_py(PyAPR& apr, PyParticleData<T>& input_parts, PyParticleData<uint16_t>& mask_parts,
                    float alpha, float beta, float avg_num_neighbours, int num_tree_smooth = 1,
                    int num_part_smooth = 1, int push_depth = 0, float intensity_threshold=0.0f,
                    float min_var = 0.0f, int std_window_size=7, float max_factor=3.0, int num_levels=3) {

    segment_apr_cpp(apr.apr, input_parts.parts, mask_parts.parts, alpha, beta, avg_num_neighbours, num_tree_smooth,
                    num_part_smooth, push_depth, intensity_threshold, min_var, std_window_size, max_factor, num_levels);
}


void err_fn(const char * msg) {
    std::cerr << msg << std::endl;
}


template<typename T>
void segment_apr_cpp(APR& apr, ParticleData<T>& input_parts, ParticleData<uint16_t>& mask_parts,
                     float alpha, float beta, float avg_num_neighbours, int num_tree_smooth = 1,
                     int num_part_smooth = 1, int push_depth = 0, float intensity_threshold=0.0f,
                     float min_var = 0.0f, int std_window_size=7, float max_factor=3.0, int num_levels=3) {

    APRTimer timer(true);

    timer.start_timer("compute adaptive min");
    ParticleData<float> loc_min;
    APRNumerics::adaptive_min(apr, input_parts, loc_min, num_tree_smooth, push_depth, num_part_smooth);
    timer.stop_timer();

    timer.start_timer("compute local scale");
    ParticleData<float> local_scale;
    std::vector<int> window_size = {std_window_size, std_window_size, std_window_size};
    APRNumerics::local_std(apr, input_parts, local_scale, window_size);
    timer.stop_timer();

    // Initialize Graph object
    typedef Graph<float,float,float> GraphType;
    auto *g = new GraphType(apr.total_number_particles(), avg_num_neighbours*apr.total_number_particles(), &err_fn);

    // Add nodes
    timer.start_timer("add nodes");
    g -> add_node(apr.total_number_particles());
    timer.stop_timer();

    // Loop over particles and edd edges
    timer.start_timer("add edges");
    auto it = apr.random_iterator();
    auto neighbour_iterator = apr.random_iterator();

    uint64_t edge_counter = 0;

    const int level_threshold = std::min(std::max(it.level_max()-num_levels, it.level_min()), it.level_max());

    for(int level = it.level_min(); level <= it.level_max(); ++level) {

        const float base_dist = std::pow(2, apr.level_max()-level);

        for(int z = 0; z < apr.z_num(level); ++z) {
            for(int x = 0; x < apr.x_num(level); ++x) {
                for(it.begin(level, z, x); it < it.end(); ++it) {

                    const size_t ct_id = it;
                    const float val = input_parts[ct_id];

                    // Compute terminal edge weights based on intensity
                    float cap_t, cap_s;
                    if(local_scale[ct_id] <= min_var || level <= level_threshold) {     // flat region -> no prior
                        cap_s = 0;
                        cap_t = 0;
                    } else if(val < std::max(intensity_threshold, loc_min[ct_id])) {    // dim region -> background
                        cap_s = 0;
                        cap_t = alpha;
                    } else if(val > loc_min[ct_id] + max_factor*local_scale[ct_id]) {   // bright region -> foreground
                        cap_s = alpha;
                        cap_t = 0;
                    } else {
                        cap_s = alpha * (val - loc_min[ct_id]) / (max_factor*local_scale[ct_id]);
                        cap_t = std::max(alpha - cap_s, 0.0f);
                    }

                    // Add terminal edge weights
                    g -> add_tweights(ct_id, cap_s, cap_t);

                    // Neighbour Particle Cell Face definitions [+y,-y,+x,-x,+z,-z] =  [0,1,2,3,4,5]
                    // Edges are bidirectional, so we only need the positive directions
                    for (int direction = 0; direction < 6; direction+=2) {
                        it.find_neighbours_in_direction(direction);

                        // For each face, there can be 0-4 neighbours
                        for (int index = 0; index < it.number_neighbours_in_direction(direction); ++index) {
                            if (neighbour_iterator.set_neighbour_iterator(it, direction, index)) {
                                //will return true if there is a neighbour defined

                                const size_t neigh_id = neighbour_iterator;
                                const float neigh_val = input_parts[neigh_id];
                                const int neigh_level = neighbour_iterator.level();

                                // in some cases the neighbour iterator and iterator are simply swapped
                                if(neigh_id == ct_id) {
                                    continue;
                                }

                                float particle_distance = base_dist;    //distance if neighbour is same level
                                if(neigh_level > level) {
                                    particle_distance *= 0.75f;         // neighbour particle cell is half the size of current cell
                                } else if(neigh_level < level) {
                                    particle_distance *= 1.5f;          // neighbour particle cell is twice the size of current cell
                                }

                                float diff = (neigh_val - val) / particle_distance;
                                float cost_apr = beta * exp(-diff*diff);

                                g->add_edge(ct_id, neigh_id, cost_apr, cost_apr);

                                edge_counter++;
                            }
                        }
                    }
                }
            }
        }
    }
    timer.stop_timer();

    std::cout << "number of edges = " << edge_counter << " (" << (float)edge_counter/(float)apr.total_number_particles() << " x nparts)" << std::endl;
    // Compute the minimum cut

    timer.start_timer("compute minimum cut");
    g -> maxflow();
    timer.stop_timer();

    // Extract the resulting mask
    timer.start_timer("write output");
    mask_parts.init(apr.total_number_particles());

#ifdef PYAPR_HAVE_OPENMP
#pragma omp parallel for schedule(static)
#endif
    for(size_t idx = 0; idx < mask_parts.size(); ++idx) {
        mask_parts[idx] = 1 * (g->what_segment(idx) == GraphType::SOURCE);
    }
    timer.stop_timer();

    delete g;
}


template<typename T>
void segment_apr_py_tiled(PyAPR& apr, const PyParticleData<T>& input_parts, PyParticleData<uint16_t>& mask_parts,
                          float alpha, float beta, float avg_num_neighbours, int z_block_size, int z_ghost_size,
                          int num_tree_smooth=1, int num_part_smooth=1, int push_depth=0, float intensity_threshold=0.0f,
                          float min_var = 0.0f, int std_window_size=7, float max_factor=3.0, int num_levels=3) {

    APRTimer total_timer(true);
    total_timer.start_timer("Total time");

    APRTimer timer(true);

    timer.start_timer("compute adaptive min");
    ParticleData<float> loc_min;
    APRNumerics::adaptive_min(apr.apr, input_parts.parts, loc_min, num_tree_smooth, push_depth, num_part_smooth);
    timer.stop_timer();

    timer.start_timer("compute local scale");
    ParticleData<float> local_scale;
    std::vector<int> window_size = {std_window_size, std_window_size, std_window_size};
    APRNumerics::local_std(apr.apr, input_parts.parts, local_scale, window_size);
    timer.stop_timer();

    mask_parts.parts.init(apr.total_number_particles()); // initialize output particles

    const int y_num = apr.apr.org_dims(0);
    const int x_num = apr.apr.org_dims(1);
    const int z_num = apr.apr.org_dims(2);

    const int number_z_blocks = (z_num + z_block_size - 1) / z_block_size;

    for (int block = 0; block < number_z_blocks; ++block) {
        timer.start_timer("segment block " + std::to_string(block+1));
        std::cout << "Block " << block + 1 << " / " << number_z_blocks << std::endl;
        int z_0 = block * z_block_size;
        int z_f = (block == (number_z_blocks - 1)) ? z_num : (block + 1) * z_block_size;

        int z_ghost_l = std::min(z_0, z_ghost_size);
        int z_ghost_r = std::min(z_num - z_f, z_ghost_size);

        ImagePatch patch;
        initPatchGlobal(patch, z_0 - z_ghost_l, z_f + z_ghost_r, 0, x_num, 0, y_num);

        patch.z_ghost_l = z_ghost_l;
        patch.z_ghost_r = z_ghost_r;
        patch.z_offset = z_0 - z_ghost_l;

        segment_apr_block(apr.apr, input_parts.parts, mask_parts.parts, alpha, beta, avg_num_neighbours,
                          patch, loc_min, local_scale, intensity_threshold, min_var, max_factor, num_levels);
        timer.stop_timer();
    }
    total_timer.stop_timer();
}


uint64_t number_particles_in_block(APR& apr, const int z_begin, const int z_end) {
    auto it = apr.iterator();
    uint64_t count = 0;

    for(int level = it.level_min(); level <= it.level_max(); ++level) {

        const float level_size = apr.level_size(level);
        const int z_begin_l = std::floor((float)z_begin / level_size);
        const int z_end_l = std::ceil((float)z_end / level_size);

        const int z_num = it.z_num(level);
        const int x_num = it.x_num(level);

        uint64_t first = it.begin(level, z_begin_l, 0);

        uint64_t last;
        if(z_end_l < z_num) {
            last = it.begin(level, z_end_l+1, 0);
        } else {
            it.begin(level, z_num-1, x_num-1);
            last = it.end();
        }
        count += (last-first);
    }
    return count;
}


void compute_offset_per_level(APR& apr, std::vector<uint64_t>& offsets, const int z_begin, const int z_end) {
    offsets.resize(apr.level_max()+1, 0);
    auto it = apr.iterator();
    uint64_t cumsum = 0;
    for(int level = it.level_min(); level <= it.level_max(); ++level) {

        const float level_size = apr.level_size(level);
        const int z_begin_l = std::floor((float)z_begin / level_size);
        const int z_end_l = std::ceil((float)z_end / level_size);

        const int z_num = it.z_num(level);
        const int x_num = it.x_num(level);

        uint64_t first = it.begin(level, z_begin_l, 0);
        offsets[level] = first - cumsum;

        uint64_t last;
        if(z_end_l < z_num) {
            last = it.begin(level, z_end_l+1, 0);
        } else {
            it.begin(level, z_num-1, x_num-1);
            last = it.end();
        }
        cumsum += (last-first);
    }
}


template<typename T>
void segment_apr_block(APR& apr, const ParticleData<T>& input_parts, ParticleData<uint16_t>& mask_parts,
                       const float alpha, const float beta, float avg_num_neighbours, const ImagePatch& patch, const ParticleData<float>& loc_min,
                       const ParticleData<float>& local_scale, const float intensity_threshold, const float min_var, const float max_factor,
                       const int num_levels) {

    APRTimer timer(true);

    uint64_t num_parts = number_particles_in_block(apr, patch.z_begin_global, patch.z_end_global) + 1;

    std::vector<uint64_t> offset_ghost;
    compute_offset_per_level(apr, offset_ghost, patch.z_begin_global, patch.z_end_global);

    std::vector<int> z_begin(apr.level_max() + 1, 0);
    std::vector<int> z_end(apr.level_max() + 1, 0);
    for(int level = apr.level_max(); level > apr.level_min(); --level) {
        const float level_size = apr.level_size(level);
        z_begin[level] = std::floor(patch.z_begin_global / level_size);
        z_end[level] = std::ceil(patch.z_end_global / level_size);
    }

    // Initialize Graph object
    typedef Graph<float,float,float> GraphType;
    auto *g = new GraphType(num_parts, avg_num_neighbours*num_parts, &err_fn);

    // Add nodes
    timer.start_timer("add nodes");
    g -> add_node(num_parts);
    timer.stop_timer();

    // Loop over particles and edd edges
    timer.start_timer("add edges");
    auto it = apr.random_iterator();
    auto neighbour_iterator = apr.random_iterator();

    const int level_threshold = std::min(std::max(it.level_max()-num_levels, it.level_min()), it.level_max());

    for(int level = apr.level_min(); level <= apr.level_max(); ++level) {
        for(int z = z_begin[level]; z < z_end[level]; ++z) {
            for(int x = 0; x < it.x_num(level); ++x) {
                for(it.begin(level, z, x); it < it.end(); ++it) {

                    const uint64_t ct_id = it;
                    const float val = input_parts[ct_id];

                    // Compute terminal edge weights based on intensity
                    float cap_t, cap_s;
                    if(local_scale[ct_id] <= min_var || level <= level_threshold) {     // flat region -> no prior
                        cap_s = 0;
                        cap_t = 0;
                    } else if(val < std::max(intensity_threshold, loc_min[ct_id])) {    // dim region -> background
                        cap_s = 0;
                        cap_t = alpha;
                    } else if(val > loc_min[ct_id] + max_factor*local_scale[ct_id]) {   // bright region -> foreground
                        cap_s = alpha;
                        cap_t = 0;
                    } else {
                        cap_s = alpha * (val - loc_min[ct_id]) / (max_factor*local_scale[ct_id]);
                        cap_t = std::max(alpha - cap_s, 0.0f);
                    }

                    // Add terminal edge weights
                    g -> add_tweights(ct_id - offset_ghost[level], cap_s, cap_t);

                    // Neighbour Particle Cell Face definitions [+y,-y,+x,-x,+z,-z] =  [0,1,2,3,4,5]
                    // Edges are bidirectional, so we only need the positive directions
                    for (int direction = 0; direction < 6; direction+=2) {
                        it.find_neighbours_in_direction(direction);

                        // For each face, there can be 0-4 neighbours
                        for (int index = 0; index < it.number_neighbours_in_direction(direction); ++index) {
                            if (neighbour_iterator.set_neighbour_iterator(it, direction, index)) {
                                //will return true if there is a neighbour defined
                                int neigh_level = neighbour_iterator.level();
                                float neigh_val = input_parts[neighbour_iterator];
                                uint64_t neigh_id = neighbour_iterator;

                                // ignore neighbour particle if it is out of block bounds
                                if(neighbour_iterator.z() < z_begin[neigh_level] || neighbour_iterator.z() >= z_end[neigh_level]) {
                                    continue;
                                }

                                // in some cases the neighbour iterator and iterator are simply swapped
                                if(neigh_id == ct_id) {
                                    continue;
                                }

                                float particle_distance = apr.level_size(level);
                                if(neigh_level > level) {
                                    particle_distance *= 0.75f;   // neighbour particle cell is half the size of current cell
                                } else if(neigh_level < level) {
                                    particle_distance *= 1.5f;    // neighbour particle cell is twice the size of current cell
                                }

                                const float diff = (neigh_val - val) / particle_distance;
                                const float cost_apr = beta * exp(-diff*diff);

                                g->add_edge(ct_id - offset_ghost[level], neighbour_iterator - offset_ghost[neigh_level], cost_apr, cost_apr);
                            }
                        }
                    }
                }
            }
        }
    }
    timer.stop_timer();

    // Compute the minimum cut
    timer.start_timer("compute minimum cut");
    g -> maxflow();
    timer.stop_timer();

    // Extract the resulting mask
    timer.start_timer("write output");

    for(int level = apr.level_max(); level > apr.level_min(); --level) {

        const float level_size = apr.level_size(level);

        const int z_ghost_l = std::floor(patch.z_ghost_l / level_size);
        const int z_ghost_r = std::floor(patch.z_ghost_r / level_size);

        const int z_begin_l = std::floor((patch.z_begin_global + patch.z_ghost_l) / level_size) - z_ghost_l;
        const int z_end_l = std::ceil((patch.z_end_global - patch.z_ghost_r) / level_size) + z_ghost_r;

        const uint64_t offset_in = offset_ghost[level];
//        const uint64_t offset_out = offset_interior[level];

#ifdef PYAPR_HAVE_OPENMP
#pragma omp parallel for schedule(dynamic) firstprivate(it)
#endif
        for(int z = z_begin_l; z < z_end_l; ++z) {
            for(int x = 0; x < it.x_num(level); ++x) {
                for(it.begin(level, z, x); it < it.end(); ++it) {
                    uint64_t graph_idx = it - offset_in;
                    mask_parts[it] = 1 * (g->what_segment(graph_idx) == GraphType::SOURCE);
                }
            }
        }

    }
    timer.stop_timer();

    delete g;
}


void AddPyAPRSegmentation(py::module &m, const std::string &modulename) {

    auto m2 = m.def_submodule(modulename.c_str());
    m2.def("graphcut", &segment_apr_py<float>, "compute graphcut segmentation of an APR",
           py::arg("apr"), py::arg("input_parts"), py::arg("mask_parts"), py::arg("alpha")=1, py::arg("beta")=1,
           py::arg("avg_num_neighbours")=3.3, py::arg("num_tree_smooth")=1, py::arg("num_part_smooth")=1,
           py::arg("push_depth")=0, py::arg("intensity_threshold")=0.0f, py::arg("min_var")=0.0f,
           py::arg("std_window_size")=7, py::arg("max_factor")=3.0, py::arg("num_levels")=2);
    m2.def("graphcut", &segment_apr_py<uint16_t>, "compute graphcut segmentation of an APR",
           py::arg("apr"), py::arg("input_parts"), py::arg("mask_parts"), py::arg("alpha")=1, py::arg("beta")=1,
           py::arg("avg_num_neighbours")=3.3, py::arg("num_tree_smooth")=1, py::arg("num_part_smooth")=1,
           py::arg("push_depth")=0, py::arg("intensity_threshold")=0.0f, py::arg("min_var")=0.0f,
           py::arg("std_window_size")=7, py::arg("max_factor")=3.0, py::arg("num_levels")=2);

    m2.def("graphcut_tiled", &segment_apr_py_tiled<uint16_t>, "compute graphcut segmentation of an APR",
           py::arg("apr"), py::arg("input_parts"), py::arg("mask_parts"), py::arg("alpha")=1.0, py::arg("beta")=1.0,
           py::arg("avg_num_neighbours")=3.3, py::arg("z_block_size")=256, py::arg("z_ghost_size")=16,
           py::arg("num_tree_smooth")=1, py::arg("num_part_smooth")=1, py::arg("push_depth")=0, py::arg("intensity_threshold")=0.0f,
           py::arg("min_var")=0.0f, py::arg("std_window_size")=5, py::arg("max_factor")=3.0, py::arg("num_levels")=2);
    m2.def("graphcut_tiled", &segment_apr_py_tiled<uint16_t>, "compute graphcut segmentation of an APR",
           py::arg("apr"), py::arg("input_parts"), py::arg("mask_parts"), py::arg("alpha")=1, py::arg("beta")=1,
           py::arg("avg_num_neighbours")=3.3, py::arg("z_block_size")=256, py::arg("z_ghost_size")=16,
           py::arg("num_tree_smooth")=1, py::arg("num_part_smooth")=1, py::arg("push_depth")=0, py::arg("intensity_threshold")=0.0f,
           py::arg("min_var")=0.0f, py::arg("std_window_size")=5, py::arg("max_factor")=3.0, py::arg("num_levels")=2);

    m2.def("get_terminal_energies", &get_terminal_energies, "Compute terminal edges (useful for debugging or fine-tuning)");
}

#endif //PYLIBAPR_PYAPRSEGMENTATION_HPP
