# Project notes

`julianvidalm/wire-os-victor` is the robot firmware (`/anki`) of the v3ctor project. The robot is the body (motion, sensors, microphone and camera capture); a PC running [wire-pod](https://github.com/julianvidalm/wire-pod) is the cortex (wake word, speech recognition, Claude-powered reasoning).

## Origin and license

The code was imported from [os-vector/wire-os-victor](https://github.com/os-vector/wire-os-victor) at commit `5c8ffbfa`, which continues Anki's Vector ("victor") source. Upstream history is not carried over; authorship of the imported code belongs to its original authors as recorded upstream. The project is licensed under GPL-3.0 (see `LICENSE`); keep that file and every copyright notice in the source files.

## Independence

- `origin` (`git@github.com:julianvidalm/wire-os-victor.git`) is the only remote. Do not add the original repository as a remote, merge from it, or open pull requests against it.
- Pull requests come only from changes to this code base.
- If an upstream fix is ever worth having (for example a security issue), review it and port it by hand in a normal pull request that credits the source.

## Changes from the imported code

- **MicTap**: vic-anim streams the processed microphone (16 kHz mono), the speaker reference (32 kHz mono) and speaking/moving flags over TCP port 8899. It is off by default; enable it with the console var `MicTap_Enable` (`curl "http://<robot>:8889/consolevarset?key=MicTap_Enable&value=true"`) or the persistent flag file `micTapEnabled`, and optionally restrict clients with `micTapAllowedPeer`. Code: `animProcess/src/cozmoAnim/micData/micTap.{h,cpp}`.
- **No Alexa**: the Alexa client, the AVS device SDK, the Alexa wake word, its CLAD messages and enum values, and its resources are removed. Kept on purpose: the audio CLAD (Wwise IDs; the mute sounds live in `Victor_Alexa.bnk`), `BUTTON_WAKEWORD_ALEXA` and settings field 10 (persisted in jdocs), the public gateway Alexa RPCs (stubs), and the engine `ALEXA_ACOUSTIC_TEST` mode. Retired CLAD tags stay reserved as `// Available` comments.

## Building

`build/build-v.sh` builds all of `/anki` (vicos Release) inside the `vic-standalone-builder-8` Docker image; the output is `_build/vicos/Release`. The toolchain and build tools live in `anki-deps/` (mounted as `~/.anki` in the container).

`docker run -it` needs a terminal. From a non-interactive shell, run it under a pseudo-terminal:

```sh
script -qec "./build/build-v.sh" build.log </dev/null
```

## Deploying to a robot

The robot must be unlocked and running WireOS. Deploy only the files whose sources changed (standalone and image builds always differ by build id) with `tools/vector/deploy-anki.sh` from the v3ctor repository:

```sh
tools/vector/deploy-anki.sh deploy bin/vic-anim lib/libcozmo_engine.so --remove <path under /anki>...
tools/vector/deploy-anki.sh rollback <same arguments>
```

Each replaced or removed file is backed up once in `/data/anki-backup`. A CLAD enum whose values are numbered by position (for example `BackpackAnimationTrigger`) must ship in the same deploy for every binary that uses it.

## Workflow

- `main` is protected: changes land through a pull request, history stays linear (squash merge only), conversations must be resolved, and force pushes and deletion are blocked.
- Branch names: `feat/...`, `fix/...`, `refactor/...`, `chore/...`. Commit messages follow Conventional Commits.
