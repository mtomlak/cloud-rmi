PHP ?= php
PHPIZE ?= phpize
COMPOSER ?= composer
COMPOSER_FLAGS ?= --no-ansi --no-interaction --no-progress
COMPOSER_BIN := $(shell command -v $(COMPOSER))
LIB_DIR := $(CURDIR)/../libcloudrmi
EXT_DIR := $(CURDIR)/ext/profiweb_cloudrmi
EXT_SO := $(EXT_DIR)/modules/profiweb_cloudrmi.so
COMPOSER_DIR := $(CURDIR)/cloudrmi

.PHONY: all build-ext clean test unit integration composer-install

all: build-ext

$(EXT_SO): $(LIB_DIR)/Makefile
	$(MAKE) -C $(LIB_DIR)
	cd $(EXT_DIR) && $(PHPIZE)
	cd $(EXT_DIR) && ./configure --with-profiweb_cloudrmi=$(LIB_DIR)
	$(MAKE) -C $(EXT_DIR)

build-ext: $(EXT_SO)

composer-install: build-ext
	cd $(COMPOSER_DIR) && $(PHP) -d extension=$(EXT_SO) $(COMPOSER_BIN) install $(COMPOSER_FLAGS)

clean:
	-$(MAKE) -C $(EXT_DIR) clean
	rm -f $(EXT_DIR)/configure $(EXT_DIR)/config.h $(EXT_DIR)/config.nice $(EXT_DIR)/Makefile
	rm -rf $(EXT_DIR)/autom4te.cache $(EXT_DIR)/acinclude.m4 $(EXT_DIR)/aclocal.m4 $(EXT_DIR)/build $(EXT_DIR)/include $(EXT_DIR)/mkinstalldirs
	rm -f $(EXT_SO)

UNIT_TESTS := $(wildcard tests/unit/*.php)
INTEGRATION_TESTS := $(wildcard tests/integration/*.php)

unit: build-ext composer-install
	@if [ -n "$(UNIT_TESTS)" ]; then \
		for test in $(UNIT_TESTS); do \
			echo "Running $$test"; \
			PROFIWEB_CLOUDRMI_EXT=$(EXT_SO) $(PHP) -d extension=$(EXT_SO) $$test || exit $$?; \
		done; \
	else \
		echo "No unit tests found"; \
	fi

integration: build-ext composer-install
	@if [ -n "$(INTEGRATION_TESTS)" ]; then \
		for test in $(INTEGRATION_TESTS); do \
			echo "Running $$test"; \
			PROFIWEB_CLOUDRMI_EXT=$(EXT_SO) $(PHP) -d extension=$(EXT_SO) $$test || exit $$?; \
		done; \
	else \
		echo "No integration tests found"; \
	fi

test: unit integration
