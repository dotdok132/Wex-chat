.PHONY: all daemon test clean app

all: daemon

daemon:
	@echo "==> Building Wex-Chat libp2p daemon..."
	cd daemon && go build -o wex-daemon .
	@echo "[+] Successfully built daemon/wex-daemon"

test:
	@echo "==> Running libp2p P2P and DM end-to-end integration tests..."
	cd daemon && go test -v -race ./...

c-build:
	@echo "==> Building C99 core..."
	cmake -B build -DCMAKE_BUILD_TYPE=Debug
	cmake --build build -j$$(nproc)

c-test:
	@echo "==> Running C99 unit tests..."
	cd build && ctest --output-on-failure

app: daemon
	./scripts/wex-app

clean:
	rm -f daemon/wex-daemon
	rm -rf build
