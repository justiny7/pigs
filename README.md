# pigs: Pi Gaussian Splat Renderer


https://github.com/user-attachments/assets/d81c6e70-77da-4a90-9f70-14543ebd557b

## Intro

This is an (almost) real-time Gaussian splat renderer on a Raspberry Pi Zero W! It has three states:
- A menu screen listing all .ply files from the SD card
- A renderer screen rendering a splat model selected from the menu
- A toggleable instruction screen showing the commands

In the renderer, the camera orbits the model in a spherical path by default, but you can toggle manual camera controls as well as zoom in and out.

## Setup

### Installation
I'm using a Raspberry Pi Zero W in a bare-metal environment. Make sure you have some cross compiler that can compile C code with no Linux libraries (I'm using musl's `arm-none-eabi` toolchain on Mac).
- This codebase is built on top of a bare-metal OS I've been working on at [rpi\_os](https://github.com/justiny7/rpi_os). It comes with a bootloader which makes uploading kernel images to the Pi easier - feel free to follow the instructions in that repository to set it up, or use your own bootloader (or just copy the kernel to the SD card)
- You'll also need to install vc4asm, an assembler for VideoCore IV kernels. On Mac, you can do so by running `brew install vc4asm`.
    - **Note**: there is one issue I ran into when compiling vc4 code with not being able to find the built-in include files (`vc4.qinc`). To fix this, I updated the qasm rules in the Makefile (lines 67-69) with the absolute path to where my vc4 binary is stored. You may have to do the same.

With this, you can clone and compile the repo:
```bash
# clone
git clone xxx
cd pigs
git submodule init --recursive

# compile
make                    # (if your make version is too old, you might get an error - I used homebrew's gmake)
rpi-install kernel.img  # (or use your own bootloader / copy to SD card)
```

The program looks for all `.ply` files in the root of your SD card, but they must be formatted in a certain way (according to the `Gaussian` struct in [include/gaussian.h](include/gaussian.h)). This should be the way most plys are formatted by default, but you should double check. The program also uses the regular right-handed camera orientation (Y up, X right, Z out). I've compiled some pre-formatted splats you can find [here](https://drive.google.com/drive/folders/1IMnQH97K2bap2XiZzAO3b9djTZOY_Kj1?usp=sharing) (sources below). These are the ones in the demo - since the Pi Zero only has 512 MB of RAM, I tried finding models with less than 300K Gaussians.

**IMPORTANT**: since the program allocates a lot of GPU memory, you need to make sure there is an adequate CPU/GPU memory split in your `config.txt`. Make sure `gpu_mem` is at least 350 (I have it at 384 by default, leaving 128MB for the CPU). I also have some overclocking settings enabled if you want faster rendering speeds. You can reference the [`config.txt`](firmware/config.txt) in this repo.

Finally, you'll need some way to connect an HDMI device to your Pi so you can see the display!

### Instructions

| Mode | Keys | Control |
| ----- | ---- | ----- |
| Menu | I, K, Enter | Select model (up/down/select) |
| Renderer | I, K | Zoom in/out|
| Renderer | Space | Reset zoom to default |
| Renderer | C | Toggle camera-control mode |
| Renderer | W, A, S, D | Move camera in camera-control mode |
| Both | H | Toggle help menu |

## How it works

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
    - (Actually, we allocate a framebuffer 3x as large as the physical screen and use the third buffer as the static instruction screen)

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
Hand-wrote a bunch of characters from [this font](https://int10h.org/oldschool-pc-fonts/fontlist/font?dos-v_twn16) to display text on the framebuffer.


## Sources
- [Fly splat (direct download)](https://github.com/danybittel/splats/releases/download/splat/cluster.fly.zip)
- [Cactus splat](https://note.com/steam_studio/n/ne9736d94f162)
- [All other splats](https://huggingface.co/VladKobranov/splats/tree/main)
- [DOS/V TWN16 font](https://int10h.org/oldschool-pc-fonts/fontlist/font?dos-v_twn16)

