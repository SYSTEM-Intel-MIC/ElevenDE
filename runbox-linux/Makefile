PREFIX ?= /usr/local
CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra
PKGS   := gtk+-3.0 x11

PC_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PC_LIBS   := $(shell pkg-config --libs $(PKGS))

all: runbox

runbox: src/runbox.c
	@pkg-config --exists $(PKGS) || { echo ""; echo "错误: 缺少 GTK3/X11 开发包，请先运行 ./install.sh 自动安装依赖"; echo ""; exit 1; }
	$(CC) $(CFLAGS) $(PC_CFLAGS) -o $@ $< $(PC_LIBS)

install: runbox
	install -Dm755 runbox $(DESTDIR)$(PREFIX)/bin/runbox
	install -Dm644 data/runbox.desktop $(DESTDIR)$(PREFIX)/share/applications/runbox.desktop
	install -Dm644 data/runbox.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/runbox.svg

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/runbox
	rm -f $(DESTDIR)$(PREFIX)/share/applications/runbox.desktop
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/runbox.svg

clean:
	rm -f runbox

.PHONY: all install uninstall clean
