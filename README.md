# pigs: Pi Gaussian Splat Renderer


https://github.com/user-attachments/assets/d81c6e70-77da-4a90-9f70-14543ebd557b


## Intro

We built an (almost) real-time Gaussian splat renderer on the Raspberry Pi! It has three states:
- A menu screen listing all .ply files from the SD card
- A renderer screen rendering a splat model selected from the menu
- A toggleable instruction screen showing the commands

In the renderer, the camera orbits the model in a spherical path by default, but you can toggle manual camera controls as well as zoom in and out.

### Gaussian splatting background
A bit of background on the Gaussian splatting pipeline:
1. Split screen into tiles (we did 16x16 pixels to align with 16-wide QPU registers)
2. For each Gaussian, calculate some attributes (screen space coordinates, depth from camera, inverse covariance matrix, spherical harmonics, etc.)
3. For each Gaussian, calculate which tiles it intersects
4. Do a global sort over all Gaussian-tile intersections by (tile, depth) keys, so that all Gaussians belonging to a tile are contiguous and sorted by depth
5. For each tile, rasterize Gaussians in sorted order and alpha blend to framebuffer

As you can see, lots of room for parallelism!

## Main components
Our project consisted of these main components:

### QPU kernels
We have seven QPU kernels that help preprocess and render the Gaussians. Not going into them in detail, but here are some cool tricks we used:
- TMU for gather operations, since VPM can only access consecutive or strided memory
- Parallel binary search within a QPU register (check `calc_tile.qasm`)
- Iterate from MSB to LSB, add that bit, then mask + subtract from lanes where some condition isn’t met
- Packing/unpacking to convert float32 to uint8 for pixel colors on QPU hardware (check render.qasm)
- Making use of register rotations
    - ex. Parallel prefix scan kernels, check `scan_rot.qasm` and `scan_sum.qasm`
- Making use of SFU for special math ops (sqrt, division, exp)
- Abusing unions to store multiple data of different types
    - ex. We don’t need the radius (float) attribute after calculating tile intersections so we reuse it as an ID (int)
- Kernel abstraction for any qasm code + loading unifs
- Asynchronous kernel launching: since kernels execute asynchronously on the QPU, we can sort (the only part of the pipeline on CPU) while rendering on the QPU for massive speedups
- Double buffering: we allocate a framebuffer twice as large as the physical screen and change the virtual offset to double buffer, always rendering to the “inactive” buffer
Actually, we allocate a framebuffer 3x as large as the physical screen and use the third buffer as the static instruction screen

### Arena allocator
Simple bump allocator for managing regions of memory, allowing for easily creating/destroying arenas after they’re done being used:
- Very useful in rendering — just allocate an arena per frame and destroy after rendering
- Also used for the heap allocator. It’s nice because it’s just an abstraction over some memory region so you can use it for GPU memory or heap memory or whatever.

### SD card
Implemented FAT32 driver from scratch, mostly based off of the lab but also allowing long file names.

### MMU
Turned on the MMU with a simple identity page table to enable the data cache, also had massive speedups.

### Interrupts / Threads
Cooperative and preemptive threads to allow for seamless switching between menu, render, and instruction states:
- UART interrupts for keyboard commands
- Used supervisor calls to generate SWIs for cooperative threads so they can use the same interrupt handler as preemptive threads
    - We used cooperative threads for switching from the menu screen to other threads because the menu thread has to do some memory setup/tear down that we don’t want to interrupt
    - We used preemptive threads for the renderer and instruction screen because you wanna be able to quit them at any time and the menu thread will handle the mess

### Bitmap font
Hand-wrote a bunch of characters from this font to display text on the framebuffer: https://int10h.org/oldschool-pc-fonts/fontlist/font?dos-v_twn16.
