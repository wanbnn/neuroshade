# NeuroShade — Product Specification

> **Status:** Draft for implementation  
> **Method:** Spec-Driven Development  
> **Target:** Production-capable v1.0  
> **Primary platform:** Linux x86_64 + AMD Radeon + Vulkan + HIP/ROCm  
> **Primary gaming path:** Native Vulkan and Windows games through Proton/DXVK/VKD3D-Proton  
> **Document role:** This file is the source of truth for scope, architecture, interfaces, acceptance criteria, milestones, and Definition of Done.

---

## 0. Executive summary

NeuroShade is an AMD-only graphics post-processing and neural-rendering runtime for games.

It is conceptually similar to ReShade in user experience, but its execution model is based on:

- Vulkan interception through a Vulkan Layer;
- a semantic FrameGraph;
- GPU-resident resources;
- standard SPIR-V shader plugins;
- HIP/ROCm compute plugins;
- temporal neural models;
- persistent temporal history;
- depth/motion/resource discovery;
- per-game profiles;
- an in-game overlay and external launcher;
- profiling and automatic fallback.

The project MUST finish with a usable product, not only a proof of concept.

The first production release MUST run a game, intercept Vulkan presentation, expose the frame to a configurable pipeline, execute at least one shader plugin and one neural temporal plugin, return the processed frame to Vulkan, expose performance metrics, persist a game profile, and fail safely when an unsupported feature is encountered.

The project MUST NOT depend on CPU readback in its preferred rendering path. A slower host-staging fallback MAY exist for compatibility.

---

# 1. Product goal

Build a generic AMD Radeon neural-rendering runtime capable of improving games without requiring access to their source code.

The runtime must enable:

1. post-processing;
2. visual restoration;
3. neural image enhancement;
4. temporal reconstruction;
5. temporal super-resolution when the required source resources are available;
6. shader plugins;
7. HIP compute plugins;
8. neural-model plugins;
9. inspection and manual binding of game render resources;
10. persistent per-game profiles.

The end-user experience should eventually resemble:

```text
Game
  ↓
Vulkan / Proton
  ↓
NeuroShade Vulkan Layer
  ↓
Semantic FrameGraph
  ├─ Shader plugin
  ├─ HIP plugin
  ├─ Neural plugin
  └─ Temporal history
  ↓
AMD Radeon
  ↓
Present
```

---

# 2. Hard product decisions

These decisions are binding for v1 unless this SPEC is explicitly amended.

## DEC-001 — Linux + Vulkan first

v1 targets:

- Linux x86_64;
- Vulkan;
- AMD Radeon;
- HIP/ROCm;
- native Vulkan applications;
- Proton/DXVK/VKD3D-Proton applications.

Native Direct3D interception on Windows is NOT a v1 requirement.

Reason: one Vulkan backend can cover both native Vulkan games and a large portion of Windows games running through Proton.

---

## DEC-002 — Explicit opt-in layer

NeuroShade MUST NOT silently inject itself system-wide.

The production launcher MUST enable the Vulkan Layer only for the selected process.

Preferred launch flow:

```text
neuroshade-run %command%
```

The wrapper sets the necessary Vulkan layer environment and starts the game.

This reduces compatibility problems and anti-cheat risk.

---

## DEC-003 — No direct assumption that VkImage is linear HIP memory

A Vulkan image MUST NOT be treated as a raw contiguous HIP pointer.

The stable interop boundary is:

```text
VkImage
  ↓ Vulkan copy/compute conversion
exportable VkBuffer
  ↓ external-memory import
HIP pointer
  ↓ HIP / neural processing
exportable VkBuffer
  ↓ Vulkan copy/compute conversion
VkImage
```

The runtime MAY add a direct-image path in the future only for image formats/layouts proven safe by a dedicated backend.

---

## DEC-004 — GPU memory zero-copy is preferred; synchronization may initially be host-coordinated

The preferred data path MUST avoid GPU→CPU→GPU frame copies.

However, v1 MUST NOT depend on Linux HIP external semaphore support being available on every supported installation.

The baseline synchronization path is allowed to be:

```text
Vulkan submit
  ↓
Vulkan fence wait on host
  ↓
HIP stream executes
  ↓
HIP event/stream completion
  ↓
Vulkan consumes HIP output
```

This is a CPU synchronization point, but frame data remains in GPU-visible shared memory.

An asynchronous external-semaphore path MAY be enabled only after runtime capability testing proves it reliable.

---

## DEC-005 — `.pth` is an import format, not the in-game runtime format

Arbitrary `.pth` files MUST NOT be loaded directly inside the game process.

`.pth` may contain only weights, a serialized Python object, or architecture-dependent state.

Production flow:

```text
.pth
 ↓
isolated model importer
 ↓
architecture adapter + validation
 ↓
ONNX/intermediate graph
 ↓
MIGraphX compile/validation
 ↓
.nsmodel package
 ↓
NeuroShade runtime
```

The user may select `.pth` in the UI, but NeuroShade converts it before the model becomes available to a game profile.

---

## DEC-006 — MIGraphX is the preferred v1 neural runtime

The production in-game neural path SHOULD use MIGraphX when the imported model is supported.

PyTorch ROCm MAY be retained as:

- importer dependency;
- development backend;
- compatibility backend.

Python MUST NOT be required inside the game process.

---

## DEC-007 — Shader plugins are the safest general plugin class

Production plugin priority:

1. SPIR-V shader plugins — fully supported;
2. `.nsmodel` neural plugins — fully supported;
3. HIP source/native plugins — supported only as trusted/advanced plugins.

HIP/native plugin code can crash the process or GPU and cannot be considered sandboxed.

---

## DEC-008 — Manual resource binding is mandatory

Automatic detection of depth, motion vectors, normals, and low-resolution scene color is useful but cannot be universally reliable.

The product MUST provide:

- automatic heuristics;
- debug previews;
- candidate list;
- manual resource selection;
- persistent per-game binding.

A game is considered supported even when resource selection requires a one-time manual profile.

---

## DEC-009 — Neural upscaling must not be falsely advertised

There are two different modes:

### Native-resolution neural enhancement

Input and output have the same presentation resolution.

Examples:

- detail restoration;
- denoise;
- deblur;
- temporal cleanup;
- anti-aliasing;
- local detail reconstruction.

This MUST work in v1 when only final color is available.

### True temporal super-resolution

A lower-resolution scene source is reconstructed into a higher-resolution output.

This REQUIRES a valid low-resolution color source and preferably:

- depth;
- motion vectors;
- jitter/exposure metadata.

v1 MUST support this when those resources can be bound.

NeuroShade MUST NOT claim FPS-saving temporal upscaling for games where it only sees the final full-resolution frame.

---

# 3. Non-goals for v1

The following are explicitly out of scope for the v1 Definition of Done:

