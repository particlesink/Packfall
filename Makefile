.PHONY: all help linux msys2 sanitize doc html test examples check test-sanitize check-sanitize clean distclean clean-linux clean-msys2 clean-sanitize clean-doc clean-tests clean-examples

PYTHON ?= python3
TEST_ARGS ?=
EXAMPLE_ARGS ?=

STD ?= -std=gnu++17
export STD

SANITIZE_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
SANITIZE_ENV := ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

ifeq ($(OS),Windows_NT)
DEFAULT_TARGET := msys2
else
DEFAULT_TARGET := linux
endif

all: $(DEFAULT_TARGET)

help:
	@echo ""
	@echo "Supported build targets"
	@echo ""
	@echo "  make linux            build with src/Makefile.linux"
	@echo "  make sanitize         build Linux Packfall with ASan and UBSan"
	@echo "  make test-sanitize    run the test suite under ASan and UBSan"
	@echo "  make check-sanitize   run tests and examples under ASan and UBSan"
	@echo "  make msys2            build with src/Makefile.msys2"
	@echo "  make html             build the Sphinx HTML docs in doc/_build/html"
	@echo "  make doc              alias for make html"
	@echo "  make test             build Linux Packfall and run every test case"
	@echo "  make examples         build Linux Packfall and run every example check"
	@echo "  make check            run all tests and example checks"
	@echo "  make clean-linux      remove Linux build artifacts"
	@echo "  make clean-msys2      remove MSYS2 build artifacts"
	@echo "  make clean-doc        remove generated doc build artifacts"
	@echo "  make clean-tests      remove generated test artifacts"
	@echo "  make clean-examples   remove generated example artifacts"
	@echo "  make clean            remove build, doc, test, and example artifacts"
	@echo "  make distclean        also remove the top-level build directory"
	@echo "  make clean-sanitize   remove sanitizer build artifacts"
	@echo ""
	@echo "Examples:"
	@echo "  make linux"
	@echo "  make linux USE_MPI=1"
	@echo "  make msys2"
	@echo "  make msys2 USE_MPI=1"
	@echo "  make html"
	@echo "  make check"
	@echo "  make test TEST_ARGS='--group smoke'"
	@echo ""

linux:
	@$(MAKE) -C src -f Makefile.linux $(filter-out $@,$(MAKECMDGOALS))

sanitize:
	@$(MAKE) -C src -f Makefile.linux BUILD_DIR=../build/sanitize \
		OPT='-O1 -g3' EXTRA_CXXFLAGS='$(SANITIZE_FLAGS)' \
		EXTRA_LDFLAGS='-fsanitize=address,undefined'

msys2:
	@$(MAKE) -C src -f Makefile.msys2 $(filter-out $@,$(MAKECMDGOALS))

html:
	@$(MAKE) -C doc -f Makefile html

doc: html

# Run the manifest-driven suites against a freshly built Linux executable.
# Recursive make keeps the source build from receiving the outer test/check goal.
test:
	@$(MAKE) linux
	@$(PYTHON) tests/run.py $(TEST_ARGS)


test-sanitize:
	@$(MAKE) sanitize
	@$(SANITIZE_ENV) $(PYTHON) tests/run.py --exe build/sanitize/packfall $(TEST_ARGS)

examples:
	@$(MAKE) linux
	@$(PYTHON) examples/run.py $(EXAMPLE_ARGS)

check:
	@$(MAKE) linux
	@$(PYTHON) tests/run.py $(TEST_ARGS)
	@$(PYTHON) examples/run.py $(EXAMPLE_ARGS)

check-sanitize:
	@$(MAKE) sanitize
	@$(SANITIZE_ENV) $(PYTHON) tests/run.py --exe build/sanitize/packfall $(TEST_ARGS)
	@$(SANITIZE_ENV) $(PYTHON) examples/run.py --exe build/sanitize/packfall $(EXAMPLE_ARGS)

clean-linux:
	@$(MAKE) -C src -f Makefile.linux clean

clean-msys2:
	@$(MAKE) -C src -f Makefile.msys2 clean

clean-sanitize:
	@$(MAKE) -C src -f Makefile.linux BUILD_DIR=../build/sanitize clean

clean-doc:
	@$(MAKE) -C doc -f Makefile clean

clean-tests:
	@$(PYTHON) tests/run.py --clean
	@find tests/cases -type f -name 'log.*' -delete

clean-examples:
	@$(PYTHON) examples/run.py --clean
	@find examples -type f -name 'log.*' -delete

clean: clean-linux clean-msys2 clean-sanitize clean-doc clean-tests clean-examples

distclean: clean
	@rm -rf build

%:
	@:
