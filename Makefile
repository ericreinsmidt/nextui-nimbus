.PHONY: build clean package

build:
	bash scripts/build_tg5040_docker.sh

package: build
	bash scripts/package_pak.sh

clean:
	rm -rf build/tg5040