- bypassing anti-cheat;
- stealth injection;
- kernel drivers;
- universal Windows DirectX hooking;
- replacing FSR/DLSS integration at the engine API level;
- frame generation;
- guaranteed automatic motion-vector detection in every game;
- generic 4× texture replacement for every engine;
- arbitrary Python plugin execution in the game;
- arbitrary `.pth` execution in the game;
- NVIDIA support;
- Intel support;
- macOS;
- mobile platforms.

These may become future work, but MUST NOT block v1.

---

# 4. Supported environment

## 4.1 Required

Production-supported system requirements:

- Linux x86_64;
- AMD GPU supported by the installed HIP/ROCm stack;
- working Vulkan driver;
- Vulkan 1.2 or newer preferred;
- ROCm/HIP runtime installed;
- `VK_KHR_external_memory_fd` or compatible memory-sharing path for preferred zero-copy mode;
- enough VRAM for the selected model and temporal history.

Runtime startup MUST perform capability detection.

---

## 4.2 Compatibility classes

Each launch receives one class:

```text
A — Full
B — Reduced
C — Shader-only
D — Unsupported
```

### Class A

- Vulkan capture;
- external-memory buffer interop;
- HIP;
- neural runtime;
- temporal resources available.

### Class B

- Vulkan capture;
- HIP;
- neural runtime;
- host-staging fallback or reduced resource set.

### Class C

- Vulkan shaders available;
- HIP/neural unavailable.

### Class D

- required Vulkan interception or presentation path unsupported.

The launcher and overlay MUST show this class.

---

# 5. Repository structure

```text
neuroshade/
├── SPEC.md
├── README.md
├── LICENSE
├── CMakeLists.txt
├── cmake/
│
├── src/
│   ├── layer/
│   │   ├── entrypoints/
│   │   ├── dispatch/
│   │   ├── instance_state/
│   │   ├── device_state/
│   │   ├── swapchain/
│   │   └── resource_tracker/
│   │
│   ├── framegraph/
│   │   ├── graph/
│   │   ├── pass/
│   │   ├── compiler/
│   │   ├── scheduler/
│   │   └── resources/
│   │
│   ├── vulkan/
│   │   ├── capture/
│   │   ├── conversions/
│   │   ├── interop_buffers/
│   │   ├── synchronization/
│   │   └── presentation/
│   │
│   ├── hip/
│   │   ├── runtime/
│   │   ├── interop/
│   │   ├── kernels/
│   │   ├── rtc/
│   │   └── profiler/
│   │
│   ├── neural/
│   │   ├── runtime/
│   │   ├── migraphx/
│   │   ├── model/
│   │   └── temporal/
│   │
│   ├── plugins/
│   │   ├── host/
│   │   ├── shader/
│   │   ├── hip/
│   │   └── neural/
│   │
│   ├── overlay/
│   ├── profile/
│   ├── telemetry/
│   ├── logging/
│   └── common/
│
├── tools/
│   ├── neuroshade-cli/
│   ├── neuroshade-run/
│   ├── nsmodel/
│   ├── ns-plugin-pack/
│   └── ns-testbed/
│
├── frontend/
│   └── desktop/
│
├── importer/
│   ├── python/
│   ├── adapters/
│   └── schemas/
│
├── sdk/
│   ├── shader/
│   ├── hip/
│   └── neural/
│
├── plugins/
│   ├── examples/
│   └── bundled/
│
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── vulkan/
│   ├── hip/
│   ├── neural/
│   └── proton/
│
└── packaging/
    ├── linux/
    ├── vulkan-layer/
    └── appimage/
```

---

# 6. Runtime architecture

```text
┌─────────────────────────────────────────────────────────┐
│                         GAME                            │
│            Native Vulkan / Proton Vulkan               │
└────────────────────────┬────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────┐
│             VK_LAYER_NEUROSHADE                         │
│                                                         │
│  Dispatch tables                                        │
│  Swapchain tracker                                      │
│  VkImage/VkBuffer tracker                               │
│  Descriptor/render-pass observations                    │
│  Resource candidates                                    │
└────────────────────────┬────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────┐
│                   RESOURCE RESOLVER                     │
│                                                         │
│ Color | Depth | Motion | Normals | Scene Color | etc.   │
└────────────────────────┬────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────┐
│                     FRAMEGRAPH                          │
│                                                         │
│ Capture → Convert → Shader → HIP/NN → Composite         │
└───────────────┬───────────────────────────┬─────────────┘
                │                           │
                ▼                           ▼
┌──────────────────────────┐   ┌──────────────────────────┐
│      VULKAN BACKEND      │   │       HIP BACKEND        │
│                          │   │                          │
│ SPIR-V                    │   │ Kernels                  │
│ image/buffer conversion   │   │ Temporal history         │
│ output composition        │   │ MIGraphX                 │
└───────────────┬──────────┘   └──────────────┬───────────┘
                │                             │
                └──────────────┬──────────────┘
                               ▼
                       AMD Radeon GPU
```

---

# 7. Vulkan Layer requirements

## VK-001 — Layer identity

The layer name MUST be:

```text
VK_LAYER_NEUROSHADE
```

The project MUST ship a valid Vulkan layer manifest.

---

## VK-002 — Loader negotiation

The layer MUST correctly implement the current Vulkan loader/layer negotiation mechanism required by the target loader.

No global function may recursively call itself.

Dispatch tables MUST be maintained per instance/device.

---

## VK-003 — Minimum intercepted API

The layer MUST observe or wrap at least:

```text
vkCreateInstance
vkDestroyInstance
vkCreateDevice
vkDestroyDevice
vkGetInstanceProcAddr
vkGetDeviceProcAddr

vkCreateSwapchainKHR
vkDestroySwapchainKHR
vkGetSwapchainImagesKHR
vkAcquireNextImageKHR
vkAcquireNextImage2KHR
vkQueuePresentKHR

vkCreateImage
vkDestroyImage
vkBindImageMemory
vkBindImageMemory2

vkCreateImageView
vkDestroyImageView

vkCreateBuffer
vkDestroyBuffer
vkBindBufferMemory

vkAllocateMemory
vkFreeMemory
vkMapMemory
vkUnmapMemory
vkFlushMappedMemoryRanges

vkCreateFramebuffer
vkDestroyFramebuffer

vkCreateRenderPass
vkCreateRenderPass2
vkCmdBeginRenderPass
vkCmdBeginRenderPass2
vkCmdEndRenderPass
vkCmdBeginRendering
vkCmdEndRendering

vkCmdPipelineBarrier
vkCmdPipelineBarrier2

vkCmdCopyImage
vkCmdCopyImage2
vkCmdBlitImage
vkCmdBlitImage2
vkCmdCopyBufferToImage
vkCmdCopyImageToBuffer

vkQueueSubmit
vkQueueSubmit2
```

Additional interception MAY be added as the resource analyzer matures.

---

## VK-004 — Pass-through safety

When NeuroShade is disabled or initialization fails, Vulkan behavior MUST remain as close to native pass-through as possible.

