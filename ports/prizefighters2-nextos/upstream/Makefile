# Prizefighters 2 universal public-release profile.
# The single ARM64 executable is deliberately capped at GLIBC_2.30.

SOURCES := $(wildcard src/*.c src/*.h)

.PHONY: all test package dist clean

all: pf2-universal

pf2-universal: build_universal.sh $(SOURCES)
	./build_universal.sh

test:
	python3 nxextract.py recipe-check --recipe extractor.json
	python3 tests/test_import_owned_save.py
	bash tests/test_nxextract_runtime_env.sh
	bash -n pf2-nextos.sh run-extractor.sh nxextract-runtime-env.sh import-owned-save.sh
	sh -n "Prizefighters 2.sh"

package: all test
	PF2_SKIP_BUILD=1 ./package/build-package.sh

dist: package

clean:
	$(RM) pf2-universal
	$(RM) -r .build
