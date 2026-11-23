# Read-only file-write plans

`gui-core-cli` can generate a file-write plan instead of patching and repacking
an `Update.img`. This is intended for firmware composers that already validate,
extract and write their own images. The existing image and GUI pipelines remain
available and use the same patch logic.

## CLI

Supply a directory containing the original files at their device-relative paths:

```text
input/
  usr/Evil/Evil
  usr/Evil/Scripts/evil
```

Then run:

```sh
gui-core-cli input plan --file-plan --model mx5 --instances 2
# Use --instances 4 to include the Anxiety OD V2 hijack.
```

No FIT image, ext4 filesystem, temporary rootfs, compression or image repacking
is used in this mode. Input files are not modified. The output directory must
not exist, and its parent must exist.

The output contains numbered binary payloads and a `plan.json` completion marker:

```json
{
  "version": 1,
  "files": [
    {"path": "/usr/Evil/Evil", "file": "0.bin", "mode": 33261}
  ]
}
```

This example shows one entry; the MX5 plan contains four: Evil, its launcher,
libnam_hook.so and libnam_preload.so. `mode` is a full POSIX regular-file mode
(`33261` is `0100755`). `file` is relative to the plan directory. Consumers can
turn this into Bridge's `{path: (bytes, mode)}` representation and apply it with
their own filesystem writer.

Validate the input firmware **before** supplying files: file planning has no
bundle metadata or whole-image hash to check. The patcher's existing ELF and
launcher precondition checks still apply. Consumers must validate the plan's
schema, allowed target paths, modes and payloads, then verify writes and
filesystem integrity themselves. Generated payloads include proprietary firmware
content: keep them local and never commit or redistribute them.

The page-turner mod is currently unsupported in file-plan mode and is rejected
before reading input. Pedalboard additive NAM/Reverb resources are supported,
using the same options as the image pipeline.

## C API

`nam_patch_plan()` in `core/patch_pipeline.h` takes a `NamReadFileFn` callback,
an explicit model and `NamPatchOptions`. The callback supplies malloc-owned file
buffers; the planner frees them. It returns a `NamFilePlan` of absolute target
paths, owned bytes and full modes. Release it with `nam_file_plan_free()`.

The planner does not write files, including when `NAM_DUMP_EVIL` is set. On
failure the output plan is empty. The caller owns source validation and applying
the plan; no build tools, Nix or subprocesses are required at runtime.

## Offline tests

After building the `gui-core-cli` target:

```sh
python3 tests/test_file_plan.py build/gui-core-cli
```

These tests construct a synthetic ELF and launcher, exercise two/four-instance
plans and rejection paths, and check that inputs are unchanged. They do not
contain or download vendor firmware and never access hardware.
