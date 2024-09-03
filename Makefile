NAME=wvkbd-dbus
BIN=${NAME}
SRC=.
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
SHAREDIR ?= $(PREFIX)/share
AUTOSTARTDIR ?= /etc/xdg/autostart

PKGS = wayland-client glib-2.0 gio-unix-2.0

WVKBD_SOURCES += $(wildcard $(SRC)/*.c)
WVKBD_HEADERS += $(wildcard $(SRC)/*.h)

CFLAGS += -std=gnu99 -Wall -g -DWITH_WAYLAND_SHM
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDFLAGS += $(shell pkg-config --libs $(PKGS)) -lm -lutil -lrt

WAYLAND_HEADERS = $(wildcard proto/*.xml)

HDRS = $(WAYLAND_HEADERS:.xml=-client-protocol.h)
WAYLAND_SRC = $(HDRS:.h=.c)
SOURCES = $(WVKBD_SOURCES) $(WAYLAND_SRC)

OBJECTS = $(SOURCES:.c=.o)

all: ${BIN}

proto/%-client-protocol.c: proto/%.xml
	wayland-scanner code < $? > $@

proto/%-client-protocol.h: proto/%.xml
	wayland-scanner client-header < $? > $@

$(OBJECTS): $(HDRS) $(WVKBD_HEADERS)

wvkbd-dbus: $(OBJECTS)
	$(CC) -o wvkbd-dbus $(OBJECTS) $(LDFLAGS)

install:
	install -D wvkbd-dbus $(DESTDIR)$(BINDIR)/wvkbd-dbus
	install -D usr/bin/wvkbd-mobintl $(DESTDIR)$(BINDIR)/wvkbd-mobintl
	install -D etc/xdg/autostart/wvkbd-dbus.desktop $(DESTDIR)$(AUTOSTARTDIR)/wvkbd-dbus.desktop
	install -D usr/share/WHO53/wvkbd.desktop $(DESTDIR)$(SHAREDIR)/WHO53/wvkbd.desktop

clean:
	rm -f $(OBJECTS) $(HDRS) $(WAYLAND_SRC) ${BIN} usr/bin/wvkbd-dbus

format:
	clang-format -i $(WVKBD_SOURCES) $(WVKBD_HEADERS)
