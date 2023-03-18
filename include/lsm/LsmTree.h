#pragma once

#include <atomic>
#include <numeric>
#include <cstdio>

#include "lsm/MemTable.h"
#include "lsm/MemoryLevel.h"
#include "ds/Alias.h"

#include "util/timer.h"

namespace lsm {

thread_local size_t sampling_attempts = 0;
thread_local size_t sampling_rejections = 0;
thread_local size_t deletion_rejections = 0;
thread_local size_t bounds_rejections = 0;
thread_local size_t tombstone_rejections = 0;
thread_local size_t memtable_rejections = 0;

/*
 * thread_local size_t various_sampling_times go here.
 */
thread_local size_t sample_range_time = 0;
thread_local size_t alias_time = 0;
thread_local size_t alias_query_time = 0;
thread_local size_t rejection_check_time = 0;
thread_local size_t memtable_sample_time = 0;
thread_local size_t memlevel_sample_time = 0;
thread_local size_t disklevel_sample_time = 0;
thread_local size_t sampling_bailouts = 0;


/*
 * LSM Tree configuration global variables
 */

// True for memtable rejection sampling
static constexpr bool LSM_REJ_SAMPLE = false;

// True for leveling, false for tiering
static constexpr bool LSM_LEVELING = true;

typedef ssize_t level_index;

class LSMTree;
struct sample_state {
    LSMTree *tree; 
    RunId rid;
    char *buff;
    MemTable *memtable;
    size_t mtable_cutoff;
};


class LSMTree {
    friend bool check_deleted();

public:
    LSMTree(std::string root_dir, size_t memtable_cap, size_t memtable_tombstone_cap, size_t scale_factor, size_t memory_levels,
            double max_tombstone_prop, double max_rejection_prop, std::string meta_fname, gsl_rng *rng) 
        : active_memtable(0), //memory_levels(memory_levels, 0),
          scale_factor(scale_factor), 
          max_tombstone_prop(max_tombstone_prop),
          max_rejections_per_tombstone(max_rejection_prop),
          root_directory(root_dir),
          last_level_idx(-1),
          memory_level_cnt(memory_levels),
          memtable_1(new MemTable(memtable_cap, LSM_REJ_SAMPLE, memtable_tombstone_cap, rng)), 
          memtable_2(new MemTable(memtable_cap, LSM_REJ_SAMPLE, memtable_tombstone_cap, rng)),
          memtable_1_merging(false), memtable_2_merging(false) {

        size_t run_cap =  (LSM_LEVELING) ? 1 : scale_factor;

        FILE *meta_f = fopen(meta_fname.c_str(), "r");
        assert(meta_f);

        char fbuf[1028];
        size_t idx = 0;
        while (fscanf(meta_f, "%s\n", fbuf) != EOF) {
            bool disk;
            level_index l_idx = this->decode_level_index(idx);

            if (disk) {
                assert(false); // not implemented
                //this->disk_levels.emplace_back(new DiskLevel(idx, run_cap, root_directory, fbuf, rng));
            } else {
                this->memory_levels.emplace_back(new MemoryLevel(idx, run_cap, root_directory, fbuf, rng));
            }

            idx++;
        }

    }


    LSMTree(std::string root_dir, size_t memtable_cap, size_t memtable_tombstone_cap, size_t scale_factor, size_t memory_levels,
            double max_tombstone_prop, double max_rejection_prop, gsl_rng *rng) 
        : active_memtable(0), //memory_levels(memory_levels, 0),
          scale_factor(scale_factor), 
          max_tombstone_prop(max_tombstone_prop),
          max_rejections_per_tombstone(max_rejection_prop),
          root_directory(root_dir),
          last_level_idx(-1),
          memory_level_cnt(memory_levels),
          memtable_1(new MemTable(memtable_cap, LSM_REJ_SAMPLE, memtable_tombstone_cap, rng)), 
          memtable_2(new MemTable(memtable_cap, LSM_REJ_SAMPLE, memtable_tombstone_cap, rng)),
          memtable_1_merging(false), memtable_2_merging(false) {}

