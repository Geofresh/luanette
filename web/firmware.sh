#!/bin/sh
# Puts the latest release's firmware next to the playground (web/build/site/firmware), with the
# manifest ESP Web Tools installs it from. Run after build.sh; needs gh (signed in, or GH_TOKEN).
# Without a release it does nothing, and the page hides its Install button.
#
# The same four parts at the same offsets as the release's esptool command, never a merged image:
# each part erases only its own sectors, so saves, settings (NVS) and sent carts are kept.
# new_install_prompt_erase makes ESP Web Tools ask before it erases anything (unticked by default);
# without it, it erases the whole chip first.
set -e
cd "$(dirname "$0")"
REPO="${REPO:-Geofresh/luanette}"
out=build/site/firmware
rm -rf "$out" && mkdir -p "$out"
tag=$(gh release view --repo "$REPO" --json tagName -q .tagName 2>/dev/null) || { echo "no release: the page won't offer an install"; rmdir "$out"; exit 0; }
gh release download "$tag" --repo "$REPO" --dir "$out" \
  -p bootloader.bin -p partitions.bin -p ota_data_initial.bin -p luanette.bin
cat > "$out/manifest.json" <<EOF
{
  "name": "Luanette",
  "version": "${tag#v}",
  "new_install_prompt_erase": true,
  "new_install_improv_wait_time": 0,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "bootloader.bin", "offset": 0 },
        { "path": "partitions.bin", "offset": 32768 },
        { "path": "ota_data_initial.bin", "offset": 102400 },
        { "path": "luanette.bin", "offset": 131072 }
      ]
    }
  ]
}
EOF
echo "firmware $tag in $out"
