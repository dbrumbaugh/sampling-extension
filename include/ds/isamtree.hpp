/*
 *
 */

#ifndef H_ISAMTREE
#define H_ISAMTREE

#include "util/base.hpp"
#include "io/pagedfile.hpp"
#include "util/mergeiter.hpp"
#include "util/mem.hpp"
#include "catalog/schema.hpp"
#include "catalog/field.hpp"
#include "io/fixedlendatapage.hpp"
#include "io/readcache.hpp"
#include "io/indexpagedfile.hpp"
#include "util/global.hpp"
#include "ds/bloomfilter.hpp"

namespace lsm { namespace ds {

struct ISAMTreeMetaHeader {
    PageNum root_node;
    PageNum first_data_page;
    PageNum last_data_page;
    PageNum first_data_bloom_page;
    PageNum first_tombstone_bloom_page;
    size_t tombstone_count;
};

struct ISAMTreeInternalNodeHeader {
    PageNum next_sibling;
    PageNum prev_sibling;
    size_t leaf_rec_cnt;
};

constexpr PageOffset ISAMTreeInternalNodeHeaderSize = MAXALIGN(sizeof(ISAMTreeInternalNodeHeader));

const PageNum BTREE_META_PNUM = 1;

class ISAMTree {
public:
    static std::unique_ptr<ISAMTree> create(std::unique_ptr<iter::MergeIterator> record_iter, PageNum leaf_page_cnt, 
                                               bool bloom_filters, global::g_state *state, size_t tombstone_count);

    /*
     * Initialize a ISAMTree structure within the file specified by pfile. The
     * file is assumed to be empty and the resulting file contents are
     * undefined if this is not the case. It copies up to data_page_cnt number
     * of pages of records from record_itr into a contiguous page range,
     * assuming that the iterator returns the records in a proper sorted order,
     * and constructs an ISAM Tree atop this, initializing the header page
     * appropriately.
     *
     * The record_schema used for parsing and comparing the records is obtained
     * from state.
     */
    static void initialize(io::IndexPagedFile *pfile, std::unique_ptr<iter::MergeIterator> record_iter, 
                           PageNum data_page_cnt, global::g_state *state, bool bloom_filters, size_t tombstone_count);


    /*
     * Return an ISAM Tree object created from pfile. pfile is assumed to have
     * already been properly initialized via a call to ISAMTree::initialize at
     * some point in its existence. If this is not the case, all method calls
     * to the returned object are undefined.
     */
    ISAMTree(io::IndexPagedFile *pfile, catalog::FixedKVSchema *record_schema,
                catalog::KeyCmpFunc key_cmp, io::ReadCache *cache);

    ISAMTree(io::IndexPagedFile *pfile, global::g_state *state); 

    ISAMTree() = default;

    ~ISAMTree();

    /*
     * Returns the first leaf page pid within the tree that contains a key
     * greater than or equal to the specified boundary key. Returns INVALID_PID
     * if no pages satisfy this constraint.
     */
    PageId get_lower_bound(const byte *key);

    /*
     * Returns the last leaf page pid within the tree that contains a key less
     * than or equal to the specified boundary key. Returns INVALID_PID if no
     * pages satisfy this constraint.
     */
    PageId get_upper_bound(const byte *key);

    /*
     * Returns true if this tree contains a tombstone for a record with a
     * matching key that has a timestamp less-than-or-equal-to the provided
     * one.
     */
    bool tombstone_exists(const byte *key, Timestamp time=0);

    /*
     * Returns the newest record within this tree with the specified key, and a
     * timestamp no greater than time.
     */
    Record get(const byte *key, FrameId *frid, Timestamp time=0);

    /*
     * Searches the tree for a tombstone record for the specified key/value
     * pair active at Timestamp time. If no such tombstone exists, returns an
     * invalid record, otherwise return the tombstone.
     */
    Record get_tombstone(const byte *key, const byte *val, FrameId *frid, Timestamp time=0);

    /*
     * Returns an iterator over all of the records within the leaf nodes of
     * this ISAM Tree. The iterator is not required to support rewinding, but
     * must emit records in sorted order.
     */
    std::unique_ptr<iter::GenericIterator<Record>> start_scan();

    /*
     * Returns the number of records contained within the leaf nodes of this
     * ISAM Tree.
     */
    size_t get_record_count();

    /*
     * Returns the number of leaf pages within this tree
     */
    PageNum get_leaf_page_count();

    /*
     * Returns a pointer to the file representing this ISAM Tree object.
     */
    io::PagedFile *get_pfile();

    /*
     * Returns true if the data stored in this ISAM Tree is fixed length,
     * and false if not.
     */
    bool is_fixed_length();
    
    /*
     * Returns an instance of the key comparison function used by
     * this ISAM Tree.
     */
    catalog::KeyCmpFunc get_key_cmp();

    /*
     * Returns if the record at a given RID has been deleted based on a
     * timestamp. If the newest record older than the time stamp has been
     * deleted, the record is considered deleted. Otherwise, it is considered
     * alive.
     */
    /*
    bool is_deleted(RecordId rid, Timestamp time=0);
    */

    /*
     * Returns the number of bytes of memory used by auxiliary structures
     * associated with this ISAM tree.
     */
    size_t memory_utilization();

    /*
     * Returns the number of tombstone records within this level
     */
    size_t tombstone_count();

private:
    ISAMTreeMetaHeader *get_metapage();

    io::IndexPagedFile *pfile;
    global::g_state *state;
    catalog::FixedKVSchema *record_schema;
    std::unique_ptr<catalog::FixedKVSchema> internal_index_schema; // schema for internal nodes
    catalog::KeyCmpFunc key_cmp;
    std::unique_ptr<BloomFilter> tombstone_bloom_filter;
    PageNum root_page;
    PageNum first_data_page;
    PageNum last_data_page;
    io::ReadCache *cache;
    size_t rec_cnt;
    bool fixed_length;
    
    size_t tombstone_cnt; // number of tombstones within the ISAM Tree

    PageNum search_internal_node_lower(PageNum pnum, const byte *key);
    PageNum search_internal_node_upper(PageNum pnum, const byte *key);
    SlotId search_leaf_page(byte *page_buf, const byte *key);

    static int initial_page_allocation(io::PagedFile *pfile, PageNum page_cnt, size_t tombstone_count, size_t key_len, bool filters, PageId *first_leaf, PageId *first_internal, PageId *meta, PageId *filter_meta, PageId *tombstone_filter_meta, global::g_state *state);
    static std::unique_ptr<catalog::FixedKVSchema> generate_internal_schema(catalog::FixedKVSchema *record_schema);
    static PageNum generate_internal_levels(io::PagedFile *pfile, PageNum first_page, catalog::FixedKVSchema *schema);
};

}}

#endif