    ~LSMTree() {
        delete this->memtable_1;
        delete this->memtable_2;

        for (size_t i=0; i<this->memory_levels.size(); i++) {
            delete this->memory_levels[i];
        }
    }

    int append(const key_t& key, const value_t& val, double weight, bool tombstone, gsl_rng *rng) {
        // NOTE: single-threaded implementation only
        MemTable *mtable;
        while (!(mtable = this->memtable()))
            ;
        
        if (mtable->is_full()) {
            this->merge_memtable(rng);
        }

        return mtable->append(key, val, weight, tombstone);
    }

    void range_sample(record_t *sample_set, const key_t& lower_key, const key_t& upper_key, size_t sample_sz, char *buffer, char *utility_buffer, gsl_rng *rng) {
        // TODO: Only working for in-memory sampling, as WIRS_ISAMTree isn't implemented.

        auto mtable = this->memtable();
        Alias *memtable_alias = nullptr;
        std::vector<record_t *> memtable_records;
        size_t mtable_cutoff = 0;

        double memtable_weight;

        if (LSM_REJ_SAMPLE) {
            memtable_weight = mtable->get_total_weight(); 
            mtable_cutoff = mtable->get_record_count() - 1;
        } else {
            memtable_weight = mtable->get_sample_range(lower_key, upper_key, memtable_records, &memtable_alias, &mtable_cutoff);
        }

        // Get the run weights for each level. Index 0 is the memtable,
        // represented by nullptr.
        std::vector<std::pair<RunId, WIRSRun *>> runs;
        std::vector<WIRSRunState*> states;
        runs.push_back({{-1, -1}, nullptr});
        states.push_back(nullptr);

        std::vector<double> run_weights;
        run_weights.push_back(memtable_weight);

        for (auto &level : this->memory_levels) {
            level->get_run_weights(run_weights, runs, states, lower_key, upper_key);
        }

        if (run_weights.size() == 1 && run_weights[0] == 0) {
            if (memtable_alias) delete memtable_alias;
            for (auto& x: states) delete x;
            sampling_bailouts++;
            return; // no records in the sampling range
        }

        double tot_weight = std::accumulate(run_weights.begin(), run_weights.end(), 0);
        for (auto& w: run_weights) w /= tot_weight;

        // Construct alias structure
        auto alias = Alias(run_weights);

        std::vector<size_t> run_samples(run_weights.size(), 0);

        size_t rejections = sample_sz;
        size_t sample_idx = 0;

        sample_state state;
        state.tree = this;
        state.buff = buffer;
        state.memtable = mtable;
        state.mtable_cutoff = mtable_cutoff;

        size_t memtable_rejections = 0;

        do {
            for (size_t i=0; i<rejections; i++) {
                run_samples[alias.get(rng)] += 1;
            }

            rejections = 0;

            while (run_samples[0] > 0) {
                const record_t *rec;
                if (LSM_REJ_SAMPLE) {
                    rec = mtable->get_sample(lower_key, upper_key, rng);
                } else {
                    rec = memtable_records[memtable_alias->get(rng)];
                }

                if (LSM_REJ_SAMPLE && !rec) {
                    memtable_rejections++;
                    rejections++;
                } else if (mtable->check_tombstone(rec->key, rec->value)) {
                    tombstone_rejections++;
                    rejections++;
                } else {
                    sample_set[sample_idx++] = *rec;
                }

                run_samples[0]--;

                // Assume nothing in memtable and bail out.
                // FIXME: rather than a bailout, we could switch to non-rejection 
                // sampling, but that would require rebuilding the full alias structure. 
                // Wouldn't be too hard to do, but for the moment I'll just do this.
                if (LSM_REJ_SAMPLE && memtable_rejections >= sample_sz && sample_idx == 0 && run_weights.size() == 1) {
                    if (memtable_alias) delete memtable_alias;
                    for (auto& x: states) delete x;
                    sampling_bailouts++;
                    return; // no records in the sampling range
                }
            }

            for (size_t i=1; i<run_samples.size(); i++) {
                // sample from each WIRS level
                state.rid = runs[i].first;
                auto sampled = runs[i].second->get_samples(states[i], sample_set + sample_idx, lower_key, upper_key, run_samples[i], &state, rng) ;
                assert(sampled <= run_samples[i]);
                sample_idx += sampled;
                rejections += run_samples[i] - sampled;
                run_samples[i] = 0;
            }

        } while (sample_idx < sample_sz);

        if (memtable_alias) delete memtable_alias;
        for (auto& x: states) delete x;

        this->enforce_rejection_ratio_maximum(rng);
    }

