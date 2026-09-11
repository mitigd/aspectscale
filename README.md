# AspectScale (Lossless Scaling / Aspect-Corrected Fullscreen for Linux)

**AspectScale** is a lightweight native C application for Void Linux and X11 that brings **Lossless Scaling style aspect-corrected fullscreen** to any windowed application or game.

It uses a hardware-accelerated OpenGL/GLX pipeline (`GLX_EXT_texture_from_pixmap`) to capture and display windowed apps at your monitor's full resolution with exact aspect ratio correction, pure black letterbox/pillarbox borders, and zero desktop bleed-through.

---

## Features

- **Hardware OpenGL Scaler**: Covers top panels, taskbars, and docks unconditionally across any desktop environment (XFCE, GNOME, KDE, MATE, Openbox, etc.).
- **Automatic Cursor Hiding**: Hides the mouse cursor automatically while in fullscreen mode (toggleable in tray or CLI).
- **Auto-Scale on Launch (Remembered Apps)**:
  - Click **"Remember Active Window for Auto-Scale"** in the tray.
  - Whenever that app/game is opened or focused, AspectScale immediately and automatically blows it up to fullscreen!
  - Manage or remove remembered rules anytime from the tray menu.
  - Configuration saved to `~/.config/aspectscale/config.ini`.
- **Exact Aspect-Ratio Correction**: Automatically calculates and centers the viewport with pure black pillarbox/letterbox bars.
- **Zero-Latency Direct Input**: Mouse clicks/motion and keyboard strokes are mapped and forwarded directly to the game.
- **Instant Hotkeys**:
  - **`Ctrl + Alt + S`**: Toggle Fullscreen Scaling / Restore
  - **`Escape`**: Exit Fullscreen Scaling
  - **`Ctrl + Alt + R`**: Restore windowed mode

---

## Building & Installing

```bash
cd /home/mitigd/Projects/aspectscale
make
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

- **Scale to Fullscreen [Ctrl+Alt+S]**
- **Restore Windowed [Ctrl+Alt+R]**
- **Remember Active Window for Auto-Scale**: Adds the currently focused app to the auto-scale list.
- **Remembered Apps (Auto-Scale)**: Submenu listing all saved rules with one-click removal.
- **Hide Cursor in Fullscreen**: Checkbox to toggle cursor visibility.
- **Show Desktop Notifications**: Checkbox to toggle notifications.
