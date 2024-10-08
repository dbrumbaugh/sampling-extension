#pragma once

#include <vector>
#include <string>

#include "util/types.h"
#include "util/bf_config.h"
#include "lsm/InMemRun.h"
#include "lsm/IsamTree.h"
#include "ds/BloomFilter.h"
#include "lsm/MemoryLevel.h"

namespace lsm {

class DiskLevel {
public:

    DiskLevel(ssize_t level_no, size_t run_cap, std::string root_directory, std::string meta_fname, gsl_rng *rng) 
    : m_level_no(level_no), m_run_cap(run_cap), m_run_cnt(0)
    , m_runs(new ISAMTree*[run_cap]{nullptr})
    , m_bfs(new BloomFilter*[run_cap]{nullptr})
    , m_pfiles(new PagedFile*[run_cap]{nullptr})
    , m_owns(new bool[run_cap]{true})
    , m_directory(root_directory)
    , m_version(0)
    , m_retain(false) {
        FILE *meta_f = fopen(meta_fname.c_str(), "r");
        assert(meta_f);

        int owns = false;
        char fnamebuff[1028] = { 0 };
        char typebuff[1028] = { 0 };
        size_t reccnt = 0;
        size_t tscnt = 0;
        size_t version = 0;
        PageNum last_leaf = 0;
        PageNum root_node = 0;
        size_t idx = 0;
        // WARNING: this assumes that the file is correctly formatted, and that
        //          file names don't exceed 1027 characters. That should be the
        //          case here, but a more robust solution may be helpful

        while (fscanf(meta_f, "%s %d %s %ld %d %ld %ld %d\n", typebuff, &owns, fnamebuff, &version, &last_leaf, &reccnt, &tscnt, &root_node) != EOF && m_run_cnt < m_run_cap) {
            assert(strcmp(typebuff, "disk") == 0);
            m_bfs[m_run_cnt] = new BloomFilter(BF_FPR, tscnt, BF_HASH_FUNCS, rng);
            m_pfiles[m_run_cnt] = PagedFile::create(fnamebuff, false);
            m_runs[m_run_cnt] = new ISAMTree(m_pfiles[m_run_cnt], reccnt, tscnt, last_leaf, root_node, m_bfs[m_run_cnt], rng);
            m_version = version;
            m_run_cnt++;
        }
    }


    DiskLevel(ssize_t level_no, size_t run_cap, std::string root_directory, size_t version=0)
    : m_level_no(level_no), m_run_cap(run_cap), m_run_cnt(0)
    , m_runs(new ISAMTree*[run_cap]{nullptr})
    , m_bfs(new BloomFilter*[run_cap]{nullptr})
    , m_pfiles(new PagedFile*[run_cap]{nullptr})
    , m_owns(new bool[run_cap]{true})
    , m_directory(root_directory)
    , m_version(version)
    , m_retain(false) {}

    ~DiskLevel() {
        for (size_t i = 0; i < m_run_cap; ++i) {
            if (m_owns[i]) {
                if (m_runs[i]) delete m_runs[i];
                if (m_bfs[i]) delete m_bfs[i];
                if (m_pfiles[i]) delete m_pfiles[i];
            }
        }

        delete[] m_runs;
        delete[] m_bfs;
        delete[] m_pfiles;
        delete[] m_owns;
    }

