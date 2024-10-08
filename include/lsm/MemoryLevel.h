#pragma once

#include <vector>
#include <memory>

#include "util/types.h"
#include "util/bf_config.h"
#include "lsm/InMemRun.h"
#include "ds/BloomFilter.h"

namespace lsm {

class DiskLevel;


class MemoryLevel {
friend class DiskLevel;

private:
    struct InternalLevelStructure {
        InternalLevelStructure(size_t cap)
        : m_cap(cap)
        , m_runs(new InMemRun*[cap]{nullptr})
        , m_bfs(new BloomFilter*[cap]{nullptr}) {} 

        ~InternalLevelStructure() {
            for (size_t i = 0; i < m_cap; ++i) {
                if (m_runs[i]) delete m_runs[i];
                if (m_bfs[i]) delete m_bfs[i];
            }

            delete[] m_runs;
            delete[] m_bfs;
        }

        size_t m_cap;
        InMemRun** m_runs;
        BloomFilter** m_bfs;
    };

public:
    MemoryLevel(ssize_t level_no, size_t run_cap, std::string root_directory, std::string meta_fname, gsl_rng *rng) 
    : m_level_no(level_no), m_run_cnt(0)
    , m_structure(new InternalLevelStructure(run_cap))
    , m_directory(root_directory) {
        FILE *meta_f = fopen(meta_fname.c_str(), "r");
        assert(meta_f);

        char fnamebuff[1028] = { 0 };
        char typebuff[1028] = { 0 };
        size_t reccnt = 0;
        size_t tscnt = 0;
        size_t idx = 0;

        // WARNING: this assumes that the file is correctly formatted, and that
        //          file names don't exceed 1027 characters. That should be the
        //          case here, but a more robust solution may be helpful
        while (fscanf(meta_f, "%s %s %ld %ld\n", typebuff, fnamebuff, &reccnt, &tscnt) != EOF && m_run_cnt < run_cap) {
            assert(strcmp(typebuff, "memory") == 0);
            m_structure->m_bfs[m_run_cnt] = new BloomFilter(BF_FPR, tscnt, BF_HASH_FUNCS, rng);
            m_structure->m_runs[m_run_cnt] = new InMemRun(std::string(fnamebuff), reccnt, tscnt, m_structure->m_bfs[m_run_cnt]);
            m_run_cnt++;
        }
    }

    MemoryLevel(ssize_t level_no, size_t run_cap, std::string root_directory)
    : m_level_no(level_no), m_run_cnt(0)
    , m_structure(new InternalLevelStructure(run_cap))
    , m_directory(root_directory) {}

    // Create a new memory level sharing the runs and repurposing it as previous level_no + 1
    // WARNING: for leveling only.
    MemoryLevel(MemoryLevel* level)
    : m_level_no(level->m_level_no + 1), m_run_cnt(level->m_run_cnt)
    , m_structure(level->m_structure) 
    , m_directory(level->m_directory) {
        assert(m_structure->m_cap == 1 && m_run_cnt == 1);
    }

    ~MemoryLevel() {}

    // WARNING: for leveling only.
    // assuming the base level is the level new level is merging into. (base_level is larger.)
    static MemoryLevel* merge_levels(MemoryLevel* base_level, MemoryLevel* new_level, const gsl_rng* rng) {
        assert(base_level->m_level_no > new_level->m_level_no || (base_level->m_level_no == 0 && new_level->m_level_no == 0));
        auto res = new MemoryLevel(base_level->m_level_no, 1, base_level->m_directory);
        res->m_run_cnt = 1;
        res->m_structure->m_bfs[0] =
            new BloomFilter(BF_FPR,
                            new_level->get_tombstone_count() + base_level->get_tombstone_count(),
                            BF_HASH_FUNCS, rng);
        InMemRun* runs[2];
        runs[0] = base_level->m_structure->m_runs[0];
        runs[1] = new_level->m_structure->m_runs[0];

        res->m_structure->m_runs[0] = new InMemRun(runs, 2, res->m_structure->m_bfs[0]);
        return res;
    }

    void append_mem_table(MemTable* memtable, const gsl_rng* rng) {
        assert(m_run_cnt < m_structure->m_cap);
        m_structure->m_bfs[m_run_cnt] = new BloomFilter(BF_FPR, memtable->get_tombstone_count(), BF_HASH_FUNCS, rng);
        m_structure->m_runs[m_run_cnt] = new InMemRun(memtable, m_structure->m_bfs[m_run_cnt]);
        ++m_run_cnt;
    }

