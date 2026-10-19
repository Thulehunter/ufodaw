# Contributing to UFODAW

Thanks for wanting to help. UFODAW is a small project, so every bug report, idea and pull request matters.

## Ways to help

- **Report a bug** with the [bug form](../../issues/new/choose). Say what you did, what happened and what you expected, and attach a project file that shows the problem if you can.
- **Request a feature** or vote on existing requests in the [issues](../../issues?q=label%3Aenhancement).
- **Ask questions and share your music** in [Discussions](../../discussions) or on [Discord](https://discord.gg/BhvKsN6knU).
- **Improve the docs.** The website lives in `docs/` (plain HTML and CSS) and the README describes every feature.
- **Fix something.** Issues labelled `good first issue` and `help wanted` are good places to start.

## Building

UFODAW is C++17 with CMake and JUCE 8. Use Release, since Debug is too slow for audio.

```
cmake -S . -B build
cmake --build build --config Release --target UFODAW
```

See *Building from source* in the [README](README.md) for ASIO and packaging.

## Before you open a pull request

1. Keep the change focused: one fix or feature per pull request.
2. Match the style of the surrounding code.
3. Run the tests (see *Tests* in the README). The engine must give identical results on one core and on several, and old project files must keep opening.
4. Add a bullet to the README for a new feature, and mention it in the pull request.
5. Open the pull request against the `dev` branch.

## License

By contributing you agree that your work is released under the project's [AGPLv3 license](LICENSE).
