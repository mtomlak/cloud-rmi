all: src/*
#	@echo -n "Updating source code ..."
#	@make update
	@echo -n "Building RMI server ..."
	make server
	@echo -n "Building RMI client ..."
	make client
	@echo -n "Building RMI proxy ..."
	make proxy

update:
	git pull

server: src/server_main.c src/base64.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c src/client.c src/cloud-rmi.h src/config.h
	@echo " * Building rmi-server binary ..."
	gcc -g -lm src/server_main.c src/base64.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c -o rmi-server -D ENABLE_LINUX_FEATURES

client: src/client_main.c src/base64.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c src/client.c src/cloud-rmi.h src/config.h src/client.h
	@echo " * Building rmi-client binary ..."
	gcc -g -lm src/client_main.c src/base64.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c src/client.c -o rmi-client -D ENABLE_LINUX_FEATURES

proxy: src/client_main.c src/base64.c src/cloud-rmi.c src/config.c src/handle_call.c src/server.c src/client.c src/cloud-rmi.h src/config.h src/client.h
	@echo " * Building rmi-client binary ..."
	gcc -g -lm src/server_main.c src/base64.c src/cloud-rmi.c src/config.c src/handle_call.c src/client.c src/protocol.c src/server.c src/proxy.c -o rmi-proxy -D PROXY_SUPPORT -D ENABLE_LINUX_FEATURES

server_install:
	@echo "Installing rmi-server ..."
	cp rmi-server /usr/local/sbin/
	strip /usr/local/sbin/rmi-server
	chmod +x /usr/local/sbin/rmi-server
	make config_install

client_install:
	@echo "Installing rmi-client ..."
	cp rmi-client /usr/local/sbin/
	strip /usr/local/sbin/rmi-client
	chmod +x /usr/local/sbin/rmi-client
	make config_install

proxy_install:
	@echo "Installing rmi-proxy ..."
	cp rmi-proxy /usr/local/sbin/
	strip /usr/local/sbin/rmi-proxy
	chmod +x /usr/local/sbin/rmi-proxy
	make config_install

config_install:
	@echo "Installing configuration files ..."
	if [ ! -f /etc/init.d/cloud-rmi ]; then \
		cp etc/init.d/cloud-rmi /etc/init.d/; \
		chmod +x /etc/init.d/cloud-rmi; \
	fi

	if [ ! -f /etc/monit.d/cloud-rmi.conf ]; then \
		cp etc/monit.d/cloud-rmi.conf /etc/monit.d/; \
	fi

	if [ ! -d /etc/cloud ]; then \
		mkdir -p /etc/cloud; \
		cp -r config/* /etc/cloud/; \
	fi

install:
	make server_install
	make client_install
	#make proxy_install