A plugin failure MUST NOT intentionally corrupt game resources.

---

## VK-005 — Present insertion

The final FrameGraph MUST execute before the image used for presentation is consumed by `vkQueuePresentKHR`.

The runtime MUST preserve valid synchronization and image layouts.

---

# 8. Resource tracker

Every tracked GPU resource MUST receive a stable runtime ID.

Example metadata:

```cpp
struct NSImageInfo {
    uint64_t id;

    VkImage image;
    VkFormat format;
    VkExtent3D extent;
    VkImageUsageFlags usage;
    VkSampleCountFlagBits samples;

    uint64_t memory_id;
    VkDeviceSize memory_offset;

    uint64_t create_frame;
    uint64_t last_read_frame;
    uint64_t last_write_frame;

    uint32_t write_count;
    uint32_t read_count;

    bool swapchain_image;
    bool depth_candidate;
    bool motion_candidate;
    bool scene_color_candidate;
};
```

The resource tracker MUST NOT retain destroyed Vulkan handles.

---

# 9. Semantic resources

The FrameGraph does not expose resources to plugins by random handle.

It exposes semantic roles.

v1 semantic namespace:

```text
Color.Final
Color.Scene
Color.LowRes

Depth.Device
Depth.Linear

Motion.Screen

Normal.World
Normal.View

History.Color.1
History.Color.2
History.Color.3

History.Depth.1
History.Motion.1

Output.Color

Frame.Index
Frame.Width
Frame.Height
Frame.DeltaTime
```

Not every semantic is guaranteed to exist.

---

# 10. Resource discovery

## RES-001 — Depth detection

Automatic depth scoring SHOULD use:

- depth-compatible format;
- image usage flags;
- render-pass/depth attachment use;
- frame dimensions;
- number of writes;
- proximity to final scene pass.

The UI MUST allow manual override.

---

## RES-002 — Motion detection

Motion candidate scoring MAY use:

- two-component floating-point formats;
- screen-sized dimensions;
- bounded positive/negative values;
- use before temporal resolve;
- recurrent per-frame writes.

Automatic identification is never considered authoritative.

The UI MUST allow preview and manual selection.

---

## RES-003 — Scene color / low-resolution color

The analyzer SHOULD identify:

- render targets smaller than swapchain extent;
- color attachments later sampled/blitted into full-resolution output;
- recurrent pre-present render targets.

This is required for true super-resolution mode.

---

## RES-004 — Resource preview

The overlay MUST provide debug visualization modes:

```text
RGBA
RGB
R
G
B
Depth normalized
Depth linearized
Motion XY visualization
Normal visualization
```

A user must be able to click:

```text
Bind as Depth
Bind as Motion
Bind as Scene Color
Bind as Low-Res Color
```

Bindings persist in the game profile.

---

# 11. Frame representation

```cpp
struct NSFrame {
    uint64_t frame_index;

    uint32_t output_width;
    uint32_t output_height;

    NSResourceHandle final_color;
    NSResourceHandle scene_color;
    NSResourceHandle low_res_color;

    NSResourceHandle depth_device;
    NSResourceHandle depth_linear;

    NSResourceHandle motion;
    NSResourceHandle normals;

    float delta_time;

    bool has_scene_color;
    bool has_low_res_color;
    bool has_depth;
    bool has_motion;
    bool has_normals;

    bool history_valid;
};
```

Plugins MUST consume `NSFrame` semantics rather than Vulkan internals whenever possible.

---

# 12. Vulkan ↔ HIP interop

## INT-001 — Interop buffer pool

The runtime MUST preallocate reusable exportable Vulkan buffers.

No per-frame allocation is permitted in steady state.

Minimum pool:

```text
InputColor[3]
Depth[3]
Motion[3]
OutputColor[3]
Scratch[N]
```

The pool SHOULD use triple buffering.

---

## INT-002 — Vulkan conversion stage

Before HIP access, Vulkan MUST convert/copy source resources into a documented linear buffer layout.

Canonical color layout for v1:

```text
FP16 RGBA planar or packed
```

Recommended canonical packed representation:

```cpp
struct alignas(8) NSHalf4 {
    uint16_t r;
    uint16_t g;
    uint16_t b;
    uint16_t a;
};
```

Canonical depth:

```text
FP32 linear depth
```

Canonical motion:

```text
FP16/FP32 screen-space XY
```

The exact ABI MUST be versioned.

---

## INT-003 — External memory

When supported, export Vulkan memory with a compatible external-memory handle and import it through HIP.

Imported memory MUST be cached for the lifetime of the backing allocation.

Do not re-import the same allocation every frame.

---

## INT-004 — Fallback

When external memory is unavailable or fails self-test:

```text
Vulkan GPU resource
 ↓
host-visible/pinned staging
 ↓
HIP copy
 ↓
processing
```

The overlay MUST display:

```text
Interop: HOST-STAGING FALLBACK
```

A warning MUST show estimated transfer cost.

---

## INT-005 — Startup self-test

Before enabling neural processing, the runtime MUST run an interop self-test:

1. Vulkan writes deterministic bytes to an interop buffer;
2. HIP reads and hashes them;
3. HIP writes a second deterministic pattern;
4. Vulkan validates the result;
5. test result is cached per GPU/driver/ROCm combination.

Failure selects fallback mode.

---

# 13. FrameGraph

## FG-001 — Declarative graph

Every effect is represented as a pass.

Example:

```text
CaptureSceneColor
  ↓
LinearizeDepth
  ↓
NormalizeMotion
  ↓
TemporalSR
  ↓
Sharpen
  ↓
ToneAdjustment
  ↓
CompositeToSwapchain
```

---

## FG-002 — Pass interface

Conceptual interface:

```cpp
struct NSPassDescriptor {
    const char* id;
    NSPassType type;

    NSInputRequirement* inputs;
    uint32_t input_count;

    NSOutputDescriptor* outputs;
    uint32_t output_count;

    NSQueuePreference queue;
};
```

---

## FG-003 — Validation

The graph compiler MUST reject:

- missing mandatory inputs;
- cycles not explicitly represented as temporal history;
- incompatible resource formats without conversion;
- multiple writers to the same output without ordering;
- unsupported model scale;
- invalid plugin API versions.

Failure MUST be presented as a human-readable error.

---

## FG-004 — Stable pipeline cache

A graph configuration MUST compile to a reusable execution plan.

The runtime SHOULD avoid rebuilding it every frame.

---

# 14. Temporal history

## TMP-001 — Ring buffers

Temporal resources MUST be persistent GPU allocations.

No new history allocation per frame.

Example:

```text
slot 0
slot 1
slot 2
slot 3
```

---

## TMP-002 — History invalidation

History MUST be invalidated on:

- resolution change;
- swapchain recreation;
- model change;
- pipeline topology change;
- large frame discontinuity;
- explicit user reset;
- detected scene/camera cut when available;
- invalid or missing motion after previously being valid.

