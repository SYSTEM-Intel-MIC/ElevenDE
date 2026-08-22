CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra
GDK      := $(shell pkg-config --exists gdk-pixbuf-2.0 && echo 1)
CFLAGS   += -Wno-format-truncation \
            $(shell pkg-config --cflags x11 xft fontconfig freetype2 libpng)
LDLIBS    = $(shell pkg-config --libs x11 xft fontconfig freetype2 libpng) -lm
ifeq ($(GDK),1)
CFLAGS   += -DHAVE_GDKPIXBUF=1 $(shell pkg-config --cflags gdk-pixbuf-2.0)
LDLIBS   += $(shell pkg-config --libs gdk-pixbuf-2.0)
endif

XTST     := $(shell pkg-config --exists xtst && echo 1)
ifeq ($(XTST),1)
CFLAGS   += -DHAVE_XTEST=1 $(shell pkg-config --cflags xtst)
LDLIBS   += $(shell pkg-config --libs xtst)
else
$(warning ==========================================================)
$(warning elevende-shell: libXtst NOT found - ghost window move)
$(warning recovery will be DISABLED. Fix: sudo apt install)
$(warning libxtst-dev libxi-dev, then rebuild.)
$(warning ==========================================================)
endif

XI       := $(shell pkg-config --exists xi && echo 1)
ifeq ($(XI),1)
CFLAGS   += -DHAVE_XINPUT2=1 $(shell pkg-config --cflags xi)
LDLIBS   += $(shell pkg-config --libs xi)
endif

DBUS_AVAIL := $(shell pkg-config --exists dbus-1 && echo yes)

all: elevende-shell elevende-lock
ifneq ($(DBUS_AVAIL),yes)
$(warning dbus-1 not found; building shell only (no notification daemon))
else
all: elevende-notifyd

elevende-notifyd: notifyd.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags dbus-1) -o $@ notifyd.c \
	    $(LDLIBS) $(shell pkg-config --libs dbus-1)
endif

elevende-shell: main.c
	$(CC) $(CFLAGS) -o $@ main.c $(LDLIBS)

# login/lock screen; installed setuid-root to read /etc/shadow.
# Links gdk-pixbuf (when present) so the Win11 logon shows the wallpaper.
LOCK_DEFS :=
ifeq ($(GDK),1)
LOCK_DEFS := -DHAVE_GDKPIXBUF=1 $(shell pkg-config --cflags gdk-pixbuf-2.0)
LOCK_LIBS := $(shell pkg-config --libs gdk-pixbuf-2.0)
endif
elevende-lock: lock.c
	$(CC) $(CFLAGS) $(LOCK_DEFS) \
	    $(shell pkg-config --cflags x11 xft fontconfig freetype2) \
	    -o $@ lock.c \
	    $(shell pkg-config --libs x11 xft fontconfig freetype2) $(LOCK_LIBS) -lcrypt

install: all
	install -Dm755 elevende-shell /usr/local/bin/elevende-shell
	install -m4755 elevende-lock /usr/local/bin/elevende-lock
ifneq ($(DBUS_AVAIL),yes)
	@echo "note: elevende-notifyd not built (missing dbus-1)"
else
	install -Dm755 elevende-notifyd /usr/local/bin/elevende-notifyd
endif

clean:
	rm -f elevende-shell elevende-notifyd elevende-lock

.PHONY: all clean install