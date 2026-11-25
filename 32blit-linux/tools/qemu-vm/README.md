# Test VMs for `32blit-linux`

The port talks to a real framebuffer (`/dev/fb0`), a real DRM device
(`/dev/dri/card0`), real evdev input devices and a real text console. WSL2 has
none of those, so it can build the port and run the unit tests but cannot run a
game. These scripts build two QEMU virtual machines that can, which is how the
port was developed and tested before any hardware existed.

| Script | What it makes |
|--------|---------------|
| `setup-vm.sh`, `run-vm.sh` | x86-64 VM with KVM. Fast; the one to use day to day |
| `setup-arm-vm.sh`, `run-arm-vm.sh` | 32-bit ARM VM, for the static ARM build from `t113.toolchain`. The CPU is emulated, so it proves games work and says nothing about speed |

> **Throwaway machines, not machines to trust.** The `sps` account has the
> password `sps`, passwordless sudo and an autologin console. SSH, the QEMU
> monitor and the screen are bound to `127.0.0.1` only, and they must stay that
> way: no bridged networking, no ports on a real interface. Re-running a setup
> script discards the disk and starts fresh.

## Once per machine

```sh
sudo apt install -y qemu-system-x86 qemu-system-arm qemu-utils cloud-image-utils
sudo usermod -aG kvm "$USER"      # then, from Windows: wsl --shutdown
```

## Using them

```sh
./setup-vm.sh                 # downloads the Ubuntu cloud image, builds the disk
./run-vm.sh                   # boots it; the screen opens in a QEMU window

ssh -i ~/.ssh/sps-vm -p 2222 sps@localhost    # 2223 for the ARM VM
```

`setup-vm.sh` generates the SSH key at `~/.ssh/sps-vm` the first time and puts
only the public half into the VM. Everything lives in `~/sps-vm` (and
`~/sps-vm-arm`), outside the repository.

**Games run on the VM's own text console (`tty1`), not over SSH.** There is no
framebuffer on an SSH session. Build over SSH, run on the console.

### Choosing the screen size

```sh
VM_DISPLAY=browser VM_SCREEN=480x272 ./run-vm.sh   # the dev board's panel
VM_DISPLAY=browser VM_SCREEN=1280x720 ./run-vm.sh
```

The default QEMU window is **always 640x480** whatever you ask for, because the
window reports its own size to the guest and the guest believes it. Browser mode
serves the screen as a web page (noVNC, downloaded once and checked against its
hash) and honours any size, which is the only way to test the port's scaling and
borders. QEMU's SDL window, the other way to pick a size, draws nothing at all
under WSLg.

### Driving it from a script

QEMU's monitor socket at `~/sps-vm/monitor.sock` takes screenshots and sends key
presses, which is how the port's test runs were automated:

```sh
printf 'screendump /tmp/vm.ppm\n' | socat - UNIX-CONNECT:$HOME/sps-vm/monitor.sock
printf 'sendkey z\n'              | socat - UNIX-CONNECT:$HOME/sps-vm/monitor.sock
```

## Things already paid for

- **`qemu-user` is not a substitute.** Static ARM binaries run and draw under
  `qemu-arm`, but it does not translate `EVIOCGBIT` or `KDGETMODE`, so there is
  no input and the console is not hidden. It only answers "does it start".
- **The ARM VM must stay on one vCPU.** With more, emulated cross-CPU TLB
  flushes stall the kernel and the first boot never finishes. Expect a few
  minutes to boot even so.
- **Virtual GPUs fake their vblank**, completing page flips immediately. The
  port caps itself at 90% of the mode's refresh period, so a VM shows about
  67 fps at 60 Hz. Tearing and real pacing can only be judged on hardware.
- **Audio:** both VMs now include an Intel HDA sound device backed by PulseAudio
  (works on Fedora/WSL with PipeWire). The `audio-test` and `waveform-demo`
  examples will play sound in the VM.

Other ways to get a real framebuffer, if QEMU is not an option: a spare Linux
machine, or a Hyper-V, VirtualBox or VMware VM. Step 2 of the port plan covers
the settings each one needs (a basic session rather than enhanced, and the right
virtual GPU).
