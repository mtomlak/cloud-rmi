PHP ?= php
PHPIZE ?= phpize
LIB_DIR := $(CURDIR)/../libcloudrmi
EXT_DIR := $(CURDIR)/ext/profiweb_cloudrmi
EXT_SO := $(EXT_DIR)/modules/profiweb_cloudrmi.so

.PHONY: all build-ext clean test unit integration

all: build-ext

$(EXT_SO): $(LIB_DIR)/Makefile
	$(MAKE) -C $(LIB_DIR)
	cd $(EXT_DIR) && $(PHPIZE)
	cd $(EXT_DIR) && ./configure --with-profiweb_cloudrmi=$(LIB_DIR)
	$(MAKE) -C $(EXT_DIR)

build-ext: $(EXT_SO)

clean:
	-$(MAKE) -C $(EXT_DIR) clean
	rm -f $(EXT_DIR)/configure $(EXT_DIR)/config.h $(EXT_DIR)/config.nice $(EXT_DIR)/Makefile
	rm -rf $(EXT_DIR)/autom4te.cache $(EXT_DIR)/acinclude.m4 $(EXT_DIR)/aclocal.m4 $(EXT_DIR)/build $(EXT_DIR)/include $(EXT_DIR)/mkinstalldirs
	rm -f $(EXT_SO)

UNIT_TESTS := $(wildcard tests/unit/*.php)
INTEGRATION_TESTS := $(wildcard tests/integration/*.php)

unit: build-ext
	@if [ -n "$(UNIT_TESTS)" ]; then \
		for test in $(UNIT_TESTS); do \
			echo "Running $$test"; \
			$(PHP) -d extension=$(EXT_SO) $$test || exit $$?; \
		done; \
	else \
		echo "No unit tests found"; \
	fi

integration: build-ext
	@if [ -n "$(INTEGRATION_TESTS)" ]; then \
		for test in $(INTEGRATION_TESTS); do \
			echo "Running $$test"; \
			$(PHP) -d extension=$(EXT_SO) $$test || exit $$?; \
		done; \
	else \
		echo "No integration tests found"; \
	fi

test: unit integration
