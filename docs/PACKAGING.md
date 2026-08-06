# Packaging

Controller-Box can be installed two ways: Flatpak (primary) or tarball
(fallback for any distro). Both produce the same install layout.

## Install layout

```
/usr/bin/controller-box                                        ← one binary, two modes
/usr/share/controller-box/controller-box.service               ← systemd user service (reference template)
/usr/share/controller-box/icons/svg/                            ← default controller icons
/usr/share/controller-box/controller-icons.yaml                 ← icon mapping table
/usr/share/applications/controller-box-manager.desktop          ← desktop entry (launches manager)
```

The systemd user service is also installed at runtime by the manager to
`~/.config/systemd/user/controller-box.service` on first run. The installed
template at `/usr/share/controller-box/controller-box.service` is a reference
copy; the manager generates the unit content dynamically (adjusting the
ExecStart path for Flatpak if needed).

## Tarball install (v1 fallback)

### Build from source

```bash
# Install build dependencies (example for Debian/Ubuntu):
sudo apt install build-essential cmake pkg-config \
    libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev \
    libsystemd-dev libyaml-dev

# Configure and build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run tests (optional)
cd build && ctest --output-on-failure && cd ..

# Install to a staging directory for testing
cmake --install build --prefix /tmp/test-install

# Verify install layout
tests/test_packaging_install.sh /tmp/test-install

# Install system-wide (default prefix: /usr)
sudo cmake --install build
```

### Dependencies

| Dependency | Debian/Ubuntu package | Fedora |
|---|---|---|
| SDL2 | libsdl2-dev | SDL2-devel |
| SDL2_ttf | libsdl2-ttf-dev | SDL2_ttf-devel |
| SDL2_image | libsdl2-image-dev | SDL2_image-devel |
| systemd (sd-bus) | libsystemd-dev | systemd-devel |
| libyaml | libyaml-dev | libyaml-devel |
| cmocka (tests only) | libcmocka-dev | libcmocka-devel |
| nanosvg | vendored in source tree | vendored in source tree |

### Post-install steps

1. **Install InputPlumber** — Controller-Box requires InputPlumber running as a
   system service. Install it first from its own package.
2. **Enable the overlay service** — Open the manager
   (`controller-box --manager`), go to Settings, and install the systemd user
   service. Alternatively, manually copy the service template and enable:
   ```bash
   cp /usr/share/controller-box/controller-box.service ~/.config/systemd/user/
   systemctl --user daemon-reload
   systemctl --user enable --now controller-box
   ```
3. **Add your user to the inputplumber group** (if using polkit):
   ```bash
   sudo usermod -aG inputplumber $USER
   # Log out and back in for group changes to take effect.
   ```

### Service unit

The systemd user service unit declares (per SPEC §2.4):

```ini
[Unit]
Description=Controller-Box Overlay Service
After=inputplumber.service
Requires=inputplumber.service

[Service]
ExecStart=/usr/bin/controller-box --overlay-service
Restart=always

[Install]
WantedBy=default.target
```

The `After=` / `Requires=` directives create a hard dependency on
InputPlumber's system service. If InputPlumber is not running, the overlay
service will not start until it is.

## Flatpak

See Task 42 for the Flatpak manifest. The Flatpak build uses the same CMake
build system. The manager installs the service at first run using
`flatpak run <app-id> --overlay-service` as the ExecStart line.

## Version

The version is embedded at build time from CMake's `project(VERSION ...)`.
Run `controller-box --version` to check the installed version. See Task 43
for version embedding details.

## Post-v1 roadmap

- .deb (amd64 + arm64)
- .rpm
- Nix derivation
- AUR
- Batocera package (.BATOEXEC)
- Eventual upstream Batocera integration