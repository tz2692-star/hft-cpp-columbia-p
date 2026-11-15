# HFT starter — local autograder + per-challenge quick builds.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -pthread -Iinclude -Itests

.PHONY: test clean pool book fix u64toa rolling spsc client register run

test:                      ## run the full autograder (grades what you've implemented)
	python3 tests/run_ci.py

# quick single-challenge builds while you iterate:
pool:    ; $(CXX) $(CXXFLAGS) tests/pool_test.cpp    -o /tmp/pool    && /tmp/pool
book:    ; $(CXX) $(CXXFLAGS) tests/book_test.cpp    -o /tmp/book    && /tmp/book
fix:     ; $(CXX) $(CXXFLAGS) tests/fix_test.cpp     -o /tmp/fix     && /tmp/fix
u64toa:  ; $(CXX) $(CXXFLAGS) tests/u64toa_test.cpp  -o /tmp/u64toa  && /tmp/u64toa
rolling: ; $(CXX) $(CXXFLAGS) tests/rolling_counter_test.cpp -o /tmp/rolling && /tmp/rolling
spsc:    ; $(CXX) $(CXXFLAGS) tests/spsc_correctness.cpp -o /tmp/spsc && /tmp/spsc

.PHONY: spsc-stress spsc-tsan pipeline-test pipeline-tsan strategy-test strategy-tsan

SPSC_N ?= 5000000

# GCC's TSan shadow layout can conflict with Linux/WSL ASLR. Disable ASLR
# only for this test process, keeping sanitizer instrumentation and failures.
# Override with TSAN_RUNNER= when using a runtime that needs no workaround.
ifeq ($(shell uname -s),Linux)
TSAN_RUNNER ?= setarch $(shell uname -m) -R
else
TSAN_RUNNER ?=
endif

spsc-stress:
	$(CXX) $(CXXFLAGS) tests/spsc_concurrency.cpp -o /tmp/hft_spsc_stress
	SPSC_N=$(SPSC_N) /tmp/hft_spsc_stress

spsc-tsan:
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=thread tests/spsc_concurrency.cpp -o /tmp/hft_spsc_tsan
	SPSC_N=$(SPSC_N) TSAN_OPTIONS=halt_on_error=1 $(TSAN_RUNNER) /tmp/hft_spsc_tsan

pipeline-test:
	$(CXX) -std=c++17 -O2 -Wall -Wextra -pthread -Iinclude tests/pipeline_test.cpp -o /tmp/hft_pipeline_test
	/tmp/hft_pipeline_test

pipeline-tsan:
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -pthread -Iinclude -fsanitize=thread tests/pipeline_test.cpp -o /tmp/hft_pipeline_tsan
	TSAN_OPTIONS=halt_on_error=1 $(TSAN_RUNNER) /tmp/hft_pipeline_tsan

strategy-test:
	$(CXX) -std=c++17 -O2 -Wall -Wextra -pthread tests/strategy_test.cpp -o /tmp/hft_strategy_test
	/tmp/hft_strategy_test

strategy-tsan:
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -pthread -fsanitize=thread tests/strategy_test.cpp -o /tmp/hft_strategy_tsan
	TSAN_OPTIONS=halt_on_error=1 $(TSAN_RUNNER) /tmp/hft_strategy_tsan

clean:   ; rm -f /tmp/pool /tmp/book /tmp/fix /tmp/u64toa /tmp/rolling /tmp/spsc report.json

# ── Arena: build the C++ client, register your team, run your bot ─────────────
ARENA ?= https://algoarenafin.duckdns.org
client:                    ## build hft/cpp_client with TLS -> hft/cpp_client/build/hft_bot
	cmake -S hft/cpp_client -B hft/cpp_client/build -DHFT_USE_TLS=ON && cmake --build hft/cpp_client/build
register:                  ## make register CODE=<class code> NAME="Your Team"  (writes .env)
	python3 scripts/register.py --arena $(ARENA) --code $(CODE) --name "$(NAME)"
run:                       ## start your bot with the credentials in .env
	./run_bot.sh
