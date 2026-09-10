SOURCES := $(wildcard src/*.c src/*.h)

.PHONY: all universal test manifest package clean

all: build/hitmango-nextos-dev

build/hitmango-nextos-dev: build.sh $(SOURCES)
	./build.sh

universal: build/hitmango-nextos

build/hitmango-nextos: build_universal.sh $(SOURCES)
	./build_universal.sh

manifest: universal
	./package/refresh-release-manifest.py

test:
	./tests/run-host-tests.sh

package: test universal
	./package/refresh-release-manifest.py
	./package/build-package.sh
	./package/test-final-zip.sh dist/v1.2.2/hitmango.zip

clean:
	$(RM) -r build dist
	$(RM) hitmango-universal
	$(RM) -r .build
