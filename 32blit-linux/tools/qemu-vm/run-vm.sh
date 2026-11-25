#!/usr/bin/env bash
# Starts the test VM made by setup-vm.sh.
# - Games run on the VM's text console: /dev/fb0 and /dev/dri/card0 come from a
#   virtio GPU, the keyboard from a virtio keyboard (an evdev device in the VM).
#   Virtio rather than the default PS/2 keyboard because it auto-repeats held
#   keys like a real one, as the ARM VM's does.
# - SSH: ssh -i ~/.ssh/sps-vm -p 2222 sps@localhost
# - The boot log and a serial console stay on this terminal; Ctrl+A then X quits QEMU.
# - QEMU's monitor (screenshots, sending keys) listens on ~/sps-vm/monitor.sock.
#
# Where the VM's screen shows (VM_DISPLAY):
# - window (default): a QEMU window (WSLg on Windows). The screen is 640x480:
#   QEMU's window reports its own size to the VM, which then uses it.
# - browser: the screen is shown in a web page (noVNC, downloaded once into
#   ~/sps-vm), opened in the Windows browser. Only here does VM_SCREEN=WxH set
#   the screen size, e.g. VM_SCREEN=1280x720 or VM_SCREEN=480x272 (dev board
#   panel). Page: http://127.0.0.1:6080/vnc.html?host=127.0.0.1&port=5700&path=&autoconnect=true
#   (empty path: QEMU's websocket only answers on "/"; 127.0.0.1, not
#   localhost, which the browser may try as IPv6 and fail).
#   (QEMU's SDL window, the other way to get a chosen size, shows nothing under WSLg.)
set -euo pipefail
# The braces make bash read the whole script before running it, so editing
# this file while a VM is up can't break the running copy when QEMU exits.
{

MODE=${VM_DISPLAY:-window}
SCREEN=${VM_SCREEN:-640x480}
XRES=${SCREEN%x*}
YRES=${SCREEN#*x}

cd ~/sps-vm

case "$MODE" in
  window)
    if [ -n "${VM_SCREEN:-}" ]; then
      echo "VM_SCREEN needs VM_DISPLAY=browser; the window's screen is 640x480" >&2
    fi
    DISPLAY_ARGS=(-device virtio-vga -display gtk)
    ;;
  browser)
    NOVNC_VERSION=1.7.0
    NOVNC_SHA256=b1003a11b6e6e8d8f7f5e5586daae7f8ca651d8aee0aa155ff9ac841c48f52c6
    NOVNC=~/sps-vm/noVNC-$NOVNC_VERSION
    if [ ! -f "$NOVNC/vnc.html" ]; then
      # checked against its hash, as tools/get-t113-toolchain.sh and
      # tools/build-libdrm.sh do, rather than piping a download into tar
      curl -fsSL -o ~/sps-vm/novnc.tar.gz "https://github.com/novnc/noVNC/archive/refs/tags/v$NOVNC_VERSION.tar.gz"
      echo "$NOVNC_SHA256  $HOME/sps-vm/novnc.tar.gz" | sha256sum -c -
      tar -C ~/sps-vm -xzf ~/sps-vm/novnc.tar.gz
      rm -f ~/sps-vm/novnc.tar.gz
    fi

    # serves the noVNC page; the page talks to QEMU's VNC websocket directly
    python3 -m http.server 6080 --bind 127.0.0.1 --directory "$NOVNC" >/dev/null 2>&1 &
    WEB_PID=$!
    trap 'kill $WEB_PID 2>/dev/null' EXIT

    URL="http://127.0.0.1:6080/vnc.html?host=127.0.0.1&port=5700&path=&autoconnect=true"
    echo "VM screen ${XRES}x${YRES}: $URL"
    # open it in the Windows browser once QEMU is listening
    (sleep 3; cmd.exe /c start "" "$URL" >/dev/null 2>&1 || true) &

    DISPLAY_ARGS=(-vga none -device virtio-gpu-pci,xres="$XRES",yres="$YRES"
                  -display none -vnc 127.0.0.1:0,websocket=127.0.0.1:5700)
    ;;
  *)
    echo "VM_DISPLAY must be window or browser" >&2
    exit 1
    ;;
esac

# Audio backend: pulseaudio works on Fedora (PipeWire-compatible)
# es1370 device because snd-hda-intel module isn't in the Ubuntu cloud image;
# snd-ens1370 is the only sound driver available as a module.
AUDIO_ARGS=(-audiodev pa,id=audio0 -device es1370,audiodev=audio0)

qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 4G \
  -drive file=disk.qcow2,if=virtio \
  -drive file=seed.iso,if=virtio,format=raw \
  -nic user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:2222-:22 \
  "${DISPLAY_ARGS[@]}" -device virtio-keyboard-pci \
  "${AUDIO_ARGS[@]}" \
  -monitor unix:"$HOME/sps-vm/monitor.sock",server,nowait \
  -serial mon:stdio
exit
}
