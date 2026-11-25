#!/usr/bin/env bash
# Creates a 32-bit ARM test VM for the T113 build (t113.toolchain): Ubuntu 24.04
# Server armhf with a 32-bit ARM kernel, so static ARM games run natively with
# the same kernel interfaces (framebuffer, evdev, console) a 32-bit ARM board has.
# User "sps" (password "sps"), logged in automatically on the VM window's text
# console; SSH from this machine on localhost:2223 (key ~/.ssh/sps-vm).
# The CPU is emulated (no KVM for ARM on an x86 PC): fine for checking that games
# work, useless for speed. Re-running it starts a fresh VM disk.
#
# Throwaway test VM, not a machine to trust: the "sps" account has the password
# "sps", passwordless sudo and an autologin console, and SSH and the screen are
# reachable on 127.0.0.1 only. Never give it a bridged network or a port on a
# real interface. Re-running this script discards the disk and starts fresh.
set -euo pipefail

VM_DIR=~/sps-vm-arm
BASE=https://cloud-images.ubuntu.com/noble/current
IMAGE=noble-server-cloudimg-armhf.img

mkdir -p "$VM_DIR"
cd "$VM_DIR"

# disk image, plus its kernel and initrd: QEMU boots the kernel directly
[ -f "$IMAGE" ] || curl -fL -o "$IMAGE" "$BASE/$IMAGE"
[ -f vmlinuz ] || curl -fL -o vmlinuz "$BASE/unpacked/noble-server-cloudimg-armhf-vmlinuz-generic"
[ -f initrd ] || curl -fL -o initrd "$BASE/unpacked/noble-server-cloudimg-armhf-initrd-generic"
[ -f ~/.ssh/sps-vm ] || ssh-keygen -q -t ed25519 -N "" -f ~/.ssh/sps-vm -C sps-vm

rm -f disk.qcow2
qemu-img create -q -f qcow2 -b "$IMAGE" -F qcow2 disk.qcow2 10G

cat > user-data <<EOF
#cloud-config
hostname: sps-arm-vm
users:
  - name: sps
    groups: [sudo, video, input]
    shell: /bin/bash
    sudo: ALL=(ALL) NOPASSWD:ALL
    lock_passwd: false
    plain_text_passwd: sps
    ssh_authorized_keys:
      - $(cat ~/.ssh/sps-vm.pub)
packages: [build-essential, cmake, ninja-build, git, python3-venv, pulseaudio]
write_files:
  # log "sps" in automatically on the VM window's console (tty1)
  - path: /etc/systemd/system/getty@tty1.service.d/autologin.conf
    content: |
      [Service]
      ExecStart=
      ExecStart=-/sbin/agetty --autologin sps --noclear %I \$TERM
runcmd:
  - [systemctl, daemon-reload]
  - [systemctl, restart, getty@tty1]
EOF
printf 'instance-id: sps-arm-vm-1\nlocal-hostname: sps-arm-vm\n' > meta-data
cloud-localds seed.iso user-data meta-data

echo "VM ready to start: $(dirname "$0")/run-arm-vm.sh"