A plugin may request additional reset conditions.

---

## TMP-003 — First-frame behavior

Temporal models MUST declare fallback behavior when history is unavailable:

```text
spatial
copy
zero-history
reject
```

A model that requires history and has no valid first-frame behavior MUST not run.

---

# 15. Plugin system

Package extension:

```text
.nsplugin
```

Recommended package structure:

```text
plugin.nsplugin
├── manifest.toml
├── shaders/
├── models/
├── hip/
├── assets/
└── LICENSE
```

---

## PLG-001 — Manifest

Example:

```toml
schema = 1

id = "org.neuroshade.examples.temporal_restore"
name = "Temporal Restore"
version = "1.0.0"
author = "Example"
type = "pipeline"

api = 1

[[passes]]
id = "restore"
backend = "neural"
model = "models/restore.nsmodel"

inputs = [
  "Color.Final",
  "History.Color.1"
]

output = "Output.Color"

history = 1

[[parameters]]
id = "strength"
type = "float"
default = 0.75
min = 0.0
max = 1.0
```

---

## PLG-002 — Plugin API versioning

The runtime MUST expose a numeric API version.

A plugin with incompatible major API MUST not load.

---

## PLG-003 — Plugin hashes

Installed plugin files MUST have recorded SHA-256 hashes.

Modified packages SHOULD be marked:

```text
UNVERIFIED / MODIFIED
```

---

# 16. Shader plugins

## SHD-001 — Runtime format

Runtime shader format:

```text
SPIR-V compute shader
```

GLSL/HLSL source MAY be accepted by installation/development tools but SHOULD be compiled before game launch.

---

## SHD-002 — Standard bindings

Shader SDK MUST define stable bindings for:

```text
input color
depth
motion
history
output
constant parameters
frame metadata
```

---

## SHD-003 — Bundled reference effects

v1 MUST ship with at least:

1. passthrough/copy;
2. sharpen;
3. color adjustment;
4. depth-aware sharpen or edge effect.

These plugins are used for integration testing.

---

# 17. HIP plugins

HIP plugins are an advanced feature.

## HIP-PLG-001

A HIP plugin MAY ship:

```text
HIP source
```

and use HIPRTC/toolchain compilation.

Compiled binaries MUST be cached by:

```text
plugin hash
GPU gfx target
ROCm version
compiler version
compile options
```

---

## HIP-PLG-002 — Trust

The UI MUST show a strong warning before enabling an untrusted HIP/native plugin.

Native/HIP plugins execute with the process' privileges and can destabilize the GPU or game.

---

## HIP-PLG-003 — No ABI dependency on C++ STL across plugin boundary

Native host API MUST use a versioned C ABI.

---

# 18. Neural model format

Runtime extension:

```text
.nsmodel
```

Structure:

```text
model.nsmodel
├── manifest.json
├── model.onnx
├── signature.json
├── metadata.json
└── preview.webp
```

A compiled GPU-specific cache is stored OUTSIDE the package.

Example cache:

```text
~/.cache/neuroshade/models/<model-hash>/<gfx>/<runtime-version>/
```

---

## NN-001 — Manifest

Example:

```json
{
  "schema": 1,
  "id": "org.example.temporal_sr",
  "name": "Temporal SR Tiny",
  "version": "1.0.0",

  "runtime": "migraphx",

  "inputs": [
    {
      "semantic": "Color.LowRes",
      "tensor": "color",
      "dtype": "fp16",
      "layout": "NCHW"
    },
    {
      "semantic": "Depth.Linear",
      "tensor": "depth",
      "dtype": "fp16",
      "layout": "NCHW",
      "optional": true
    },
    {
      "semantic": "Motion.Screen",
      "tensor": "motion",
      "dtype": "fp16",
      "layout": "NCHW",
      "optional": true
    },
    {
      "semantic": "History.Color.1",
      "tensor": "history_color",
      "dtype": "fp16",
      "layout": "NCHW"
    }
  ],

  "output": {
    "semantic": "Output.Color",
    "tensor": "output",
    "dtype": "fp16",
    "layout": "NCHW"
  },

  "history": 1,

  "scale": {
    "x": 2.0,
    "y": 2.0
  },

  "first_frame": "spatial"
}
```

---

## NN-002 — Shape support

Models MUST declare whether shapes are:

```text
fixed
dynamic
bucketed
```

For production performance, bucketed shapes are preferred.

Example:

```text
1280×720  → 2560×1440
960×540   → 1920×1080
1920×1080 → 3840×2160
```

---

## NN-003 — Warm-up

A neural model MUST be warmed up before becoming active.

The first real gameplay frame MUST NOT be used to trigger expensive compilation if a cached compile can be produced before launch or during an explicit loading stage.

---

# 19. `.pth` importer

Tool:

```text
nsmodel
```

Basic usage:

```bash
nsmodel import model.pth \
  --adapter my_model_adapter.py \
  --output model.nsmodel
```

---

## IMP-001 — Isolation

`.pth` import MUST occur outside the game process.

Preferred implementation:

```text
dedicated Python virtual environment / subprocess
```

---

## IMP-002 — Weights-only default

Where technically possible, model loading MUST default to a weights-only safe path.

If the user requests loading a serialized Python object, the importer MUST require explicit unsafe opt-in.

---

## IMP-003 — Architecture adapters

Because a state dictionary does not define the network architecture, import MUST support adapters.

Adapter responsibilities:

1. instantiate architecture;
2. load state dict;
3. declare example inputs;
4. declare temporal semantics;
5. export an intermediate graph;
6. validate numerical output.

---

## IMP-004 — Import verification

The importer MUST compare reference output between source PyTorch and exported model using deterministic input tensors.

Import fails when numerical deviation exceeds the adapter/model tolerance.

---

## IMP-005 — Runtime compile

After successful conversion, the system SHOULD test MIGraphX compile before installation.

Unsupported operators produce a clear error.

Optional PyTorch compatibility backend MAY be offered later.

---

# 20. Neural execution

## NNR-001 — No Python in render loop

The render process MUST NOT call Python.

---

## NNR-002 — Preallocated tensors

Input/output/scratch allocations SHOULD be persistent.

No general heap allocation per frame in the neural steady state.

---

## NNR-003 — Stream

The neural runtime MUST own or receive a documented HIP stream.

All HIP kernels in a neural pass MUST be associated with the correct stream/order.

---

## NNR-004 — Failure containment

If neural execution fails:

1. disable the failing pass;
2. preserve the previous valid pipeline;
3. use shader/copy fallback;
4. display error in overlay;
5. write diagnostic log.

The game SHOULD continue whenever technically possible.

---

# 21. Pipeline presets

Profile example:

