CC ?= gcc
CPPFLAGS ?=

BIN_SERVER = rmi-server
BIN_CLIENT = rmi-client
BIN_PROXY = rmi-proxy

LIBCLOUDRMI = libcloudrmi/libcloudrmi.a
LIBCLOUDRMI_INC = -Ilibcloudrmi/include
LIBCLOUDRMI_LDFLAGS = -Llibcloudrmi -lcloudrmi
APP_VERSION_HEADER = src/app_version.h

all: app_version $(LIBCLOUDRMI) src/*
	#@echo -n "Updating source code ..."
	#@make update
	@echo -n "Building RMI server ..."
	$(MAKE) server
	@echo -n "Building RMI client ..."
	$(MAKE) client
	@echo -n "Building RMI proxy ..."
	$(MAKE) proxy

$(LIBCLOUDRMI):
	$(MAKE) -C libcloudrmi

.PHONY: app_version
app_version:
	@if command -v git > /dev/null 2>&1 && git rev-parse --git-dir > /dev/null 2>&1; then \
	  version=$$(git tag --sort=-v:refname | head -n 1 | sed 's/v//g'); \
	else \
	  version=""; \
	fi; \
	if [ -z "$$version" ]; then version="0.0.0.0"; fi; \
	tmp="$(APP_VERSION_HEADER).tmp"; \
	{ echo "#ifndef APP_VERSION_H"; \
	  echo "#define APP_VERSION_H"; \
	  echo ""; \
	  echo "#define APP_NAME \"CloudRMI\""; \
	  echo "#define APP_VERSION \"$$version\""; \
	  echo ""; \
	  echo "#endif /* APP_VERSION_H */"; } > "$$tmp"; \
	if [ ! -f "$(APP_VERSION_HEADER)" ] || ! cmp -s "$$tmp" "$(APP_VERSION_HEADER)"; then \
	  mv "$$tmp" "$(APP_VERSION_HEADER)"; \
	else \
	  rm "$$tmp"; \
	fi

update:
	git pull

server: app_version $(LIBCLOUDRMI) src/server_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c src/client.c src/capabilities.c src/cloud-rmi.h src/config.h
	@echo " * Building rmi-server binary ..."
	$(CC) -g $(CPPFLAGS) $(LIBCLOUDRMI_INC) src/server_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c src/capabilities.c $(LIBCLOUDRMI_LDFLAGS) -lm -o $(BIN_SERVER)

client: app_version $(LIBCLOUDRMI) src/client_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c src/client.c src/capabilities.c src/cloud-rmi.h src/config.h src/client.h
	@echo " * Building rmi-client binary ..."
	$(CC) -g $(CPPFLAGS) $(LIBCLOUDRMI_INC) src/client_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c src/client.c src/capabilities.c $(LIBCLOUDRMI_LDFLAGS) -lm -o $(BIN_CLIENT)

proxy: app_version $(LIBCLOUDRMI) src/client_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/server.c src/client.c src/capabilities.c src/cloud-rmi.h src/config.h src/client.h
	@echo " * Building rmi-client binary ..."
	$(CC) -g $(CPPFLAGS) $(LIBCLOUDRMI_INC) src/server_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/client.c src/protocol.c src/server.c src/proxy.c src/capabilities.c $(LIBCLOUDRMI_LDFLAGS) -lm -o $(BIN_PROXY) -D PROXY_SUPPORT

server_install:
	@echo "Installing rmi-server ..."
	mkdir -p $(DESTDIR)/usr/local/sbin/
	cp $(BIN_SERVER) $(DESTDIR)/usr/local/sbin/
	strip $(DESTDIR)/usr/local/sbin/$(BIN_SERVER)
	chmod +x $(DESTDIR)/usr/local/sbin/$(BIN_SERVER)
	$(MAKE) config_install DESTDIR=$(DESTDIR)

client_install:
	@echo "Installing rmi-client ..."
	mkdir -p $(DESTDIR)/usr/local/sbin/
	cp $(BIN_CLIENT) $(DESTDIR)/usr/local/sbin/
	strip $(DESTDIR)/usr/local/sbin/$(BIN_CLIENT)
	chmod +x $(DESTDIR)/usr/local/sbin/$(BIN_CLIENT)
	$(MAKE) config_install DESTDIR=$(DESTDIR)

proxy_install:
	@echo "Installing rmi-proxy ..."
	mkdir -p $(DESTDIR)/usr/local/sbin/
	cp $(BIN_PROXY) $(DESTDIR)/usr/local/sbin/
	strip $(DESTDIR)/usr/local/sbin/$(BIN_PROXY)
	chmod +x $(DESTDIR)/usr/local/sbin/$(BIN_PROXY)
	$(MAKE) config_install DESTDIR=$(DESTDIR)

config_install:
	@echo "Installing configuration files ..."
	mkdir -p $(DESTDIR)/etc/init.d/
	cp etc/init.d/cloud-rmi $(DESTDIR)/etc/init.d/
	chmod +x $(DESTDIR)/etc/init.d/cloud-rmi

	mkdir -p $(DESTDIR)/etc/monit.d/
	cp etc/monit.d/cloud-rmi.conf $(DESTDIR)/etc/monit.d/

	mkdir -p $(DESTDIR)/etc/cloud/
	cp -r config/* $(DESTDIR)/etc/cloud/

install:
	$(MAKE) server_install
	$(MAKE) client_install
	#make proxy_install

test: all
	$(MAKE) -C libcloudrmi/tests/unit all
	cd libcloudrmi/tests/unit && ./protocol_feed_test
	cd libcloudrmi/tests/unit && ./proxy_payload_test
	$(MAKE) -C tests/integration test

clean:
	$(MAKE) -C libcloudrmi clean
	rm -f $(APP_VERSION_HEADER)
	rm -f $(BIN_SERVER) $(BIN_CLIENT) $(BIN_PROXY)

.PHONY: all update server client proxy server_install client_install proxy_install config_install install clean test app_version
