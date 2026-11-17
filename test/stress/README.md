# Window and graphics context rebuild stress test

This standalone test repeatedly creates, switches, closes, and recreates
`sf::RenderWindow`, `sf::Context`, and `sf::RenderTexture` instances. Every
iteration also exercises duplicate creation, destruction during a context
switch, and invalid window handles. A deterministic scene is rendered after
each iteration and compared byte-for-byte with a golden image.

## Build and run

```sh
cmake -S . -B build -DSFML_BUILD_STRESS_TESTS=ON -DSFML_BUILD_TEST_SUITE=OFF
cmake --build build --target sfml-window-context-rebuild-stress
./build/bin/sfml-window-context-rebuild-stress --iterations 200
```

Use `--fault-inject N` to perform a one-pixel negative control at zero-based
iteration `N`. The minimizer then prints the smallest operation sequence that
still reproduces the mismatch.

Useful options:

```text
--iterations N
--seed N
--fault-inject N
--no-minimize
--show-sfml-errors
--max-rss-growth-mb N
```

The process checks file descriptor stability, glibc heap growth, and RSS growth
after a 20-iteration warmup. For a strict leak verdict, run the same binary
under AddressSanitizer/LeakSanitizer.