    // Checks the tree and memtable for a tombstone corresponding to
    // the provided record in any run *above* the rid, which
    // should correspond to the run containing the record in question
    // 
    // Passing INVALID_RID indicates that the record exists within the MemTable
    bool is_deleted(const record_t *record, const RunId &rid, char *buffer, MemTable *memtable, size_t memtable_cutoff) {
        // check for tombstone in the memtable. This will require accounting for the cutoff eventually.
        if (memtable->check_tombstone(record->key, record->value)) {
            return true;
        }

        // if the record is in the memtable, then we're done.
        if (rid == INVALID_RID) {
            return false;
        }

        for (size_t lvl=0; lvl<rid.level_idx; lvl++) {
            if (lvl < memory_levels.size()) {
                if (memory_levels[lvl]->tombstone_check(memory_levels[lvl]->get_run_count(), record->key, record->value)) {
                    return true;
                }
            } else {
                assert(false);
            }
        }

        // check the level containing the run
        if (rid.level_idx < memory_levels.size()) {
            size_t run_idx = std::min((size_t) rid.run_idx, memory_levels[rid.level_idx]->get_run_count() + 1);
            return memory_levels[rid.level_idx]->tombstone_check(run_idx, record->key, record->value);
        } else {
            assert(false);
        }
    }


    size_t get_record_cnt() {
        // FIXME: need to account for both memtables with concurrency
        size_t cnt = this->memtable()->get_record_count();

        for (size_t i=0; i<this->memory_levels.size(); i++) {
            if (this->memory_levels[i]) cnt += this->memory_levels[i]->get_record_cnt();
        }

        return cnt;
    }


    size_t get_tombstone_cnt() {
        // FIXME: need to account for both memtables with concurrency
        size_t cnt = this->memtable()->get_tombstone_count();

        for (size_t i=0; i<this->memory_levels.size(); i++) {
            if (this->memory_levels[i]) cnt += this->memory_levels[i]->get_tombstone_count();
        }

        return cnt;
    }

    size_t get_height() {
        return this->memory_levels.size();
    }

    size_t get_memory_utilization() {
        size_t cnt = this->memtable_1->get_memory_utilization() + this->memtable_2->get_memory_utilization();

        for (size_t i=0; i<this->memory_levels.size(); i++) {
            if (this->memory_levels[i]) cnt += this->memory_levels[i]->get_memory_utilization();
        }

        return cnt;
    }

    size_t get_aux_memory_utilization() {
        size_t cnt = this->memtable_1->get_aux_memory_utilization() + this->memtable_2->get_aux_memory_utilization();

        for (size_t i=0; i<this->memory_levels.size(); i++) {
            if (this->memory_levels[i]) {
                cnt += this->memory_levels[i]->get_aux_memory_utilization();
            }
        }

        return cnt;
    }


    bool validate_tombstone_proportion() {
        long double ts_prop;
        for (size_t i=0; i<this->memory_levels.size(); i++) {
            if (this->memory_levels[i]) {
                ts_prop = (long double) this->memory_levels[i]->get_tombstone_count() / (long double) this->calc_level_record_capacity(i);
                if (ts_prop > (long double) this->max_tombstone_prop) {
                    return false;
                }
            }
        }

        return true;
    }

