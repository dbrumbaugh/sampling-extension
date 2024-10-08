#define ENABLE_TIMER

#include "bench.h"

static void benchmark(lsm::LSMTree *tree, size_t k, size_t trial_cnt, size_t min, size_t max, double selectivity)
{
    char* buffer1 = (char*) std::aligned_alloc(lsm::SECTOR_SIZE, lsm::PAGE_SIZE);
    char* buffer2 = (char*) std::aligned_alloc(lsm::SECTOR_SIZE, lsm::PAGE_SIZE);
    char sample_set[k*lsm::record_size];

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < trial_cnt; i++) {
        auto range = get_key_range(min, max, selectivity);
        tree->range_sample(sample_set, (char*) &range.first, (char*) &range.second, k, buffer1, buffer2, g_rng);
    }

    auto stop = std::chrono::high_resolution_clock::now();

    auto total_latency = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start);
    double avg_latency = (double) total_latency.count() / trial_cnt;

    free(buffer1);
    free(buffer2);

    //fprintf(stderr, "Average Sample Latency (ns)\n");
    printf("%lf %.0lf\n", selectivity, avg_latency);
}

static void benchmark(lsm::LSMTree *tree, size_t k, double selectivity, const std::vector<std::pair<size_t, size_t>>& queries)
{
    char* buffer1 = (char*) std::aligned_alloc(lsm::SECTOR_SIZE, lsm::PAGE_SIZE);
    char* buffer2 = (char*) std::aligned_alloc(lsm::SECTOR_SIZE, lsm::PAGE_SIZE);
    char sample_set[k*lsm::record_size];
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < queries.size(); i++) {
        tree->range_sample(sample_set, (char*) &queries[i].first, (char *) &queries[i].second, k, buffer1, buffer2, g_rng);
    }

    auto stop = std::chrono::high_resolution_clock::now();

    auto total_latency = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start);
    double avg_latency = (double) total_latency.count() / queries.size();

    //fprintf(stderr, "Average Sample Latency (ns)");
    printf("%lf %.0lf\n", selectivity, avg_latency);
}


int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "Usage: lsm_bench <filename> <record_count> <sample_size>\n");
        exit(EXIT_FAILURE);
    }

    std::string filename = std::string(argv[1]);
    size_t record_count = atol(argv[2]);
    size_t sample_size = atol(argv[3]);

	std::vector<double> sel = {0.1, 0.05, 0.01, 0.001, 0.0005, 0.0001};
	std::vector<std::pair<size_t, size_t>> queries[6];

	if (argc == 5) {
		FILE* fp = fopen(argv[4], "r");
		size_t cnt = 0;
		size_t offset = 0;
		double selectivity;
		size_t start, end;
		while (EOF != fscanf(fp, "%zu%zu%lf", &start, &end, &selectivity)) {
			if (start < end && std::abs(selectivity - sel[offset]) / sel[offset] < 0.1)
				queries[offset].emplace_back(start, end);
			++cnt;
			if (cnt % 100 == 0) ++offset;
		}
		fclose(fp);
	}

    std::string root_dir = "benchmarks/data/sample_bench";

    init_bench_env(true);

    // use for selectivity calculations
    lsm::key_type min_key = 0;
    lsm::key_type max_key = record_count - 1;

    auto sampling_tree = lsm::LSMTree(root_dir, 15000, 750, 10, 1000, 1, g_rng);

    std::fstream datafile;
    datafile.open(filename, std::ios::in);

    //build_lsm_tree(&sampling_tree, &datafile);
	warmup(&datafile, &sampling_tree, record_count, 0.05);
	
	//std::vector<double> sel = {0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1};

    size_t n;
	if (argc == 5) {
		for (size_t i = 0; i < 6; ++i)
			benchmark(&sampling_tree, sample_size, sel[i], queries[i]);
	} else {
		for (auto selectivity: sel)
			benchmark(&sampling_tree, sample_size, 10000, min_key, max_key, selectivity);
	}

    delete_bench_env();
    exit(EXIT_SUCCESS);
}
