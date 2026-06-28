# MediaPipe Segmentation Runtime

Place runtime binaries and model files in this directory before configuring, pass
`-DMEDIAPIPE_SEGMENTATION_RUNTIME_DIR=<path>` to CMake, or build the bundled
DirectML runtime by passing `-DONNXRUNTIME_ROOT=<path-to-Microsoft.ML.OnnxRuntime.DirectML>`.

At runtime the OBS filter looks for these default files in the plugin data directory:

- `obs-mediapipe-runtime.dll`
- `selfie_segmenter.onnx`

The runtime DLL must export the C ABI declared in `mediapipe-runtime.h`. Dependent
DLLs, such as `onnxruntime.dll` and `DirectML.dll`, can sit next to it in the
same copied plugin data directory.
