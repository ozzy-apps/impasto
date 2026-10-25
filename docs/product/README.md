# Screenshot provenance

Captured from a running Impasto 0.12.0 Linux Release build on 25 October 2026, using Qt 6.4.2 and the real OpenGL painting engine. No mock UI or generated screenshot images.

The sample landscapes are actual layered `.imp` documents: gradients, selected fills, and pencil strokes rendered by CanvasEngine. The four samples are saved in the output directory's Artworks folder.

Reproduce from the repository root, with a working desktop/OpenGL 4.3 session:

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 4
./build-release/impasto --product-shots /tmp/Impasto-0.12.0
```

`painting.png`, `brushes.png`, and `gallery.png` capture the real canvas framebuffer and visible application widgets. `about.png` is a grab of the actual About dialog opened through the gallery overflow action. The same run checks the displayed version, logo, stack, credit, support/repository links, Close, Escape, and artwork preservation. The command uses isolated settings and writes only its own sample documents.
