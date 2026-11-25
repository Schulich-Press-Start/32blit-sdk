#!/usr/bin/env bash
# Creates the test VM for running 32blit-linux on a real framebuffer and keyboard:
# Ubuntu 24.04 Server, user "sps" (password "sps"), logged in automatically on the
# VM window's text console, SSH from this machine on localhost:2222 (key ~/.ssh/sps-vm).
# Run from WSL or any Linux with QEMU + KVM. Re-running it starts a fresh VM disk.
#
# Throwaway test VM, not a machine to trust: the "sps" account has the password
# "sps", passwordless sudo and an autologin console, and SSH and the screen are
# reachable on 127.0.0.1 only. Never give it a bridged network or a port on a
# real interface. Re-running this script discards the disk and starts fresh.
set -euo pipefail

VM_DIR=~/sps-vm
IMAGE=noble-server-cloudimg-amd64.img

mkdir -p "$VM_DIR"
cd "$VM_DIR"

[ -f "$IMAGE" ] || curl -fL -o "$IMAGE" "https://cloud-images.ubuntu.com/noble/current/$IMAGE"
[ -f ~/.ssh/sps-vm ] || ssh-keygen -q -t ed25519 -N "" -f ~/.ssh/sps-vm -C sps-vm

rm -f disk.qcow2
qemu-img create -q -f qcow2 -b "$IMAGE" -F qcow2 disk.qcow2 20G

cat > user-data <<EOF
#cloud-config
hostname: sps-vm
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
  # 32blit tools; bitstring 5 breaks them
  - [sudo, -H, -u, sps, bash, -c, "python3 -m venv ~/blitvenv && ~/blitvenv/bin/pip install -q 32blit 'bitstring<5'"]
  - [sudo, -H, -u, sps, bash, -c, "echo 'export PATH=~/blitvenv/bin:\$PATH' >> ~/.profile"]
  - [systemctl, daemon-reload]
  - [systemctl, restart, getty@tty1]
EOF
printf 'instance-id: sps-vm-1\nlocal-hostname: sps-vm\n' > meta-data
cloud-localds seed.iso user-data meta-data

echo "VM ready to start: $(dirname "$0")/run-vm.sh"