    void persist_tree(gsl_rng *rng) {
        std::string meta_dir = this->root_directory + "/meta";
        mkdir(meta_dir.c_str(), 0755);

        std::string meta_fname = meta_dir + "/lsmtree.dat";
        FILE *meta_f = fopen(meta_fname.c_str(), "w");
        assert(meta_f);

        // merge the memtable down to ensure it is persisted
        this->merge_memtable(rng);
        
        // persist each level of the tree
        for (size_t i=0; i<this->get_height(); i++) {
           // bool disk = false;

            auto level_idx = this->decode_level_index(i);
            std::string level_meta = meta_dir + "/level-" + std::to_string(i) +"-meta.dat";

            memory_levels[level_idx]->persist_level(level_meta);
        }

        fclose(meta_f);
    }

    size_t get_memtable_capacity() {
        return memtable_1->get_capacity();
    }
    

    WIRSRun *get_flattened_wirs_run() {
        std::vector<WIRSRun *> runs;

        if (this->memory_levels.size() > 0) {
            for (int i=this->memory_levels.size() - 1; i>= 0; i--) {
                if (this->memory_levels[i]) {
                    runs.emplace_back(this->memory_levels[i]->get_merged_run());
                }
            }
        }

        runs.emplace_back(new WIRSRun(this->memtable(), nullptr));

        WIRSRun *runs_array[runs.size()];

        size_t j = 0;
        for (size_t i=0; i<runs.size(); i++) {
            if (runs[i]) {
                runs_array[j++] = runs[i];
            }
        }

        WIRSRun *flattened = new WIRSRun(runs_array, j, nullptr);

        for (auto run : runs) {
            delete run;
        }

        return flattened;
    }

private:
    MemTable *memtable_1;
    MemTable *memtable_2;
    std::atomic<bool> active_memtable;
    std::atomic<bool> memtable_1_merging;
    std::atomic<bool> memtable_2_merging;

    size_t scale_factor;
    double max_tombstone_prop;
    double max_rejections_per_tombstone;

    std::vector<MemoryLevel *> memory_levels;
    size_t memory_level_cnt;

    level_index last_level_idx;

    // The directory containing all of the backing files
    // for this LSM Tree.
    std::string root_directory;



    MemTable *memtable() {
        if (memtable_1_merging && memtable_2_merging) {
            return nullptr;
        }

        return (active_memtable) ? memtable_2 : memtable_1;
    }

    inline bool rejection(const record_t *record, RunId rid, const key_t& lower_bound, const key_t& upper_bound, char *buffer, MemTable *memtable, size_t memtable_cutoff) {
        if (record->is_tombstone()) {
            tombstone_rejections++;
            return true;
        } else if (record->key < lower_bound || record->key > upper_bound) {
            bounds_rejections++;
            return true;
        } else if (this->is_deleted(record, rid, buffer, memtable, memtable_cutoff)) {
            deletion_rejections++;
            return true;
        }

        return false;
    }

    inline size_t rid_to_disk(RunId rid) {
        return rid.level_idx - this->memory_levels.size();
    }

    inline bool add_to_sample(const record_t *record, RunId rid, const key_t& upper_key, const key_t& lower_key, char *io_buffer,
                              record_t *sample_buffer, size_t &sample_idx, MemTable *memtable, size_t memtable_cutoff) {
        TIMER_INIT();
        TIMER_START();
        sampling_attempts++;
        if (!record || rejection(record, rid, lower_key, upper_key, io_buffer, memtable, memtable_cutoff)) {
            sampling_rejections++;
            return false;
        }
        TIMER_STOP();
        rejection_check_time += TIMER_RESULT();

        sample_buffer[sample_idx++] = *record;
        return true;
    }

