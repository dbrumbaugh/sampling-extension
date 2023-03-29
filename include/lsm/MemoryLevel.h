#pragma once

#include <vector>
#include <memory>

#include "util/types.h"
#include "util/bf_config.h"
#include "lsm/WIRSRun.h"
#include "ds/BloomFilter.h"

namespace lsm {

class DiskLevel;

class MemoryLevel {
friend class DiskLevel;

static const size_t REJECTION_TRIGGER_THRESHOLD = 1024;

private:
    struct InternalLevelStructure {
        InternalLevelStructure(size_t cap)
        : m_cap(cap)
        , m_runs(new WIRSRun*[cap]{nullptr})
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
        WIRSRun** m_runs;
        BloomFilter** m_bfs;
    };

public:
    MemoryLevel(ssize_t level_no, size_t run_cap, bool tagging)
    : m_level_no(level_no), m_run_cnt(0)
    , m_structure(new InternalLevelStructure(run_cap))
    , m_tagging(tagging) {}

    // Create a new memory level sharing the runs and repurposing it as previous level_no + 1
    // WARNING: for leveling only.
    MemoryLevel(MemoryLevel* level, bool tagging)
    : m_level_no(level->m_level_no + 1), m_run_cnt(level->m_run_cnt)
    , m_structure(level->m_structure)
    , m_tagging(tagging) {
        assert(m_structure->m_cap == 1 && m_run_cnt == 1);
    }


    ~MemoryLevel() {}

    // WARNING: for leveling only.
    // assuming the base level is the level new level is merging into. (base_level is larger.)
    static MemoryLevel* merge_levels(MemoryLevel* base_level, MemoryLevel* new_level, bool tagging, const gsl_rng* rng) {
        assert(base_level->m_level_no > new_level->m_level_no || (base_level->m_level_no == 0 && new_level->m_level_no == 0));
        auto res = new MemoryLevel(base_level->m_level_no, 1, tagging);
        res->m_run_cnt = 1;
        res->m_structure->m_bfs[0] =
            new BloomFilter(BF_FPR,
                            new_level->get_tombstone_count() + base_level->get_tombstone_count(),
                            BF_HASH_FUNCS, rng);
        WIRSRun* runs[2];
        runs[0] = base_level->m_structure->m_runs[0];
        runs[1] = new_level->m_structure->m_runs[0];

        res->m_structure->m_runs[0] = new WIRSRun(runs, 2, res->m_structure->m_bfs[0], tagging);
        return res;
    }

    void append_mem_table(MemTable* memtable, const gsl_rng* rng) {
        assert(m_run_cnt < m_structure->m_cap);
        m_structure->m_bfs[m_run_cnt] = new BloomFilter(BF_FPR, memtable->get_tombstone_count(), BF_HASH_FUNCS, rng);
        m_structure->m_runs[m_run_cnt] = new WIRSRun(memtable, m_structure->m_bfs[m_run_cnt], m_tagging);
        ++m_run_cnt;
    }

    void append_merged_runs(MemoryLevel* level, const gsl_rng* rng) {
        assert(m_run_cnt < m_structure->m_cap);
        m_structure->m_bfs[m_run_cnt] = new BloomFilter(BF_FPR, level->get_tombstone_count(), BF_HASH_FUNCS, rng);
        m_structure->m_runs[m_run_cnt] = new WIRSRun(level->m_structure->m_runs, level->m_run_cnt, m_structure->m_bfs[m_run_cnt], m_tagging);
        ++m_run_cnt;
    }

    WIRSRun *get_merged_run() {
        WIRSRun *runs[m_run_cnt];

        for (size_t i=0; i<m_run_cnt; i++) {
            runs[i] = (m_structure->m_runs[i]) ? m_structure->m_runs[i] : nullptr;
        }

        return new WIRSRun(runs, m_run_cnt, nullptr, m_tagging);
    }

    // Append the sample range in-order.....
    void get_run_weights(std::vector<double>& weights, std::vector<std::pair<RunId, WIRSRun *>> &runs) {
        for (size_t i=0; i<m_run_cnt; i++) {
            if (m_structure->m_runs[i] && m_structure->m_runs[i]->get_total_weight() > 0) {
                runs.push_back({{m_level_no, (ssize_t) i}, m_structure->m_runs[i]});
                weights.push_back(m_structure->m_runs[i]->get_total_weight());
            }
        }
    }

    bool bf_rejection_check(size_t run_stop, const key_t& key) {
        for (size_t i = 0; i < run_stop; ++i) {
            if (m_structure->m_bfs[i] && m_structure->m_bfs[i]->lookup(key))
                return true;
        }
        return false;
    }

    bool check_tombstone(ssize_t run_stop, const key_t& key, const value_t& val) {
        for (size_t i = m_run_cnt - 1; i > run_stop;  i--) {
            if (m_structure->m_runs[i] && (m_structure->m_bfs[i]->lookup(key))
                && m_structure->m_runs[i]->check_tombstone(key, val))
                return true;
        }
        return false;
    }

    bool delete_record(const key_t& key, const value_t& val) {
        for (size_t i = 0; i < m_structure->m_cap;  ++i) {
            if (m_structure->m_runs[i] && m_structure->m_runs[i]->delete_record(key, val)) {
                return true;
            }
        }

        return false;
    }

    const record_t* get_record_at(size_t run_no, size_t idx) {
        return m_structure->m_runs[run_no]->get_record_at(idx);
    }
    
    WIRSRun* get_run(size_t idx) {
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

    size_t get_rejection_count() {
        size_t rej_cnt = 0;
        for (size_t i=0; i<m_run_cnt; i++) {
            if (m_structure->m_runs[i]) {
                rej_cnt += m_structure->m_runs[i]->get_rejection_count();
            }
        }

        return rej_cnt;
    }

    double get_rejection_rate() {
        size_t rej_cnt = 0;
        size_t attempt_cnt = 0;
        for (size_t i=0; i<m_run_cnt; i++) {
            if (m_structure->m_runs[i]) {
                attempt_cnt += m_structure->m_runs[i]->get_ts_check_count();
                rej_cnt += m_structure->m_runs[i]->get_rejection_count();
            }
        }

        if (attempt_cnt == 0) return 0;

        // the rejection rate is considered 0 until we exceed an
        // absolute threshold of rejections.
        if (rej_cnt <= REJECTION_TRIGGER_THRESHOLD) return 0;

        return (double) rej_cnt / (double) attempt_cnt;
    }

private:
    ssize_t m_level_no;
    
    size_t m_run_cnt;
    size_t m_run_size_cap;
    bool m_tagging;
    std::shared_ptr<InternalLevelStructure> m_structure;
};

}