    static DiskLevel *merge_levels(DiskLevel *base_level, MemoryLevel *new_level, const gsl_rng *rng) {
        assert(base_level->m_level_no > new_level->m_level_no);
        auto res = new DiskLevel(base_level->m_level_no, 1, base_level->m_directory, base_level->m_version + 1);
        res->m_run_cnt = 1;

        res->m_bfs[0] = new BloomFilter(BF_FPR,
                            new_level->get_tombstone_count() + base_level->get_tombstone_count(),
                            BF_HASH_FUNCS, rng);

        ISAMTree *run1 = base_level->m_runs[0];
        InMemRun *run2 = new_level->m_structure->m_runs[0];
        assert(run2);

        res->m_pfiles[0] = PagedFile::create(base_level->get_fname(0));
        res->m_owns[0] = true;
        assert(res->m_pfiles[0]);
        
        res->m_runs[0] = (run1) ? new ISAMTree(res->m_pfiles[0], rng, res->m_bfs[0], &run2, 1, &run1, 1)
                                : new ISAMTree(res->m_pfiles[0], rng, res->m_bfs[0], &run2, 1, nullptr, 0);
        
        return res;
    }

    
    static DiskLevel *merge_levels(DiskLevel *base_level, DiskLevel *new_level, const gsl_rng *rng) {
        assert(base_level->m_level_no > new_level->m_level_no);

        auto res = new DiskLevel(base_level->m_level_no, 1, base_level->m_directory, base_level->m_version+1);

        // If the base level is empty, we can simply shift the new
        // level into it without rebuilding the level
        if (base_level->get_run_count() == 0) {
            res->m_bfs[0] = new_level->m_bfs[0];
            res->m_pfiles[0] = new_level->m_pfiles[0];
            res->m_pfiles[0]->rename_file(base_level->get_fname(0));
            res->m_runs[0] = new_level->m_runs[0];
            res->m_owns[0] = true;
            res->m_run_cnt = 1;
            new_level->release_ownership(0);
            return res;
        }

        res->m_bfs[0] = new BloomFilter(BF_FPR,
                            new_level->get_tombstone_count() + base_level->get_tombstone_count(),
                            BF_HASH_FUNCS, rng);

        res->m_pfiles[0] = PagedFile::create(base_level->get_fname(0), true);
        assert(res->m_pfiles[0]);

        res->m_run_cnt = 1;

        ISAMTree *runs[2] = {
                             base_level->m_runs[0],
                             new_level->m_runs[0]
                            };

        res->m_runs[0] = (runs[0]) ? new ISAMTree(res->m_pfiles[0], rng, res->m_bfs[0], nullptr, 0, runs, 2) 
                                   : new ISAMTree(res->m_pfiles[0], rng, res->m_bfs[0], nullptr, 0, &runs[1], 1);

        return res;
    }


    void append_merged_runs(DiskLevel* level, const gsl_rng* rng) {
        assert(m_run_cnt < m_run_cap);

        // If the level being appended only has one run in it, we can 
        // simply move the contents of that level into this one without
        // running the merge process
        if (level->get_run_count() == 1) {
            m_bfs[m_run_cnt] = level->m_bfs[0];
            m_pfiles[m_run_cnt] = level->m_pfiles[0];
            m_pfiles[m_run_cnt]->rename_file(this->get_fname(m_run_cnt));
            m_runs[m_run_cnt] = level->m_runs[0];
            level->release_ownership(0);
        } else {
            m_bfs[m_run_cnt] = new BloomFilter(BF_FPR, level->get_tombstone_count(), BF_HASH_FUNCS, rng);

            m_pfiles[m_run_cnt] = PagedFile::create(this->get_fname(m_run_cnt), true);
            assert(m_pfiles[m_run_cnt]);

            m_runs[m_run_cnt] = new ISAMTree(m_pfiles[m_run_cnt], rng, m_bfs[m_run_cnt], nullptr, 0, level->m_runs, level->m_run_cnt);
        }
        m_owns[m_run_cnt] = true;
        ++m_run_cnt;
    }

    void append_merged_runs(MemoryLevel *level, const gsl_rng *rng) {
        assert(m_run_cnt < m_run_cap);
        m_bfs[m_run_cnt] = new BloomFilter(BF_FPR, level->get_tombstone_count(), BF_HASH_FUNCS, rng);

        m_pfiles[m_run_cnt] = PagedFile::create(this->get_fname(m_run_cnt), true);
        assert(m_pfiles[m_run_cnt]);

        m_runs[m_run_cnt] = new ISAMTree(m_pfiles[m_run_cnt], rng, m_bfs[m_run_cnt], level->m_structure->m_runs, level->m_run_cnt, nullptr, 0);
        m_owns[m_run_cnt] = true;
        ++m_run_cnt;
    }