    /*
     * Add a new level to the LSM Tree and return that level's index. Will
     * automatically determine whether the level should be on memory or on disk,
     * and act appropriately.
     */
    inline level_index grow() {
        level_index new_idx;

        size_t new_run_cnt = (LSM_LEVELING) ? 1 : this->scale_factor;
        if (this->memory_levels.size() < this->memory_level_cnt) {
            new_idx = this->memory_levels.size();
            if (new_idx > 0) {
                assert(this->memory_levels[new_idx - 1]->get_run(0)->get_tombstone_count() == 0);
            }
            this->memory_levels.emplace_back(new MemoryLevel(new_idx, new_run_cnt, this->root_directory));
        } else {
            assert(false);
        } 

        this->last_level_idx++;
        return new_idx;
    }


    // Merge the memory table down into the tree, completing any required other
    // merges to make room for it.
    inline void merge_memtable(gsl_rng *rng) {
        auto mtable = this->memtable();

        if (!this->can_merge_with(0, mtable->get_record_count())) {
            this->merge_down(0, rng);
        }

        this->merge_memtable_into_l0(mtable, rng);
        this->enforce_tombstone_maximum(0, rng);

        mtable->truncate();
        return;
    }

    /*
     * Merge the specified level down into the tree. The level index must be
     * non-negative (i.e., this function cannot be used to merge the memtable). This
     * routine will recursively perform any necessary merges to make room for the 
     * specified level.
     */
    inline void merge_down(level_index idx, gsl_rng *rng) {
        level_index merge_base_level = this->find_mergable_level(idx);
        if (merge_base_level == -1) {
            merge_base_level = this->grow();
        }

        for (level_index i=merge_base_level; i>idx; i--) {
            this->merge_levels(i, i-1, rng);
            this->enforce_tombstone_maximum(i, rng);
        }

        return;
    }

    /*
     * Find the first level below the level indicated by idx that
     * is capable of sustaining a merge operation and return its
     * level index. If no such level exists, returns -1. Also
     * returns -1 if idx==0, and no such level exists, to simplify
     * the logic of the first merge.
     */
    inline level_index find_mergable_level(level_index idx, MemTable *mtable=nullptr) {

        if (idx == 0 && this->memory_levels.size() == 0) return -1;

        bool level_found = false;
        bool disk_level;
        level_index merge_level_idx;

        size_t incoming_rec_cnt = this->get_level_record_count(idx, mtable);
        for (level_index i=idx+1; i<=this->last_level_idx; i++) {
            if (this->can_merge_with(i, incoming_rec_cnt)) {
                return i;
            }

            incoming_rec_cnt = this->get_level_record_count(i);
        }

        return -1;
    }

    /*
     * Merge the level specified by incoming level into the level specified
     * by base level. The two levels should be sequential--i.e. no levels
     * are skipped in the merge process--otherwise the tombstone ordering
     * invariant may be violated by the merge operation.
     */
    inline void merge_levels(level_index base_level, level_index incoming_level, gsl_rng *rng) {
        size_t base_idx = decode_level_index(base_level);
        size_t incoming_idx = decode_level_index(incoming_level);

        // merging two memory levels
        if (LSM_LEVELING) {
            auto tmp = this->memory_levels[base_idx];
            this->memory_levels[base_idx] = MemoryLevel::merge_levels(this->memory_levels[base_idx], this->memory_levels[incoming_idx], rng);
            this->mark_as_unused(tmp);
        } else {
            this->memory_levels[base_idx]->append_merged_runs(this->memory_levels[incoming_idx], rng);
        }

        this->mark_as_unused(this->memory_levels[incoming_idx]);
        this->memory_levels[incoming_idx] = new MemoryLevel(incoming_level, (LSM_LEVELING) ? 1 : this->scale_factor, this->root_directory);
    }

    inline void merge_memtable_into_l0(MemTable *mtable, gsl_rng *rng) {
        assert(this->memory_levels[0]);
        if (LSM_LEVELING) {
            // FIXME: Kludgey implementation due to interface constraints.
            auto old_level = this->memory_levels[0];
            auto temp_level = new MemoryLevel(0, 1, this->root_directory);
            temp_level->append_mem_table(mtable, rng);
            auto new_level = MemoryLevel::merge_levels(old_level, temp_level, rng);

            this->memory_levels[0] = new_level;
            delete temp_level;
            this->mark_as_unused(old_level);
        } else {
            this->memory_levels[0]->append_mem_table(mtable, rng);
        }
    }

