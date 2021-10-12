all: src/*
	@echo -n " * Building RMI server ..."
	@make server > /dev/null && echo 'ok' || echo 'failed'
	@echo -n " * Building RMI client ..."
	@make client > /dev/null && echo 'ok' || echo 'failed'
	@echo -n " * Building RMI proxy ..."
	@make proxy > /dev/null && echo 'ok' || echo 'failed'

server: src/server_main.c src/base64.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c src/client.c src/cloud-rmi.h src/config.h
	@echo " * Building rmi-server binary ..."
	@gcc -g src/server_main.c src/base64.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c -o rmi-server

client: src/client_main.c src/base64.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c src/client.c src/cloud-rmi.h src/config.h src/client.h
	@echo " * Building rmi-client binary ..."
	@gcc -g src/client_main.c src/base64.c src/cloud-rmi.c src/config.c src/handle_call.c src/protocol.c src/server.c src/client.c -o rmi-client

proxy: src/client_main.c src/base64.c src/cloud-rmi.c src/config.c src/handle_call.c src/server.c src/client.c src/cloud-rmi.h src/config.h src/client.h
	@echo " * Building rmi-client binary ..."
	@gcc -g -lm src/client_main.c src/base64.c src/cloud-rmi.c src/config.c src/protocol.c src/handle_call.c src/client.c src/server.c src/proxy.c -o rmi-proxy


install:
	@echo "Installing rmi-server ..."
	@cp rmi-server /usr/local/sbin/
	@echo "Installing rmi-client ..."
	@cp rmi-client /usr/local/sbin/

