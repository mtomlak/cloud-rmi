ROOT_DIR := $(abspath ../..)
INTEGRATION_SCRIPT := $(ROOT_DIR)/tests/integration/run_integration.sh

all: test

test:
	cd $(ROOT_DIR) && $(INTEGRATION_SCRIPT)

clean:

.PHONY: all test clean
