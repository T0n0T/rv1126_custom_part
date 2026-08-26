GO ?= go
BIN := bin/gb28181-daemon

.PHONY: build test vet clean

build:
	$(GO) build -buildvcs=false -o $(BIN) ./cmd/gb28181-daemon

test:
	$(GO) test -buildvcs=false ./...

vet:
	$(GO) vet -buildvcs=false ./...

clean:
	$(RM) -rf bin