    /*
     * Mark a given memory level as no-longer in use by the tree. For now this
     * will just free the level. In future, this will be more complex as the
     * level may not be able to immediately be deleted, depending upon who
     * else is using it.
     */ 
    inline void mark_as_unused(MemoryLevel *level) {
        delete level;
    }

    /*
     * Check the tombstone proportion for the specified level and
     * if the limit is exceeded, forcibly merge levels until all
     * levels below idx are below the limit.
     */
    inline void enforce_tombstone_maximum(level_index idx, gsl_rng *rng) {
        size_t level_idx = this->decode_level_index(idx);

        long double ts_prop = (long double) this->memory_levels[level_idx]->get_tombstone_count() / (long double) this->calc_level_record_capacity(idx);

        if (ts_prop > (long double) this->max_tombstone_prop) {
            this->merge_down(idx, rng);
        }

        return;
    }

    inline void enforce_rejection_ratio_maximum(gsl_rng *rng) {
        if (this->memory_levels.size() == 0) {
            return;
        }

        for (size_t i=0; i<this->last_level_idx; i++) {
            level_index idx = decode_level_index(i);

            if (this->memory_levels[idx]) {
                double ratio = this->memory_levels[idx]->get_rejections_per_tombstone();
                if (ratio > this->max_rejections_per_tombstone) {
                    this->merge_down(i, rng);
                }
            }
        } 
    }

    /*
     * Assume that level "0" should be larger than the memtable. The memtable
     * itself is index -1, which should return simply the memtable capacity.
     */
    inline size_t calc_level_record_capacity(level_index idx) {
        return this->memtable()->get_capacity() * pow(this->scale_factor, idx+1);
    }

    /*
     * Returns the actual number of records present on a specified level. An
     * index value of -1 indicates the memory table. Can optionally pass in
     * a pointer to the memory table to use, if desired. Otherwise, there are
     * no guarantees about which memtable will be accessed if level_index is -1.
     */
    inline size_t get_level_record_count(level_index idx, MemTable *mtable=nullptr) {
        assert(idx >= -1);
        if (idx == -1) {
            return (mtable) ? mtable->get_record_count() : memtable()->get_record_count();
        }

        size_t vector_index = decode_level_index(idx);

        return (memory_levels[vector_index]) ? memory_levels[vector_index]->get_record_cnt() : 0;
    }

    /*
     * Determines if the specific level can merge with another record containing
     * incoming_rec_cnt number of records. The provided level index should be 
     * non-negative (i.e., not refer to the memtable) and will be automatically
     * translated into the appropriate index into either the disk or memory level
     * vector.
     */
    inline bool can_merge_with(level_index idx, size_t incoming_rec_cnt) {
        ssize_t vector_index = decode_level_index(idx);
        assert(vector_index >= 0);

        if (vector_index >= this->memory_levels.size() || !this->memory_levels[vector_index]) {
            return false;
        }

        if (LSM_LEVELING) {
            return this->memory_levels[vector_index]->get_record_cnt() + incoming_rec_cnt <= this->calc_level_record_capacity(idx);
        } else {
            return this->memory_levels[vector_index]->get_run_count() < this->scale_factor;
        }

        // unreachable
        assert(true);
    }

    /*
     * Converts a level_index into the appropriate integer index into 
     * either the memory level or disk level vector. If the index is
     * for a memory level, set disk_level to false. If it is for a 
     * disk level, set disk_level to true. If the index is less than
     * zero, returns -1 (may indicate either an invalid index, or the
     * memtable).
     */
    inline ssize_t decode_level_index(level_index idx) {
        if (idx < 0) return -1;

        return idx;
    }


};


bool check_deleted(record_t *record, sample_state *state) {
    return state->tree->is_deleted(record, state->rid, state->buff, state->memtable, state->mtable_cutoff);
}

}

