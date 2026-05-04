.PHONY: help build package clean

help:
	@echo "Available targets:"
	@echo "  make build    - build tg5040 binary inside toolchain container"
	@echo "  make package  - create release zip"
	@echo "  make clean    - remove build artifacts"

build:
	bash scripts/build_tg5040_docker.sh

package: build
	bash scripts/package_pak.sh

clean:
	rm -rf build/tg5040
