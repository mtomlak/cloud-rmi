LIBCLOUDRMI_SRC=libcloudrmi/src/cloud-rmi.c libcloudrmi/src/base64.c libcloudrmi/src/rmi_connection.c libcloudrmi/src/rmi_payload.c libcloudrmi/src/rmi_protocol.c libcloudrmi/src/rmi_server.c libcloudrmi/src/rmi_error.c libcloudrmi/src/rmi_connection_handlers.c
LIBCLOUDRMI_OBJ=$(LIBCLOUDRMI_SRC:.c=.o)
CFLAGS=-g -I./src -I./libcloudrmi/include

all: libcloudrmi/libcloudrmi.a
	@echo -n "Building RMI server ..."
	make server
	@echo -n "Building RMI client ..."
	make client
	@echo -n "Building RMI proxy ..."
	make proxy

update:
	git pull

libcloudrmi/libcloudrmi.a: $(LIBCLOUDRMI_OBJ)
	@echo " * Building libcloudrmi static library ..."
	ar rcs $@ $(LIBCLOUDRMI_OBJ)

libcloudrmi/src/%.o: libcloudrmi/src/%.c
	gcc $(CFLAGS) -c $< -o $@ -D ENABLE_LINUX_FEATURES

server: src/server_main.c src/config.c src/server.c src/client.c src/cloud-rmi.h src/config.h libcloudrmi/libcloudrmi.a
	@echo " * Building rmi-server binary ..."
	gcc -g -I./src -I./libcloudrmi/include src/server_main.c src/config.c src/server.c src/client.c libcloudrmi/libcloudrmi.a -lm -o rmi-server -D ENABLE_LINUX_FEATURES

client: src/client_main.c src/config.c src/server.c src/client.c src/cloud-rmi.h src/config.h src/client.h libcloudrmi/libcloudrmi.a
	@echo " * Building rmi-client binary ..."
	gcc -g -I./src -I./libcloudrmi/include src/client_main.c src/config.c src/server.c src/client.c libcloudrmi/libcloudrmi.a -lm -o rmi-client -D ENABLE_LINUX_FEATURES

proxy: src/client_main.c src/config.c src/server.c src/client.c src/cloud-rmi.h src/config.h src/client.h libcloudrmi/libcloudrmi.a
	@echo " * Building rmi-client binary ..."
	gcc -g -I./src -I./libcloudrmi/include src/server_main.c src/config.c src/client.c src/server.c src/proxy.c libcloudrmi/libcloudrmi.a -lm -o rmi-proxy -D PROXY_SUPPORT -D ENABLE_LINUX_FEATURES

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
