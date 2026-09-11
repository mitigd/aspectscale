CC ?= gcc
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share

APPINDICATOR_PKG := $(shell pkg-config --exists ayatana-appindicator3-0.1 && echo ayatana-appindicator3-0.1 || echo appindicator3-0.1)
PKG_DEPS := gtk+-3.0 $(APPINDICATOR_PKG) x11 xrandr xcomposite gl xfixes xcursor xtst

CFLAGS ?= -O2 -g -Wall -Wextra
CFLAGS += $(shell pkg-config --cflags $(PKG_DEPS))
LIBS := $(shell pkg-config --libs $(PKG_DEPS)) -lm -lpthread

SRCS := src/main.c src/x11_scale.c src/gl_scaler.c src/config.c src/notify.c
OBJS := $(SRCS:.c=.o)
TARGET := aspectscale

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -d $(DESTDIR)$(DATADIR)/applications
	install -m 644 aspectscale.desktop $(DESTDIR)$(DATADIR)/applications/aspectscale.desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(DATADIR)/applications/aspectscale.desktop

.PHONY: all clean install uninstall
