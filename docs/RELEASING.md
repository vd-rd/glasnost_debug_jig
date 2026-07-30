# Cutting a release

Two binaries get attached to each tagged release: a merged flashable
firmware image, and a standalone `jigctl` executable. Neither is committed
to the repo — they're built fresh and attached to the GitHub Release only.

## 1. Bump the version

`GLASNOST_FW_VERSION` in `firmware/main/board_config.h`. Tag name is `v<that
version>` (e.g. `v0.1.0`).

## 2. Build the merged firmware binary

```sh
firmware/scripts/build.sh   # fresh podman build, don't release stale artifacts

podman run --rm \
  -v "$PWD/firmware:/project:Z" -w /project \
  docker.io/espressif/idf:release-v5.5 \
  idf.py merge-bin -o glasnost_jig-<version>-esp32s3-merged.bin
```

Gotcha: `idf.py merge-bin` runs `esptool.py` from inside `build/` already,
so `-o build/foo.bin` resolves to a nonexistent nested `build/build/foo.bin`
— pass a bare filename and pick it up from `firmware/build/` afterward.

`merge-bin` reads real per-binary flash offsets from `build/flasher_args.json`
— don't hand-copy offsets from memory/docs, they can change if the
partition table or bootloader config changes. Sanity-check the merged
file's size roughly matches (app flash offset + app binary size); it should
not be suspiciously small.

Flashing the result on real hardware: `esptool.py --chip esp32s3 write_flash
0x0 glasnost_jig-<version>-esp32s3-merged.bin`.

## 3. Build the standalone jigctl binary

```sh
python3 -m venv /tmp/jigctl-build-venv && . /tmp/jigctl-build-venv/bin/activate
pip install pyserial pyinstaller
pyinstaller --onefile --name jigctl host/jigctl.py \
  --distpath /tmp/jigctl-dist --workpath /tmp/jigctl-work --specpath /tmp/jigctl-spec
```

Verify it actually runs standalone (`--help`, plus a subcommand's `--help`)
before shipping it — this is a Linux x86_64-only build tied to the glibc of
whatever machine built it; cut platform-specific builds separately if this
ever needs to run elsewhere.

## 4. Tag and push

```sh
git tag -a v<version> -m "v<version>"
git push origin v<version>
```

## 5. Publish the GitHub Release

Needs either the `gh` CLI authenticated, or a token with `repo` scope for
the API — neither was available in the sandbox this was first built in, so
step 5 had to be hand-finished by a human once. If `gh` is available:

```sh
gh release create v<version> \
  glasnost_jig-<version>-esp32s3-merged.bin \
  jigctl-<version>-linux-x86_64 \
  --title v<version> --notes "..."
```

Otherwise: push the tag (step 4), then use the GitHub web UI — "Draft a new
release" from the pushed tag, attach both binaries by hand.
