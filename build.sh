#!/bin/sh
set -e

echo "=== AspectScale Build Helper ==="

MISSING_TOOLS=""
for tool in gcc make pkg-config; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        MISSING_TOOLS="$MISSING_TOOLS $tool"
    fi
done

if [ -n "$MISSING_TOOLS" ]; then
    echo "Missing build tools:$MISSING_TOOLS"
    echo ""
    echo "Please install build tools (gcc, make, pkg-config) using your package manager."
    echo ""
    exit 1
fi

# Check pkg-config packages
for pkg in gtk+-3.0 x11 xrandr; do
    if ! pkg-config --exists "$pkg"; then
        echo "Missing pkg-config library: $pkg"
        echo "Please install development headers (GTK 3, X11, Xrandr) using your package manager."
        exit 1
    fi
done

if ! pkg-config --exists ayatana-appindicator3-0.1 && ! pkg-config --exists appindicator3-0.1; then
    echo "Missing libayatana-appindicator or libappindicator development package!"
    echo "Please install libayatana-appindicator-devel (or equivalent) using your package manager."
    exit 1
fi

echo "All dependencies found. Compiling..."
make -B
echo ""
echo "Build successful! Binary: ./aspectscale"
echo "To install system-wide:"
echo "  sudo make install"
echo ""
echo "To run now:"
echo "  ./aspectscale &"
