# macOS smoke test (Apple silicon)

Small programs that check, without the game, what the port needs from a Mac: the guest image at
0x800000000, the guest range above Rosetta's reservation, shared memory and 4 KiB views, TLS through
a pthread key at `gs:[8*key]`, fault-based write tracking, runtime code patching, and that x86-64
instructions cpuid hides (AVX, F16C, BMI1, LZCNT, MOVBE) execute under Rosetta 2. Takes a few
minutes. The results are in [docs/sacredfire/19-macos-apple-silicon.md](../../docs/sacredfire/19-macos-apple-silicon.md).

Needs a Mac with Apple silicon, macOS 15 or newer, the Xcode command line tools
(`xcode-select --install`) and Rosetta 2 (`softwareupdate --install-rosetta`).

```
cd tools/macos-smoke
bash run.sh
```

It writes `report.txt` next to itself. It only uses memory it creates, reads or changes no files
of yours and does not go online. With Homebrew's `vulkan-tools` and `molten-vk` installed it also
adds the Vulkan device's report.