```json
{
  "game": {
    "executable": "game.exe"
  },

  "resources": {
    "depth": 417,
    "motion": 882,
    "low_res_color": 151
  },

  "pipeline": [
    {
      "plugin": "org.example.temporal_sr",
      "enabled": true,
      "parameters": {
        "quality": "balanced"
      }
    },
    {
      "plugin": "org.neuroshade.sharpen",
      "enabled": true,
      "parameters": {
        "strength": 0.2
      }
    }
  ]
}
```

Resource bindings SHOULD additionally use fingerprints so they can survive handle changes between runs.

---

# 22. Resource fingerprints

Persisting raw VkImage IDs is insufficient.

A binding fingerprint SHOULD include:

```text
format
extent relative to output
usage
attachment behavior
creation order bucket
write/read pattern
render-pass relationship
```

On next launch:

```text
saved fingerprint
 ↓
candidate matching
 ↓
confidence score
 ↓
automatic rebind
```

If confidence is low, the user is prompted to reselect.

---

# 23. Overlay

Recommended implementation: Dear ImGui or equivalent native Vulkan overlay.

Toggle default:

```text
Home
```

Tabs:

```text
Overview
Pipeline
Resources
Models
Profiler
Compatibility
Logs
```

---

## UI-001 — Overview

Show:

```text
Game
GPU
gfx target
ROCm/HIP version
Vulkan driver
Interop mode
Compatibility class
Active profile
```

---

## UI-002 — Pipeline editor

User can:

- enable/disable passes;
- reorder compatible passes;
- edit parameters;
- select a model;
- save preset;
- restore defaults.

Invalid order MUST be rejected by FrameGraph validation.

---

## UI-003 — Resource inspector

For each candidate:

- dimensions;
- format;
- usage;
- score;
- preview;
- semantic binding actions.

---

## UI-004 — Profiler

Show per frame:

```text
NeuroShade total GPU/CPU cost
Vulkan conversion
shader passes
HIP kernels
neural pass
history copy
composite
frame time
estimated FPS impact
VRAM usage
```

Moving averages MUST be used to avoid unreadable flicker.

---

# 24. External desktop application

A desktop frontend is part of the production experience but is not allowed in the hot path.

Functions:

- detect AMD GPU and ROCm;
- run compatibility self-test;
- list installed plugins;
- import `.pth`;
- install `.nsmodel`;
- manage game profiles;
- manage caches;
- launch games;
- generate Steam launch option;
- display diagnostics.

The frontend communicates with tools/config files, not directly with frame processing.

---

# 25. CLI

Required commands:

```text
neuroshade doctor
neuroshade launch <command...>
neuroshade profile list
neuroshade profile show <game>
neuroshade plugin list
neuroshade plugin install <file>
neuroshade model list
neuroshade model import <pth>
neuroshade cache clear
```

Example:

```bash
neuroshade doctor
```

Expected output class:

```text
GPU: AMD Radeon ...
gfx: gfx....
Vulkan: OK
ROCm/HIP: OK
External memory: PASS
Neural runtime: PASS
Interop mode: ZERO-COPY BUFFER
Compatibility: A
```

---

# 26. Steam/Proton integration

Install wrapper:

```text
~/.local/bin/neuroshade-run
```

Recommended Steam launch option:

```text
neuroshade-run %command%
```

The wrapper:

1. identifies executable/game;
2. loads profile;
3. sets layer path;
4. enables `VK_LAYER_NEUROSHADE`;
5. sets NeuroShade profile/config variables;
6. executes original command.

The wrapper MUST preserve arguments and exit status.

---

# 27. Logging

Logs stored under:

```text
$XDG_STATE_HOME/neuroshade/
```

or fallback:

```text
~/.local/state/neuroshade/
```

Per-session log MUST contain:

```text
runtime version
GPU
gfx target
driver
Vulkan version
ROCm version
profile
plugin versions
model hashes
interop mode
resource selections
pipeline compile result
fatal/nonfatal errors
```

Do not log arbitrary game memory contents.

---

# 28. Crash diagnostics

When possible, the runtime SHOULD generate a concise crash marker containing:

```text
last frame number
last FrameGraph pass
last plugin
last HIP error
last Vulkan error
```

Do not install signal handlers that break the game's own crash handling unless explicitly enabled.

---

# 29. Performance requirements

## PERF-001 — Disabled overhead

With NeuroShade loaded but all processing disabled:

- no frame readback;
- no neural work;
- no unnecessary resource copies;
- no per-frame heap churn.

Release acceptance target:

```text
< 1% average FPS regression
< 2% regression in 1% lows
```

on the project reference testbed.

---

## PERF-002 — Allocation

After steady state begins:

```text
0 Vulkan allocations/frame
0 HIP allocations/frame
0 model compilations/frame
0 plugin file reads/frame
```

Unexpected allocation counts MUST be profiler-visible in debug builds.

---

## PERF-003 — Neural budget

Each model MUST expose measured cost for known resolutions on the local GPU after warm-up.

The runtime SHOULD warn when:

```text
effect cost > configured frame budget
```

Example user setting:

```text
NeuroShade budget = 4.0 ms
```

---

## PERF-004 — Adaptive bypass

Optional v1 feature:

if processing repeatedly exceeds budget, runtime MAY:

- skip optional pass;
- lower model quality preset;
- bypass one frame;
- notify user.

It MUST NOT silently change visual quality unless adaptive mode is enabled.

---

# 30. Memory requirements

The runtime MUST show additional VRAM use.

The memory planner MUST account for:

```text
interop buffers
temporal history
model weights
model workspace
shader intermediates
resource previews
```

When allocation would exceed configured budget, graph activation MUST fail gracefully.

---

# 31. Texture enhancement scope

Generic high-resolution texture replacement is NOT required for v1 because replacing arbitrary game images can require proxy resource/descriptor substitution.

v1 SHALL implement the groundwork:

- texture upload observation;
- texture candidate identification;
- hashing where safely possible;
- optional capture/export for debugging;
- architecture for a future `Texture.Resource` plugin type.

Optional v1 experimental mode MAY process textures in-place when:

- dimensions do not change;
- format is supported;
- synchronization is known;
- replacement is safe.

True dimension-changing neural texture replacement is v2.

This restriction exists to ensure v1 actually ships.

---

# 32. Anti-cheat policy

NeuroShade MUST NOT:

- hide its Vulkan layer;
- bypass anti-cheat checks;
- spoof modules;
- patch anti-cheat;
- inject into protected games against their policy.

The launcher SHOULD maintain a deny/warning mechanism.

Default behavior for known protected online games:

```text
Do not launch NeuroShade automatically.
```

The primary release target is:

- single-player;
- offline;
- games without hostile anti-cheat;
- development/test applications.

---

# 33. Security

## SEC-001

Never `torch.load()` arbitrary community `.pth` inside the game process.

## SEC-002

`.pth` conversion runs in an isolated importer process.

## SEC-003

Plugin packages are hashed.

## SEC-004

Native/HIP plugins are marked unsafe/trusted-code.

## SEC-005

Shader/model plugins MUST NOT receive arbitrary host pointers.

