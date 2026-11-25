#!/usr/bin/env bash
# Starts the 32-bit ARM test VM made by setup-arm-vm.sh.
# - The VM's screen opens as a window (WSLg on Windows). Its text console is
#   where games run: /dev/fb0 (and /dev/dri/card0) come from the virtio-gpu
#   display, the keyboard from a virtio keyboard (an evdev device in the VM).
# - SSH: ssh -i ~/.ssh/sps-vm -p 2223 sps@localhost
# - The boot log and a serial console stay on this terminal; Ctrl+A then X quits QEMU.
# - QEMU's monitor (screenshots, sending keys) listens on ~/sps-vm-arm/monitor.sock.
# - Booting takes a few minutes: the ARM CPU is emulated. One CPU only: with
#   several, the emulated cross-CPU TLB flushes stall the kernel for minutes.
set -euo pipefail

cd ~/sps-vm-arm
# Audio backend: pulseaudio works on Fedora (PipeWire-compatible)
# es1370 device because snd-hda-intel module isn't in the Ubuntu cloud image;
# snd-ens1370 is the only sound driver available as a module.
AUDIO_ARGS=(-audiodev pa,id=audio0 -device es1370,audiodev=audio0)

exec qemu-system-arm -M virt,highmem=off -cpu cortex-a15 -smp 1 -m 2G \
  -kernel vmlinuz -initrd initrd \
  -append "root=LABEL=cloudimg-rootfs console=tty1 console=ttyAMA0" \
  -drive file=disk.qcow2,if=none,id=disk -device virtio-blk-pci,drive=disk \
  -drive file=seed.iso,if=none,id=seed,format=raw -device virtio-blk-pci,drive=seed \
  -nic user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:2223-:22 \
  -device virtio-gpu-pci -device virtio-keyboard-pci -display gtk \
  "${AUDIO_ARGS[@]}" \
  -monitor unix:"$HOME/sps-vm-arm/monitor.sock",server,nowait \
  -serial mon:stdio
