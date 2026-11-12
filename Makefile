# ===========================================================================
#  Makefile — build helpers for cppbalancer and the benchmark harness.
#
#    make            build the balancer
#    make bench      build the balancer + the benchmark tools
#    make run        build and run the balancer on port 8181
#    make test       build, start 3 echo backends + the balancer, run the client
#    make bench-run  run the full benchmark suite (takes several minutes)
#    make bench-report  regenerate benchmark/results/SUMMARY.md from raw data
#    make clean      remove build artefacts
# ===========================================================================

CXX      ?= g++
CC       ?= gcc
CXXFLAGS ?= -O2 -std=c++17 -pthread
CFLAGS   ?= -O2 -pthread -Wall -Wextra
PYTHON   ?= python3

BALANCER := cppbalancer
BENCH    := benchmark/build

.PHONY: all bench run test bench-run bench-report clean

all: $(BALANCER)

$(BALANCER): cppbalancer.cpp
	$(CXX) $(CXXFLAGS) -Wall -Wextra -o $@ $<

bench: $(BALANCER) $(BENCH)/http_backend $(BENCH)/cppbalancer_prefix

$(BENCH):
	mkdir -p $(BENCH)

$(BENCH)/http_backend: benchmark/backends/http_backend.c | $(BENCH)
	$(CC) $(CFLAGS) -o $@ $<

$(BENCH)/cppbalancer_prefix: benchmark/baseline/cppbalancer_prefix.cpp | $(BENCH)
	$(CXX) $(CXXFLAGS) -o $@ $<

run: $(BALANCER)
	./$(BALANCER)

# Interactive smoke test: 3 echo backends + balancer + one client request.
# NOTE: every background process is redirected away from make's stdout, and
# the trap kills them on exit — otherwise `make` waits forever for the pipe
# to close.
test: $(BALANCER)
	@bash -c '\
	  $(PYTHON) test/test_server.py --port 9007 >/dev/null 2>&1 & B1=$$!; \
	  $(PYTHON) test/test_server.py --port 9003 >/dev/null 2>&1 & B2=$$!; \
	  $(PYTHON) test/test_server.py --port 6789 >/dev/null 2>&1 & B3=$$!; \
	  trap "kill $$B1 $$B2 $$B3 2>/dev/null" EXIT; \
	  sleep 1; \
	  ./$(BALANCER) --quiet >/dev/null 2>&1 & LB=$$!; \
	  trap "kill $$B1 $$B2 $$B3 $$LB 2>/dev/null" EXIT; \
	  sleep 1; \
	  $(PYTHON) test/test_client.py; \
	  echo "--- round-robin check (expect 9007, 9003, 6789, 9007, 9003, 6789) ---"; \
	  for i in 1 2 3 4 5 6; do $(PYTHON) test/test_client.py | grep Received; done; \
	  kill $$LB 2>/dev/null; \
	  trap "kill $$B1 $$B2 $$B3 2>/dev/null" EXIT'

bench-run: bench
	cd benchmark && ./run_benchmarks.sh

bench-report:
	cd benchmark && $(PYTHON) tools/analyze.py --results results

clean:
	rm -f $(BALANCER)
	rm -rf $(BENCH)
