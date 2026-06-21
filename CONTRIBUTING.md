# Contributing

Thanks for wanting to contribute to this learning-focused container engine project. The repo is intentionally low-level and interacts with Linux kernel features. Please read the platform guidance below before running code or submitting patches.

## Platform & safety

- This project targets Linux and requires kernel features (namespaces, cgroups v2, OverlayFS, iptables). Use a disposable Linux VM (Ubuntu 22.04 or similar) or a dedicated Linux machine for development and testing.
- Many operations require `sudo`. Only run these commands in a safe environment where you can modify networking and firewall settings.

## Quick build (example)

From the `docker-implementation/` folder there is an engine implementation. A common quick build step is:

```bash
cd docker-implementation
gcc engine.c -o engine
```

Adjust filenames according to the stage you're working on (some folders use `main.c`, `stageX.c`, or `engine.c`).

## Quick smoke test

Run simple commands that don't modify host state first:

```bash
# Compile (from the right folder)
gcc engine.c -o engine

# Run a non-destructive help or init (some commands will download a small rootfs)
sudo ./engine --help
sudo ./engine init
```

Avoid running network- or firewall-modifying commands on production hosts. Prefer an isolated VM.

## Tests and validation

- There are no automated unit tests included by default. If you add tests, please include them in a new `tests/` folder and provide a short script to run them.
- When modifying C code, run a compile and a brief smoke run in a disposable VM to ensure no kernel state or host firewall settings are left altered.

## Code style

- Keep changes small and focused. Prefer clear comments where you use raw `system()` calls or write to `/proc` or `/sys` paths.
- Document any new required host capabilities in this `CONTRIBUTING.md`.

## Submitting changes

1. Fork the repository and create a feature branch.
2. Make small, well-documented commits with clear messages.
3. Open a pull request describing the change, how to reproduce, and any manual testing performed.

If your change requires platform setup (for example: enabling cgroup v2, installing `iproute2`), include the exact commands you used in the PR description.

## Helpful notes for maintainers

- If you add scripts that change host network state, ensure they clean up on both success and failure (remove iptables rules, delete veth interfaces, unmount temporary mounts).
- Prefer using `2>/dev/null || true` idiom on `ip link add` or `ip link del` operations when re-running scripts so they don't fail if an artifact already exists.

Thanks — contributions that improve documentation, add safe helper scripts, or add test harnesses are especially welcome.