## SEC-006

Configuration parsing MUST reject invalid sizes, overflows, path traversal, and unsupported schema versions.

---

# 34. Testing strategy

Testing is not optional.

---

## 34.1 Unit tests

Required modules:

```text
manifest parser
profile parser
framegraph compiler
resource fingerprints
ring history
model signature validation
plugin version validation
cache keys
format conversions
```

---

## 34.2 `ns-testbed`

The repository MUST contain a deterministic Vulkan test application.

It renders:

```text
animated color pattern
known depth geometry
known motion vectors
known low-resolution scene color
known normals
```

It is the canonical integration target.

This eliminates dependency on commercial games during development.

---

## 34.3 Interop tests

Test:

```text
Vulkan → HIP
HIP → Vulkan
repeated mapping/use
triple-buffer rotation
resolution change
device loss path
```

Data integrity MUST be verified with deterministic checksums/patterns.

---

## 34.4 Shader tests

Each bundled shader MUST have a deterministic screenshot/image comparison test with tolerance.

---

## 34.5 Neural tests

For bundled model:

- fixed tensor input;
- expected tensor output tolerance;
- temporal first frame;
- second frame with history;
- history reset;
- resolution switch;
- invalid missing input.

---

## 34.6 Vulkan validation

Development/CI test runs MUST support Khronos validation layers.

Release runtime MUST NOT enable validation layers by default.

---

## 34.7 Proton smoke tests

Before v1 release, qualification MUST include:

- at least one DX11 → DXVK path;
- at least one DX12 → VKD3D-Proton path;
- at least one native Vulkan path.

The exact games/apps used MUST be documented in `COMPATIBILITY.md`.

---

# 35. Continuous integration

CI minimum:

```text
format
lint
C++ build
unit tests
manifest/schema tests
package build
```

GPU integration tests run on a dedicated AMD runner when available.

No release tag may be produced when mandatory CPU-side tests fail.

---

# 36. Build system

CMake is the canonical build system.

Minimum conceptual flow:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build -j
ctest --test-dir build
```

HIP sources MUST compile for explicitly selected gfx targets or a configured supported target list.

Developer command:

```bash
rocminfo | grep gfx
```

---

# 37. Installation layout

User-local default:

```text
~/.local/lib/neuroshade/
~/.local/share/neuroshade/
~/.local/share/vulkan/explicit_layer.d/
~/.config/neuroshade/
~/.cache/neuroshade/
~/.local/state/neuroshade/
```

No root installation is required for normal use.

---

# 38. Packaging

v1 MUST provide at least:

1. tarball/portable package;
2. install script;
3. uninstall script.

Desktop frontend MAY be AppImage.

The Vulkan Layer itself MUST remain available as a normal `.so` + manifest independent of the frontend packaging.

---

# 39. Configuration schema

All persistent configuration schemas MUST have:

```text
schema_version
```

Old configuration SHOULD migrate forward where practical.

Unknown future fields SHOULD be ignored when safe.

Invalid critical fields MUST produce explicit errors.

---

# 40. Error model

Use structured errors.

Example classes:

```text
NS_ERR_VULKAN
NS_ERR_HIP
NS_ERR_INTEROP
NS_ERR_MODEL
NS_ERR_PLUGIN
NS_ERR_RESOURCE
NS_ERR_FRAMEGRAPH
NS_ERR_PROFILE
NS_ERR_UNSUPPORTED
```

An error MUST include:

```text
code
message
subsystem
plugin/model if applicable
recoverable yes/no
```

---

# 41. Production modes

Runtime mode enum:

```text
PASS_THROUGH
SHADER_ONLY
NEURAL_SPATIAL
NEURAL_TEMPORAL
TEMPORAL_SUPER_RESOLUTION
```

The overlay MUST display the active mode.

---

# 42. Bundled demo neural model

v1 MUST ship with or provide a reproducibly downloadable/buildable small open model compatible with the project license.

Its role is not to beat DLSS.

Its role is to prove:

```text
Frame N
History N-1
optional depth/motion
 ↓
MIGraphX/HIP inference
 ↓