    // Append the sample range in-order.....
    void get_sample_ranges(std::vector<SampleRange>& dst, std::vector<size_t>& rec_cnts, const char* low, const char* high, char *buffer) {
        for (ssize_t i = 0; i < m_run_cnt; ++i) {
            auto low_pos = m_runs[i]->get_lower_bound(low, buffer);

            // If the low position is invalid, then there are no elements
            // on this level within the specified sample range.
            if (low_pos == INVALID_PNUM) {
                continue;
            }

            auto high_pos = m_runs[i]->get_upper_bound(high, buffer);

            // If high is larger than the largest key in the run,
            // technically no "upper bound" on that key exists.
            // Instead, the last data page should be used as the
            // high_pos.
            if (high_pos == INVALID_PNUM) {
                high_pos = m_runs[i]->get_last_leaf_pnum();
            }
            assert(high_pos >= low_pos);
            dst.emplace_back(SampleRange{RunId{m_level_no, i}, low_pos, high_pos});
            rec_cnts.emplace_back((high_pos - low_pos + 1) * (PAGE_SIZE/record_size));
        }
    }

    bool bf_rejection_check(size_t run_stop, const char* key) {
        for (size_t i = 0; i < run_stop; ++i) {
            if (m_bfs[i] && m_bfs[i]->lookup(key, key_size))
                return true;
        }
        return false;
    }

    bool tombstone_check(size_t run_stop, const char* key, const char* val, char *buffer) {
        for (size_t i = 0; i < run_stop;  ++i) {
            if (m_runs[i] && m_bfs[i]->lookup(key, key_size) && m_runs[i]->check_tombstone(key, val, buffer))
                return true;
        }
        return false;
    }

    const char* get_record_at(size_t run_no, PageNum initial_pnum, size_t idx, char *buffer, PageNum &pg_in_buffer) {
        return m_runs[run_no]->sample_record(initial_pnum, idx, buffer, pg_in_buffer);
    }
    
    ISAMTree* get_run(size_t idx) {
        return m_runs[idx];
    }

    size_t get_run_count() {
        return m_run_cnt;
    }
    
    size_t get_tombstone_count() {
        size_t res = 0;
        for (size_t i = 0; i < m_run_cnt; ++i) {
            res += m_runs[i]->get_tombstone_count();
        }
        return res;
    }

    size_t get_record_cnt() {
        size_t cnt = 0;
        for (size_t i=0; i<m_run_cnt; i++) {
            cnt += m_runs[i]->get_record_count();
        }

        return cnt;
    }

    size_t get_aux_memory_utilization() {
        size_t cnt = 0;
        for (size_t i=0; i<m_run_cnt; i++) {
            if (m_bfs[i]) {
                cnt += m_bfs[i]->get_memory_utilization();
            }
        }

        return cnt;
    }

    double get_tombstone_prop() {
        size_t tscnt = 0;
        size_t reccnt = 0;
        for (size_t i=0; i<m_run_cnt; i++) {
            if (m_runs[i]) {
                tscnt += m_runs[i]->get_tombstone_count();
                reccnt += m_runs[i]->get_record_count();
            }
        }

        return (double) tscnt / (double) (tscnt + reccnt);
    }

    void persist_level(std::string meta_fname) {
        FILE *meta_f = fopen(meta_fname.c_str(), "w");
        assert(meta_f);
        for (size_t i=0; i<m_run_cap; i++) {
            if (m_runs[i]) {
                fprintf(meta_f, "disk %d %s %ld %d %ld %ld %d\n", m_owns[i], m_runs[i]->get_pfile()->get_fname().c_str(), m_version, m_runs[i]->get_last_leaf_pnum(), m_runs[i]->get_record_count(), m_runs[i]->get_tombstone_count(), m_runs[i]->get_root_pnum());
                m_runs[i]->retain();
            }
        }
        fclose(meta_f);
    }

private:
    ssize_t m_level_no;
    size_t m_run_cap;
    size_t m_run_cnt;
    size_t m_version;
    ISAMTree** m_runs;
    BloomFilter** m_bfs;
    PagedFile** m_pfiles;
    std::string m_directory;
    bool *m_owns;
    bool m_retain;


    std::string get_fname(size_t idx) {
        return m_directory + "/level" + std::to_string(m_level_no)
            + "_run" + std::to_string(idx) + "-" + std::to_string(m_version + 1) + ".dat";
    }

    void release_ownership(size_t idx) {
        assert(idx < m_run_cnt);
        m_owns[idx] = false;
        m_run_cnt--;
        m_bfs[idx] = nullptr;
        m_runs[idx] = nullptr;
        m_pfiles[idx] = nullptr;
    }
};

}