    void append_merged_runs(MemoryLevel* level, const gsl_rng* rng) {
        assert(m_run_cnt < m_structure->m_cap);
        m_structure->m_bfs[m_run_cnt] = new BloomFilter(BF_FPR, level->get_tombstone_count(), BF_HASH_FUNCS, rng);
        m_structure->m_runs[m_run_cnt] = new InMemRun(level->m_structure->m_runs, level->m_run_cnt, m_structure->m_bfs[m_run_cnt]);
        ++m_run_cnt;
    }

    // Append the sample range in-order.....
    void get_sample_ranges(std::vector<SampleRange>& dst, std::vector<size_t>& rec_cnts, const char* low, const char* high) {
        for (ssize_t i = 0; i < m_run_cnt; ++i) {
            auto low_pos = m_structure->m_runs[i]->get_lower_bound(low);
            auto high_pos = m_structure->m_runs[i]->get_upper_bound(high);
            assert(high_pos >= low_pos);
            dst.emplace_back(SampleRange{RunId{m_level_no, i}, low_pos, high_pos});
            rec_cnts.emplace_back(high_pos - low_pos);
        }
    }

    bool bf_rejection_check(size_t run_stop, const char* key) {
        for (size_t i = 0; i < run_stop; ++i) {
            if (m_structure->m_bfs[i] && m_structure->m_bfs[i]->lookup(key, key_size))
                return true;
        }
        return false;
    }

    bool tombstone_check(size_t run_stop, const char* key, const char* val) {
        for (size_t i = 0; i < run_stop;  ++i) {
            if (m_structure->m_runs[i] && m_structure->m_bfs[i]->lookup(key, key_size) && m_structure->m_runs[i]->check_tombstone(key, val))
                return true;
        }
        return false;
    }

    const char* get_record_at(size_t run_no, size_t idx) {
        return m_structure->m_runs[run_no]->get_record_at(idx);
    }
    
    InMemRun* get_run(size_t idx) {
        return m_structure->m_runs[idx];
    }

    size_t get_run_count() {
        return m_run_cnt;
    }

    size_t get_record_cnt() {
        size_t cnt = 0;
        for (size_t i=0; i<m_run_cnt; i++) {
            cnt += m_structure->m_runs[i]->get_record_count();
        }

        return cnt;
    }
    
    size_t get_tombstone_count() {
        size_t res = 0;
        for (size_t i = 0; i < m_run_cnt; ++i) {
            res += m_structure->m_runs[i]->get_tombstone_count();
        }
        return res;
    }

    size_t get_aux_memory_utilization() {
        size_t cnt = 0;
        for (size_t i=0; i<m_run_cnt; i++) {
            if (m_structure->m_bfs[i]) {
                cnt += m_structure->m_bfs[i]->get_memory_utilization();
            }
        }

        return cnt;
    }

    size_t get_memory_utilization() {
        size_t cnt = 0;
        for (size_t i=0; i<m_run_cnt; i++) {
            if (m_structure->m_runs[i]) {
                cnt += m_structure->m_runs[i]->get_memory_utilization();
            }
        }

        return cnt;
    }

    double get_tombstone_prop() {
        size_t tscnt = 0;
        size_t reccnt = 0;
        for (size_t i=0; i<m_run_cnt; i++) {
            if (m_structure->m_runs[i]) {
                tscnt += m_structure->m_runs[i]->get_tombstone_count();
                reccnt += m_structure->m_runs[i]->get_record_count();
            }
        }

        return (double) tscnt / (double) (tscnt + reccnt);
    }

    void persist_level(std::string meta_fname) {
        FILE *meta_f = fopen(meta_fname.c_str(), "w");
        assert(meta_f);
        for (size_t i=0; i<m_structure->m_cap; i++) {
            if (m_structure->m_runs[i]) {
                std::string fname = m_directory + "/level" + std::to_string(m_level_no) 
                                     + "_run" + std::to_string(i) + "-0.dat";
                m_structure->m_runs[i]->persist_to_file(fname);
                fprintf(meta_f, "memory %s %ld %ld\n", fname.c_str(), m_structure->m_runs[i]->get_record_count(), m_structure->m_runs[i]->get_tombstone_count());
            }
        }
        fclose(meta_f);
    }

private:
    ssize_t m_level_no;
    
    size_t m_run_cnt;
    size_t m_run_size_cap;
    std::shared_ptr<InternalLevelStructure> m_structure;
    std::string m_directory;
};

}
