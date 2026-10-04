# partman-tui

A small professional-style ncurses storage administration application for Linux.
<img width="1851" height="1080" alt="Screenshot From 2026-10-04 19-49-30" src="https://github.com/user-attachments/assets/9354ec3c-71f2-40ff-b7ba-15cbcbd63745" />

## Safety model

- Starts by obtaining root access through `sudo`.
- Scans disks, partitions, LVM and encrypted mappings with `lsblk`.
- Supports mount and unmount operations for detected filesystems.
- Creates mount directories only under an absolute path selected by the administrator.
- Backs up `/etc/fstab`, locks it, appends UUID-based entries, flushes the file, and reloads systemd.
- Opens `cfdisk` for partition editing only after the exact confirmation phrase `ERASE-RISK`.
- Never automatically formats a partition. Formatting is intentionally excluded because selecting the wrong device causes irreversible loss.
- Logs actions and return codes to `/var/log/partman-tui.log`.

## Supported filesystems

Detection is not hard-coded to ext4 or FAT. It displays any filesystem reported by `lsblk`, including NTFS, exFAT, XFS, Btrfs, F2FS, swap, LUKS mappings, and others supported by the installed kernel/userspace drivers.

## Dependencies

```bash
sudo apt update
sudo apt install build-essential libncurses-dev util-linux
```

Optional filesystem helpers may be required, for example `ntfs-3g`, `exfatprogs`, `xfsprogs`, or `btrfs-progs`.

## Build and install

```bash
make
make check
sudo make install
sudo partman-tui
```

For a local test:

```bash
make
./partman-tui
```

The application automatically re-executes itself through `sudo`.

## Keys

- Up/Down: select device
- R: rescan
- M: mount
- U: unmount
- S: save a persistent UUID-based entry to `/etc/fstab`
- P: launch `cfdisk` for the selected whole disk
- D: show diagnostics
- Q: quit

## Return-code conventions

- `0`: clean exit
- `1`: allocation/general startup failure
- `77`: root privilege acquisition failed
- Child utility return codes are shown in the status line and written to the log.

## Recovery

Every fstab update creates `/etc/fstab.partman.TIMESTAMP.bak`. Restore one with:

```bash
sudo cp -a /etc/fstab.partman.TIMESTAMP.bak /etc/fstab
sudo systemctl daemon-reload
sudo mount -a
```

Before repartitioning, back up all important data. Test the program first inside a virtual machine or with a disposable USB drive.
