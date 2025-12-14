CC ?= gcc

BIN_SERVER = rmi-server
BIN_CLIENT = rmi-client
BIN_PROXY = rmi-proxy

LIBCLOUDRMI = libcloudrmi/libcloudrmi.a
LIBCLOUDRMI_INC = -Ilibcloudrmi/include
LIBCLOUDRMI_LDFLAGS = -Llibcloudrmi -lcloudrmi

all: $(LIBCLOUDRMI) src/*
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

update:
	git pull

server: $(LIBCLOUDRMI) src/server_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c src/client.c src/cloud-rmi.h src/config.h
	@echo " * Building rmi-server binary ..."
	$(CC) -g $(LIBCLOUDRMI_INC) src/server_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c $(LIBCLOUDRMI_LDFLAGS) -lm -o $(BIN_SERVER) -D ENABLE_LINUX_FEATURES

client: $(LIBCLOUDRMI) src/client_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c src/client.c src/cloud-rmi.h src/config.h src/client.h
	@echo " * Building rmi-client binary ..."
	$(CC) -g $(LIBCLOUDRMI_INC) src/client_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c src/client.c $(LIBCLOUDRMI_LDFLAGS) -lm -o $(BIN_CLIENT) -D ENABLE_LINUX_FEATURES

proxy: $(LIBCLOUDRMI) src/client_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/server.c src/client.c src/cloud-rmi.h src/config.h src/client.h
	@echo " * Building rmi-client binary ..."
	$(CC) -g $(LIBCLOUDRMI_INC) src/server_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/client.c src/protocol.c src/server.c src/proxy.c $(LIBCLOUDRMI_LDFLAGS) -lm -o $(BIN_PROXY) -D PROXY_SUPPORT -D ENABLE_LINUX_FEATURES

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

clean:
	$(MAKE) -C libcloudrmi clean
	rm -f $(BIN_SERVER) $(BIN_CLIENT) $(BIN_PROXY)

.PHONY: all update server client proxy server_install client_install proxy_install config_install install clean
