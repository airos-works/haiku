# ROCK 5 ITX fork instructions

This is jmgasper's experimental, explicitly AI-assisted Haiku fork. The owner
has authorized AI-assisted development here. Upstream Haiku's policy does not
accept AI-generated or AI-modified contributions; do not submit this fork's
work upstream or represent it as upstream-approved.

- Keep all local project files, downloads, temporary files, caches, builds,
  credentials and evidence under the mounted `/mnt/HaikuWork` filesystem.
  Do not change HOME or CODEX_HOME. Use `tools/rock5-itx/env.sh`.
- `master` is the integration branch and holds all of the fork's work; it is
  checked out in `/mnt/HaikuWork/src/haiku`. Device work goes on `rpi4`,
  a board integration branch such as `cubie-a7s`, or a short topic branch
  started from `master` (the `airos-release` branch was retired on
  2026-11-05: every air/OS image, x86_64, arm64, rpi4 and cubie, is built by
  CI from `master`); X399
  workstation work goes straight to topic branches (the `x399-workstation`
  branch was retired on 2026-10-19 and `/mnt/HaikuWork/x399/haiku` is a
  detached checkout of `master` kept for its `docs/x399-workstation` tools).
  Upstream Haiku is the `upstream` remote; merge it into `master`
  deliberately, not as a side effect. Pin source/toolchain revisions in build
  manifests.
- **Land fixes on `master` as soon as they are ready; don't wait to be asked.**
  The owner does not always remember to say "merge it", and finished work left
  on a branch or in unpushed local commits ships to nobody. When a fix or
  feature is done and verified (it builds for the affected arches; tested on
  hardware or QEMU where that applies), merge it into `master`
  (`git merge --no-ff <branch>`, or fast-forward/rebase a small one), build-check
  the merge, push, and delete the topic branch. Board integration branches such
  as `cubie-a7s` take the work first, then go on to `master` the same way.
  For third-party forks the integration branch is their `airos` branch; push it
  and bump the pin that consumes it. Long-lived branches still merge into
  `master` at least every day or two.
- Keep a change off `master` only when the owner says to, or when it is
  unverified, experimental or known to regress something. Then say so plainly
  in the final report: the branch name, what is on it and what it still needs.
- At the start of a session, check for stranded work in the checkout you use
  (`git status`, `git log origin/master..HEAD`, `git worktree list`) and report
  any. At the end, report where each fix landed: the pushed `master` commit,
  or the branch it is on and why.
- Read `docs/rock5-itx/README.md`, `STATUS.md` and `ROADMAP.md` in that directory
  before hardware work. Update status with actual evidence, not assumptions.
- The dedicated ROCK and NanoKVM may be deployed to, restarted and configured
  as authorized by the owner. Use the lab lock for target mutations.
- Preserve a working recovery route. Establish serial capture and a verified
  restore procedure before replacing boot firmware. Raw register/clock work
  needs the exact board revision and matching hardware documentation.
- Do not commit credentials, local configuration, images, logs or captures.
- Run the local build and QEMU smoke test before a hardware iteration.
  A USB attachment or QEMU boot does not prove native ROCK hardware support.
- Keep phase acceptance tests specific to the device and record the Linux
  reference configuration used for comparison. Missing fixtures stay untested.
