all: src/*
	@echo -n " * Updating source code ..."
	@make update > /dev/null && echo 'ok' || echo 'failed'
	@echo -n " * Building RMI server ..."
	@make server > /dev/null && echo 'ok' || echo 'failed'
	@echo -n " * Building RMI client ..."
	@make client > /dev/null && echo 'ok' || echo 'failed'
	@echo -n " * Building RMI proxy ..."
	@make proxy > /dev/null && echo 'ok' || echo 'failed'

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
	@cp rmi-server /usr/local/sbin/

client_install:
	@echo "Installing rmi-client ..."
	@cp rmi-client /usr/local/sbin/

proxy_install:
	@echo "Installing rmi-proxy ..."
	@cp rmi-proxy /usr/local/sbin/

install:
	@make server_install
	@make client_install
	@make proxy_install


