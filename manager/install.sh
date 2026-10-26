#!/usr/bin/env bash
# Installs a `beer-manager` command and an application-grid entry for the current user.
set -euo pipefail
here="$(cd -- "$(dirname -- "$0")" && pwd)"
mkdir -p "$HOME/.local/bin" "$HOME/.local/share/applications"
cat > "$HOME/.local/bin/beer-manager" <<EOF
#!/usr/bin/env bash
exec python3 "$here/beer_manager.py" "\$@"
EOF
chmod +x "$HOME/.local/bin/beer-manager"
cat > "$HOME/.local/share/applications/dev.beer.Manager.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Beer
Comment=Game library, launcher and live controls for the Beer loader
Exec=$HOME/.local/bin/beer-manager
Icon=applications-games
Terminal=false
Categories=Game;
StartupWMClass=dev.beer.Manager
EOF
echo "Installed: beer-manager (command) and the Beer entry in the application grid."
