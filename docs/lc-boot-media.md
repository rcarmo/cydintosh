# Macintosh LC boot media workflow

This branch does not commit boot disk images. Use local-only `vendor/` storage.

## Expected local paths

| Asset | Local path | Git status |
|---|---|---|
| LC ROM | `vendor/mac-lc.rom` | ignored, user-supplied |
| LC boot disk | `vendor/lc-disk.img` | ignored, user-supplied |

`vendor/lc-disk.img` is the preferred placeholder for the first LC boot image.
Keep it read-only during early bring-up until the write path has been validated.

## Candidate boot media

Start with the simplest known-good LC-compatible media available locally:

1. 1.44MB System 6.0.8 HFS floppy image, if the ROM accepts floppy boot.
2. Small read-only HFS SCSI disk image for System 7 if floppy boot is not enough.

Do not enable guest writes until ROM boot, hardware stubs, and disk command paths
are stable. The S3/Mac Plus work showed that write-enabled Desktop updates can
cause guest-side stalls, so the LC path should begin read-only as well.

## Metadata inspection

Use:

```bash
make lc-disk-info
python3 tools/inspect_lc_disk.py vendor/lc-disk.img
```

The inspector prints only metadata:

- size and known floppy-size classification;
- SHA256/MD5 for local comparison;
- boot-block and HFS MDB signatures;
- a small amount of allocation metadata if present.

It does not list files or dump disk contents.

If the file is absent, `make lc-disk-info` reports the expected path and returns
success so CI/build checks are not blocked by missing copyrighted/user assets.

## Future firmware flow

The initial firmware probes only the `disk` partition availability/size and keeps
all LC disk behavior read-only. Actual LC disk I/O should be added later behind
explicit tracing:

- command name;
- sector/block number;
- byte count;
- read/write flag;
- return status;
- throttled trace-ring entry.

`src/machine_lc/lc_disk.c` now provides the trace/policy scaffold. It records
sample SWIM/SCSI-style command names, sector/block numbers, byte counts,
read/write flags, and status into logs plus the LC trace ring. Write commands are
reported as blocked while `LC_DISK_IMAGE_READ_ONLY=1`.

Write handling should remain disabled or panic-gated until the read-only boot path
has reached a stable ROM/System probe phase.
