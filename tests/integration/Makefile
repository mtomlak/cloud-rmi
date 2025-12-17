ROOT_DIR := $(abspath ../..)
INTEGRATION_SCRIPT := $(ROOT_DIR)/tests/integration/run_tests.py

all: test

test:
	cd $(ROOT_DIR) && $(INTEGRATION_SCRIPT)

clean:
	rm -rf $(ROOT_DIR)/tests/integration/.venv

.PHONY: all test clean
