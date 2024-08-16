# Prerequisites

Before using KVM, you need to ensure that you have the necessary permissions to access `/dev/kvm`. This requires adding your user to the `kvm` group.

## Adding Yourself to the kvm Group

Follow these steps to add your user to the `kvm` group:

1. **Check if `/dev/kvm` is accessible**

   Run the following command to check if `/dev/kvm` exists:

   ```bash
   ls -l /dev/kvm
   ```
If it exists, you should see something like this:

```bash

crw-rw----+ 1 root kvm 10, 232 Aug 17 12:34 /dev/kvm
```
Notice that the group associated with /dev/kvm is kvm.

2. **Add your user to the kvm group:**

Use the following command to add your user to the kvm group:

```bash
sudo usermod -aG kvm $USER
```
Replace `$USER` with your username if you're not running the command as the intended user.

3. **Apply the changes**

After adding yourself to the kvm group, you need to log out and log back in for the changes to take effect.

Alternatively, you can apply the changes immediately using:

```bash
newgrp kvm
```

# Build

```bash
$ git clone https://github.com/x86driver/kvm
$ cd kvm
$ make
```

# Run
```bash
cd kvm
./kvm bzImage initramfs-busybox-x86.cpio.gz
```