processed output
```

The test model MUST be small enough for routine CI/dev testing on supported Radeon hardware.

If licensing prevents redistribution, scripts and checksums MUST reproduce acquisition/conversion.

---

# 43. v1 user journey

A successful user journey MUST be:

```text
1. Install NeuroShade
2. Run `neuroshade doctor`
3. Select/add game
4. Configure `neuroshade-run %command%`
5. Launch game
6. Open overlay with Home
7. See detected final color/depth candidates
8. Enable bundled shader
9. See visual change
10. Enable bundled neural model
11. See neural pass execute
12. See GPU cost in profiler
13. Save profile
14. Restart game
15. Profile reloads automatically
```

For a temporal-SR-capable profile:

```text
16. Bind low-resolution scene color
17. Bind depth
18. Bind motion
19. Enable temporal SR model
20. Validate temporal history
21. Save bindings
22. Restart and automatically recover bindings
```

---

# 44. Functional requirements matrix

| ID | Requirement | Priority | v1 blocker |
|---|---|---:|---:|
| FR-001 | Vulkan layer loads per-game | P0 | Yes |
| FR-002 | Pass-through does not break testbed | P0 | Yes |
| FR-003 | Swapchain present interception | P0 | Yes |
| FR-004 | Final color capture | P0 | Yes |
| FR-005 | SPIR-V shader pass | P0 | Yes |
| FR-006 | FrameGraph | P0 | Yes |
| FR-007 | Resource tracker | P0 | Yes |
| FR-008 | Depth candidate detection | P0 | Yes |
| FR-009 | Manual resource binding | P0 | Yes |
| FR-010 | Vulkan↔HIP interop self-test | P0 | Yes |
| FR-011 | HIP processing pass | P0 | Yes |
| FR-012 | Host-staging fallback | P0 | Yes |
| FR-013 | `.nsmodel` loader | P0 | Yes |
| FR-014 | MIGraphX inference | P0 | Yes |
| FR-015 | Temporal history | P0 | Yes |
| FR-016 | Bundled temporal model | P0 | Yes |
| FR-017 | Overlay | P0 | Yes |
| FR-018 | Profiler | P0 | Yes |
| FR-019 | Persistent game profile | P0 | Yes |
| FR-020 | Launcher/wrapper | P0 | Yes |
| FR-021 | `.pth` importer | P0 | Yes |
| FR-022 | Proton DX11 smoke test | P0 | Yes |
| FR-023 | Proton DX12 smoke test | P0 | Yes |
| FR-024 | Resource preview | P0 | Yes |
| FR-025 | Motion candidate/manual binding | P1 | No |
| FR-026 | True temporal SR profile | P1 | Release target |
| FR-027 | HIP plugin SDK | P1 | No |
| FR-028 | Desktop frontend | P1 | Yes for product release |
| FR-029 | Texture observation | P1 | No |
| FR-030 | Texture replacement/upscale | P2 | No |
| FR-031 | Windows native D3D12 | P2 | No |
| FR-032 | Frame generation | P2 | No |

---

# 45. Milestones

Milestones are sequential. A milestone is complete only when its acceptance tests pass.

---

## M0 — Repository and deterministic testbed

Deliver:

- repository structure;
- CMake;
- logging;
- Vulkan testbed;
- CI;
- unit-test harness.

Acceptance:

```text
ns-testbed renders deterministic animation
CI builds Release and Debug
tests pass
```

---

## M1 — Production-safe Vulkan Layer

Deliver:

- explicit Vulkan Layer;
- dispatch tables;
- swapchain tracking;
- per-game launch wrapper;
- pass-through.

Acceptance:

```text
native Vulkan testbed runs through layer
output matches without effects
no validation errors caused by layer
layer can be disabled without reinstalling
```

---

## M2 — Shader FrameGraph

Deliver:

- FrameGraph;
- resource abstraction;
- final-color capture;
- SPIR-V compute passes;
- output composite;
- plugin manifest parser.

Acceptance:

```text
bundled sharpen modifies testbed frame
effect can be toggled live
two shader effects can be ordered
profile persists
```

At this point NeuroShade is already a minimal ReShade-like Vulkan product.

---

## M3 — Resource analyzer

Deliver:

- tracked images;
- depth candidates;
- scene-color candidates;
- resource preview;
- manual binding;
- profile fingerprinting.

Acceptance:

```text
testbed depth is identified
user can intentionally choose wrong candidate
user can rebind correct candidate
binding survives restart
```

---

## M4 — HIP interop

Deliver:

- exportable Vulkan buffer pool;
- canonical image→buffer conversion;
- HIP external-memory import path;
- self-test;
- host-staging fallback;
- simple HIP kernel pass.

Acceptance:

```text
Vulkan pattern read correctly by HIP
HIP pattern read correctly by Vulkan
HIP color effect visible in testbed
no CPU frame readback in preferred path
fallback can be forced for test
```

---

## M5 — Neural spatial runtime

Deliver:

- `.nsmodel`;
- MIGraphX integration;
- tensor buffer planning;
- warmup/cache;
- spatial neural model.

Acceptance:

```text
bundled neural model changes output
no Python process is required after game launch
model warms before activation
failure falls back to copy/shader
```

---

## M6 — Temporal runtime

Deliver:

- history ring;
- history invalidation;
- temporal model semantics;
- profiler;
- bundled temporal model.

Acceptance:

```text
model consumes N and N-1
history reset works
swapchain recreation does not use stale history
resolution change does not crash
```

---

## M7 — `.pth` model importer

Deliver:

- isolated Python importer;
- adapter API;
- weights-only default;
- ONNX/intermediate export;
- numerical verification;
- `.nsmodel` packaging.

Acceptance:

```text
known reference .pth imports
source/export outputs agree within tolerance
bad architecture fails clearly
unsupported MIGraphX op fails before game launch
```

---

## M8 — Motion + true temporal SR path

Deliver:

- motion candidate scoring;
- motion preview;
- manual binding;
- low-resolution scene source;
- 2× temporal-SR example profile.

Acceptance:

```text
testbed renders low-res scene
model reconstructs higher-res output
motion vectors affect temporal result
missing motion triggers declared fallback
```

---

## M9 — Productization

Deliver:

- desktop frontend;
- installer;
- uninstaller;
- plugin manager;
- model manager;
- diagnostic doctor;
- release packaging;
- compatibility documentation;
- Proton qualification.

Acceptance:

The complete v1 user journey in section 43 passes from a clean user account.

---

# 46. Definition of Done — v1.0

v1.0 MUST NOT be tagged until every item below is true.

## Core

- [ ] Vulkan Layer installs without root.
- [ ] Layer is explicitly activated per game.
- [ ] Native Vulkan testbed works.
- [ ] DX11 Proton smoke test works.
- [ ] DX12 Proton smoke test works.
- [ ] Pass-through works.
- [ ] Shader plugin works.
- [ ] HIP pass works.
- [ ] Neural spatial model works.
- [ ] Neural temporal model works.
- [ ] History resets correctly.
- [ ] Resource inspector works.
- [ ] Depth manual binding works.
- [ ] Motion manual binding works on testbed.
- [ ] Profiles persist.
- [ ] Profiler reports pass costs.
- [ ] Interop self-test exists.
- [ ] Interop fallback exists.
- [ ] No Python is required in render loop.
- [ ] `.pth` importer produces `.nsmodel`.
- [ ] Invalid models fail before gameplay when possible.

## Reliability

- [ ] 30-minute automated testbed soak test passes.
- [ ] 30-minute temporal-model soak test passes.
- [ ] repeated swapchain recreation passes.
- [ ] fullscreen/windowed transitions tested where applicable.
- [ ] plugin enable/disable loop tested.
- [ ] model enable/disable loop tested.
- [ ] no steady-state per-frame GPU allocations.
- [ ] no known resource-lifetime leak in tracked test scenarios.

## Product

- [ ] installer exists;
- [ ] uninstaller exists;
- [ ] `neuroshade doctor` exists;
- [ ] overlay exists;
- [ ] desktop frontend exists;
- [ ] logs are user-accessible;
- [ ] compatibility class is visible;
- [ ] README installation flow is tested;
- [ ] at least one bundled shader plugin exists;
- [ ] at least one bundled temporal neural example exists;
- [ ] release artifacts are generated by CI or reproducible release scripts.

## Safety

- [ ] anti-cheat warning policy exists;
- [ ] no stealth injection;
- [ ] arbitrary `.pth` is not loaded in game;
- [ ] untrusted HIP/native plugins require explicit opt-in;
- [ ] plugin hashes are recorded.

---

# 47. Release gates

A release candidate is rejected for any of the following:

```text
game corruption caused by disabled/pass-through layer
unbounded VRAM growth
per-frame model recompilation
per-frame resource allocation in steady state
stale temporal history after resize
crash when an optional semantic resource is absent
silent fallback from zero-copy without UI indication
arbitrary .pth execution inside game process
profile corruption after normal shutdown
Vulkan validation errors introduced by normal testbed path
```

---

# 48. Coding rules

## C++

- C++20;
- RAII for owned Vulkan/HIP resources;
- no exceptions across plugin ABI;
- explicit ownership;
- `std::span`/views preferred over naked pointer+length internally;
- no global mutable Vulkan state;
- no C++ ABI exposed to third-party native plugins.

## HIP

- no device allocation inside per-frame kernels;
- errors checked in Debug and production-critical boundaries;
- kernels require explicit dimensional bounds checks where needed;
- gfx architecture recorded in binary cache.

## Vulkan

- resource layout transitions explicit;
- synchronization centralized in backend;
- layer state scoped to Vulkan instance/device;
- no guessed resource lifetime.

---

# 49. Observability

Every FrameGraph pass receives a stable identifier.

Example:

```text
capture.color
convert.depth
plugin.cas
neural.temporal_sr
composite.present
```

The profiler and logs use these IDs.

A diagnostic export MAY create:

```text
diagnostic.zip
├── system.json
├── profile.json
├── plugins.json
├── model-metadata.json
└── session.log
```

No screenshots/frame data are included unless user explicitly requests capture.

---

# 50. Future roadmap after v1

Not part of current Definition of Done.

## v1.1

- improved automatic motion discovery;
- async interop synchronization where proven reliable;
- better model shape cache;
- quality auto-budgeting;
- community profile sharing.

## v1.5

- proxy resources;
- neural texture restoration;
- texture cache;
- normal/roughness reconstruction;
- material enhancement.

## v2

- dimension-changing generic texture replacement;
- native Windows D3D12 backend if HIP/runtime support is sufficient;
- game-specific adapter SDK;
- frame generation research;
- deeper engine resource reconstruction.

---

# 51. Reference implementation pipeline

The first complete temporal pipeline SHOULD be:

```text
Game Render
   ↓
