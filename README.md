# Kiki — an early experiment in AI-assisted emulation

I started Kiki to explore how far I could get building a pinball emulator with
AI. The difficult part was deciding when its conclusions were believable and
when to push back. I did substantial manual reverse engineering, tested my own
theories, and checked proposed explanations against disassembly and runtime
behavior. The journal records the experiments and the assumptions that changed.

I was the sole human developer of the original Kiki code in this early snapshot,
with Claude, Codex, and Binary Ninja assisting with implementation and analysis.
The software and content it builds on have their own authors and terms.

This is a historical source and journal archive through May 12, 2026. It covers
an AArch64 shim running under QEMU user-mode emulation, device and switch models,
graphics marshalling, host media decoding, and synthetic tests. It is intended
for reading, rather than as a supported application to install.

Kiki has continued well beyond this version. If you're interested in working
on the project, [get in touch with me](https://christiancoleman.info/) about the
current work.

## Explore the engineering

| Area | Entry point |
| --- | --- |
| Runtime and host layout | [src/README.md](src/README.md) |
| Shim architecture | [src/SHIM.md](src/SHIM.md) |
| Device model | [src/nodebus/](src/nodebus/) |
| Graphics and media | [src/display/](src/display/) |
| Machine profile | [src/games/](src/games/) |
| Synthetic tests | [src/tests/README.md](src/tests/README.md) |
| Engineering journal | [docs/JOURNAL.md](docs/JOURNAL.md) |

## Reading the history

All 169 selected original commits are retained, beginning April 4, 2026; the
emulator source first appears on April 11. Their original author and committer
dates and parent relationships are preserved, including empty commits.
Some material is omitted from historical revisions and some commit messages
are edited, so commit hashes differ. The documentation and license update
following that history carries its actual date.

The journal includes hypotheses, failed experiments, and later corrections.
Historical successful runs describe the implementation at the time; they do
not establish that this reduced snapshot reproduces those runs or the current
Kiki implementation. Build and launch scripts retain their historical
Linux/WSL assumptions and require an external runtime environment.

## License

[LICENSE](LICENSE) provides MIT terms for my original retained work, including
the retained historical revisions. [LICENSES.md](LICENSES.md) describes the
scope and external dependencies. Game binaries, firmware, artwork, music,
manuals, and table assets are not included or licensed by this repository.

This independent project is not affiliated with or endorsed by Stern Pinball,
Nintendo, The Pokemon Company, or other game rights holders.
