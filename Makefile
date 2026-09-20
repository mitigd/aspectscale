CC ?= gcc
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share

APPINDICATOR_PKG := $(shell pkg-config --exists ayatana-appindicator3-0.1 && echo ayatana-appindicator3-0.1 || echo appindicator3-0.1)
PKG_DEPS := gtk+-3.0 $(APPINDICATOR_PKG) x11 xrandr xcomposite gl xfixes xcursor xtst

CFLAGS ?= -O2 -g -Wall -Wextra
CFLAGS += $(shell pkg-config --cflags $(PKG_DEPS))
LIBS := $(shell pkg-config --libs $(PKG_DEPS)) -lm -lpthread

SRCS := src/main.c src/x11_scale.c src/gl_scaler.c src/popup_input.c src/config.c src/notify.c
OBJS := $(SRCS:.c=.o)
TARGET := aspectscale

all: $(TARGET)

# Protocol tests use Xlib stubs and do not need a running X server.
test:
	@set -e; test_bin=$$(mktemp /tmp/aspectscale-test.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections \
	    tests/scaler_window_test.c -Wl,--gc-sections -o "$$test_bin" $(LIBS); \
	"$$test_bin"

# Every test runs on a private display; never inject events into the desktop.
test-x11: $(OBJS)
	@set -e; test_dir=$$(mktemp -d /tmp/aspectscale-x11.XXXXXX); \
	trap 'rm -rf "$$test_dir"' EXIT; \
	$(CC) $(CFLAGS) tests/popup_input_test.c src/popup_input.o \
	    -o "$$test_dir/popup" $(LIBS); \
	$(CC) $(CFLAGS) tests/scaler_menu_test.c $(filter-out src/main.o,$(OBJS)) \
	    -o "$$test_dir/scaler" $(LIBS); \
	$(CC) $(CFLAGS) tests/virtual_desktop_test.c $(filter-out src/main.o,$(OBJS)) \
	    -o "$$test_dir/virtual" $(LIBS); \
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections tests/overlay_stack_test.c \
	    -Wl,--gc-sections -o "$$test_dir/stack" $(LIBS); \
	xvfb-run -a -s '-screen 0 1280x1024x24 -nolisten tcp' \
	    sh -ec '"$$3"; "$$1"; "$$2" 2; "$$2" 3; "$$2" 0; "$$4" 2; "$$4" 0' sh "$$test_dir/popup" "$$test_dir/scaler" "$$test_dir/stack" "$$test_dir/virtual"

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
	sed -e 's|Exec=aspectscale|Exec=$(BINDIR)/$(TARGET)|' \
	    aspectscale.desktop > $(DESTDIR)$(DATADIR)/applications/aspectscale.desktop

install-user: $(TARGET)
	install -d $(HOME)/.local/bin
	install -m 755 $(TARGET) $(HOME)/.local/bin/$(TARGET)
	install -d $(HOME)/.local/share/applications
	sed -e 's|Exec=aspectscale|Exec=$(HOME)/.local/bin/$(TARGET)|' \
	    aspectscale.desktop > $(HOME)/.local/share/applications/aspectscale.desktop
	update-desktop-database $(HOME)/.local/share/applications/ >/dev/null 2>&1 || true

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(DATADIR)/applications/aspectscale.desktop

.PHONY: all test test-x11 clean install uninstall
