```text
    _                         _   ____            _      
   / \   ___ _ __   ___  ___ | |_/ ___|  ___ __ _| | ___ 
  / _ \ / __| '_ \ / _ \/ __|| __\___ \ / __/ _` | |/ _ \
 / ___ \\__ \ |_) |  __/ (__ | |_ ___) | (_| (_| | |  __/
/_/   \_\___/ .__/ \___|\___| \__|____/ \___\__,_|_|\___|
            |_|                                          

==================================================================
  Hardware-Accelerated Aspect-Corrected Fullscreen Scaler (X11)
==================================================================
```

# AspectScale

**AspectScale** is a lightweight native C application for Linux and X11 that brings **hardware-accelerated aspect-corrected fullscreen scaling** to any windowed application or game.

It uses a high-performance OpenGL/GLX pipeline (`GLX_EXT_texture_from_pixmap`) to capture and display windowed apps at your monitor's full resolution with exact aspect ratio correction, pure black letterbox/pillarbox borders, and zero desktop bleed-through.

---

## Features

- **Hardware OpenGL Scaler**: Covers top panels, taskbars, and docks unconditionally across any desktop environment (XFCE, GNOME, KDE, MATE, Openbox, etc.).
- **Automatic Cursor Hiding**: Hides the mouse cursor automatically while in fullscreen mode (toggleable in tray or CLI).
- **Auto-Scale on Launch (Remembered Apps)**:
  - Click **"Remember Active Window for Auto-Scale"** in the tray.
  - Whenever that app/game is opened or focused, AspectScale immediately and automatically blows it up to fullscreen!
  - Manage or remove remembered rules anytime from the tray menu.
  - Configuration saved to `~/.config/aspectscale/config.ini`.
- **Exact Aspect-Ratio & Integer Scaling**:
  - **Filtering (Bilinear)**: Smooth aspect-ratio corrected fullscreen scaling.
  - **No Filtering (Nearest Neighbor)**: Sharp, unfiltered aspect-ratio scaling without bilinear blur.
  - **Integer Scaling**: Pixel-perfect integer ratio scaling ($1\times, 2\times, 3\times\dots$) centered with pure black borders.
- **Scaled Input**: Mouse clicks/motion and keyboard strokes are mapped to the game. While native popup menus are open, the application receives original pointer events and the scaler draws the cursor at its scaled position. Cursor movement is magnified with the menu during this interaction; its desktop position is restored when the menus close. This requires XFixes 4 or newer.
- **Instant Hotkeys**:
  - **`Ctrl + Alt + F`**: Toggle Fullscreen Scaling / Restore
  - **`Ctrl + Alt + S`**: Cycle Windowed Scaling ($2\times, 3\times, 4\times\dots$, centers on the monitor and cycles back to $1\times$ at the maximum fit)
  - **`Ctrl + Alt + R`**: Restore windowed mode ($1\times$)
  - **`Escape`**: Dismiss an open native menu; otherwise exit scaling

---

## Building & Installing

```bash
cd /home/mitigd/Projects/aspectscale
make
```

Regression tests:

```bash
make test       # Focus/ownership protocol tests; no display required
make test-x11   # Private Xvfb display; requires Xvfb, xauth and GLX support
```

Install for current user (`~/.local/bin` and desktop entry):

```bash
make install-user
```

Install system-wide:

```bash
sudo make install
```

Run in the background:

```bash
aspectscale &
```

---

## Tray Menu Options

- **Scale to Fullscreen [Ctrl+Alt+F]**: Scales active window to full screen resolution with hardware OpenGL aspect correction.
- **Scale Window (Next) [Ctrl+Alt+S]**: Cycles active window through $2\times, 3\times, 4\times\dots$ integer zoom without exceeding monitor resolution.
- **Window Scale Preset**: Submenu with direct scale options:
  - **1x (Restore Original)**
  - **2x**
  - **3x**
  - **4x**
  - **5x**
  - **Max Fit (Fill Screen Integer)**
- **Restore Windowed [Ctrl+Alt+R]**: Restores window to standard unscaled ($1\times$) state.
- **Remember Active Window for Auto-Scale**: Adds the currently focused app to the auto-scale list.
- **Remembered Apps (Auto-Scale)**: Submenu listing all saved rules with one-click removal.
- **Scaling Mode**: Submenu with live radio selection:
  - **Filtering (Bilinear / Smooth)**
  - **No Filtering (Nearest Neighbor / Sharp)**
  - **Integer Scaling (Pixel-Perfect)**
- **Hide Cursor in Fullscreen**: Checkbox to toggle cursor visibility.
- **Show Desktop Notifications**: Checkbox to toggle notifications.
