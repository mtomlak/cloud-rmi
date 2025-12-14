all: libcloudrmi/libcloudrmi.a src/*
#       @echo -n "Updating source code ..."
#       @make update
	@echo -n "Building RMI server ..."
	make server
	@echo -n "Building RMI client ..."
	make client
	@echo -n "Building RMI proxy ..."
	make proxy

libcloudrmi/libcloudrmi.a:
	$(MAKE) -C libcloudrmi

update:
	git pull

server: libcloudrmi/libcloudrmi.a src/server_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c src/client.c src/cloud-rmi.h src/config.h
	@echo " * Building rmi-server binary ..."
	gcc -g -Ilibcloudrmi/include src/server_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c -Llibcloudrmi -lcloudrmi -lm -o rmi-server -D ENABLE_LINUX_FEATURES

client: libcloudrmi/libcloudrmi.a src/client_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c src/client.c src/cloud-rmi.h src/config.h src/client.h
	@echo " * Building rmi-client binary ..."
	gcc -g -Ilibcloudrmi/include src/client_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c src/client.c -Llibcloudrmi -lcloudrmi -lm -o rmi-client -D ENABLE_LINUX_FEATURES

proxy: libcloudrmi/libcloudrmi.a src/client_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/server.c src/client.c src/cloud-rmi.h src/config.h src/client.h
	@echo " * Building rmi-client binary ..."
	gcc -g -Ilibcloudrmi/include src/server_main.c src/cloud-rmi.c src/config.c src/handle_call.c src/client.c src/protocol.c src/server.c src/proxy.c -Llibcloudrmi -lcloudrmi -lm -o rmi-proxy -D PROXY_SUPPORT -D ENABLE_LINUX_FEATURES

server_install:
	@echo "Installing rmi-server ..."
	mkdir -p $(DESTDIR)/usr/local/sbin/
	cp rmi-server $(DESTDIR)/usr/local/sbin/
	strip $(DESTDIR)/usr/local/sbin/rmi-server
	chmod +x $(DESTDIR)/usr/local/sbin/rmi-server
	make config_install DESTDIR=$(DESTDIR)

client_install:
	@echo "Installing rmi-client ..."
	mkdir -p $(DESTDIR)/usr/local/sbin/
	cp rmi-client $(DESTDIR)/usr/local/sbin/
	strip $(DESTDIR)/usr/local/sbin/rmi-client
	chmod +x $(DESTDIR)/usr/local/sbin/rmi-client
	make config_install DESTDIR=$(DESTDIR)

proxy_install:
	@echo "Installing rmi-proxy ..."
	mkdir -p $(DESTDIR)/usr/local/sbin/
	cp rmi-proxy $(DESTDIR)/usr/local/sbin/
	strip $(DESTDIR)/usr/local/sbin/rmi-proxy
	chmod +x $(DESTDIR)/usr/local/sbin/rmi-proxy
	make config_install DESTDIR=$(DESTDIR)

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
	make server_install
	make client_install
	#make proxy_install