Resolve selected scene color
   ↓
Vulkan Convert → canonical FP16 buffer
   ↓
Depth linearization
   ↓
Motion normalization
   ↓
HIP-visible interop buffers
   ↓
Temporal history resolve
   ↓
MIGraphX Temporal Model
   ↓
Optional SPIR-V sharpen
   ↓
Vulkan output conversion
   ↓
Swapchain image
   ↓
Present
```

Fallback pipeline:

```text
Game Render
   ↓
Shader-only enhancement
   ↓
Present
```

Fatal initialization fallback:

```text
Game Render
   ↓
Native Present
```

The native game must remain the final fallback.

---

# 52. Acceptance scenario: temporal neural enhancement

Given:

```text
1920×1080 Vulkan game
Color.Final available
Depth optional
History enabled
```

When:

```text
Temporal Restore plugin is enabled
```

Then:

1. current frame is converted into canonical GPU buffer;
2. previous frame is available from GPU history;
3. MIGraphX executes model;
4. output is composited into presentation image;
5. profiler displays model time;
6. no full frame is copied through CPU in preferred interop mode;
7. after resize, first new-resolution frame starts with invalid history;
8. game continues if model is disabled.

---

# 53. Acceptance scenario: true temporal 2× SR

Given:

```text
Color.LowRes = 960×540
Output = 1920×1080
Depth.Linear bound
Motion.Screen bound
Temporal 2× model installed
```

When:

```text
Temporal SR is enabled
```

Then:

1. low-res source is used instead of the already-upscaled final frame;
2. depth and motion are normalized;
3. temporal history uses matching low-resolution semantics required by model;
4. model outputs 1920×1080;
5. final result replaces/composites into swapchain image;
6. resource mismatch disables the SR pass rather than reading unknown memory;
7. profile stores resource fingerprints;
8. restart rebinds with a confidence score or requests manual correction.

---

# 54. Acceptance scenario: importing `.pth`

Given:

```text
model.pth
adapter.py
```

When:

```bash
neuroshade model import model.pth --adapter adapter.py
```

Then:

1. importer creates isolated process;
2. weights are loaded using the safest compatible method;
3. adapter instantiates architecture;
4. deterministic reference input is generated;
5. model is exported;
6. source and exported output are compared;
7. MIGraphX compatibility is tested;
8. `.nsmodel` is created;
9. package hash is recorded;
10. model appears in launcher;
11. game never opens the original `.pth`.

---

# 55. Source-of-truth rule for development agents

Any coding agent working from this repository MUST:

1. read `SPEC.md` before implementing a feature;
2. identify requirement IDs affected;
3. avoid implementing P2 work while P0 blockers remain unless explicitly requested;
4. add/update tests for affected acceptance criteria;
5. update this SPEC if an architectural contract changes;
6. never silently reinterpret a hard decision;
7. report incomplete acceptance criteria honestly.

A feature is not complete because it compiles.

A feature is complete only when its SPEC acceptance criteria pass.

---

# 56. Suggested issue format

```markdown
## Requirement

FR-XXX / subsystem requirement

## Goal

What behavior from SPEC is being implemented.

## Implementation

Files/subsystems affected.

## Acceptance

- [ ] ...
- [ ] ...
- [ ] ...

## Tests

Commands and test cases.

## Risks

Compatibility/performance/resource-lifetime concerns.

## SPEC changes

None / link to explicit amendment.
```

---

# 57. Suggested PR gate

Every PR SHOULD answer:

```text
Which SPEC IDs does this implement?
Which acceptance tests pass?
Does it allocate anything per frame?
Does it alter Vulkan synchronization?
Does it alter resource ownership?
Does it affect plugin ABI?
Does it affect model ABI?
What is the failure fallback?
```

PRs that modify synchronization, interop, ABI, or temporal history require dedicated integration testing.

---

# 58. Final v1 product statement

When v1.0 is complete, NeuroShade is:

> A user-installable AMD Radeon Vulkan neural post-processing runtime for Linux/Proton that can intercept game frames, inspect and bind render resources, run SPIR-V and HIP-backed effects, import temporal PyTorch `.pth` models into a safe runtime package, execute supported models through ROCm/MIGraphX with persistent temporal history, profile their cost, save per-game configurations, and always retain a safe native-present fallback.

It is NOT yet:

> a universal DLSS replacement, universal game texture remasterer, frame generator, anti-cheat bypass, or cross-vendor graphics runtime.

That boundary is intentional. It is what makes a functional v1 achievable.

---

# 59. Technical references

Implementation must verify the exact behavior against the versions used in the build.

- Khronos Vulkan Guide — Layers: https://docs.vulkan.org/guide/latest/layers.html
- Khronos Vulkan Guide — Loader: https://docs.vulkan.org/guide/latest/loader.html
- AMD HIP Runtime API — External Resource Interoperability: https://rocm.docs.amd.com/projects/HIP/en/docs-7.2.0/how-to/hip_runtime_api/external_interop.html
- AMD HIP Runtime API Reference: https://rocm.docs.amd.com/projects/HIP/en/latest/doxygen/html/
- AMD ROCm — PyTorch on ROCm installation: https://rocm.docs.amd.com/projects/install-on-linux/en/docs-7.2.2/install/3rd-party/pytorch-install.html
- AMD MIGraphX documentation: https://rocm.docs.amd.com/projects/AMDMIGraphX/en/latest/
- PyTorch serialization notes: https://docs.pytorch.org/docs/stable/notes/serialization.html

> **Important implementation note:** HIP external-resource APIs and their Linux synchronization support have changed across releases and documentation branches. The runtime MUST perform capability/self-tests rather than assuming that external semaphore operations are available because the symbols exist. The v1 baseline therefore allows host-coordinated GPU synchronization while preserving GPU-resident frame memory.

