CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
PLUGIN_DIR ?= $(HOME)/.config/bbhost/plugins

savebak.so: savebak.c bbhost_plugin.h
	$(CC) -shared -fPIC $(CFLAGS) -o $@ savebak.c

install: savebak.so
	install -D -m 644 savebak.so $(PLUGIN_DIR)/savebak.so

clean:
	rm -f savebak.so

.PHONY: install clean
