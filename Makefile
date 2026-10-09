# B+ Tree (C++17)
#   make              build everything
#   make test         unit tests
#   make demo         synthetic 500k-row run (no dataset needed)
#   make quick        first 1M records of the real dataset
#   make full         all 10M records + buffer pool sweep
CSV      ?= /Users/nileshmishra/Downloads/bplus_tree_data_10m.csv
CXX      ?= c++
CXXFLAGS ?= -O3 -DNDEBUG -std=c++17 -Wall -Wextra
HDRS     := $(wildcard src/*.hpp)
BINS     := build/bptree_bench build/bptree_report build/gen_sample

all: $(BINS)

build/bptree_bench: src/benchmark.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ src/benchmark.cpp

build/bptree_report: src/report.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ src/report.cpp

build/gen_sample: src/gen_sample.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ src/gen_sample.cpp

build/test_bptree: tests/test_bptree.cpp $(HDRS)
	@mkdir -p build
	$(CXX) -O2 -std=c++17 -Wall -Wextra -o $@ tests/test_bptree.cpp

debug: build/gen_sample   ## -O0 -g binaries for the VS Code debugger
	@mkdir -p build/debug data
	$(CXX) -O0 -g -std=c++17 -Wall -Wextra -o build/debug/bptree_bench src/benchmark.cpp
	$(CXX) -O0 -g -std=c++17 -Wall -Wextra -o build/debug/test_bptree tests/test_bptree.cpp
	@[ -f data/sample.csv ] || ./build/gen_sample 200000 data/sample.csv

test: build/test_bptree
	./build/test_bptree

demo: all
	./build/gen_sample 500000 data/sample.csv
	./run_all.sh --csv data/sample.csv --validate --pool-sweep 8,32 --pool-mb 16 --checkpoint 50000 --results-dir results_demo

quick: all
	./run_all.sh --csv "$(CSV)" --limit 1000000 --validate

full: all
	./run_all.sh --csv "$(CSV)" --validate --pool-sweep 32,128,512

report: build/bptree_report
	./build/bptree_report results

clean:
	rm -rf build storage data results_demo

.PHONY: all debug test demo quick full report clean
