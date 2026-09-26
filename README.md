# Basic Minecraft implementation with ASCII characters in C

A simple 3D raycasting engine inspired by Minecraft, rendered in the terminal using ASCII characters and color codes. It features world generation, player movement, physics with jumping, and a functioning block inventory/hotbar system!

![Gameplay Screenshot](source/readme_image.png) 

## Prerequisites

You need a C compiler (like GCC) and the `make` utility installed.

*   **macOS:**
    *   Install Xcode Command Line Tools: `xcode-select --install`
*   **Windows:**
    *   Install [MSYS2](https://www.msys2.org/).
    *   From the MSYS2 terminal, install the necessary tools: `pacman -S mingw-w64-x86_64-gcc make`
*   **Linux:**
    *   Use your distribution's package manager, e.g., `sudo apt update && sudo apt install build-essential` (Debian/Ubuntu) or `sudo dnf groupinstall "Development Tools"` (Fedora).

## Compilation

1.  Clone the repository (if you haven't already).
2.  Navigate to the project directory in your terminal (or MSYS2 terminal on Windows).
3.  Run the `make` command:
    ```bash
    make
    ```
    This will compile the `minecraft.c` source file and create an executable file named `minecraft` (or `minecraft.exe` on Windows).

## Running

After successful compilation, run the executable from your terminal:

*   **macOS / Linux:**
    ```bash
    ./minecraft
    ```
*   **Windows (MSYS2 terminal):**
    ```bash
    ./minecraft.exe
    ```

Make sure your terminal window is large enough to display the output correctly (recommended ~180 rows, ~900 columns, although you can experiment).

## Controls

*   **`w` / `s`**: Look up / down
*   **`a` / `d`**: Look left / right
*   **`i` / `k`**: Move forward / backward
*   **`j` / `l`**: Strafe left / right
*   **`space`**: Jump
*   **`x`**: Break the block you are looking at (this adds it to your inventory)
*   **`f` / `p`**: Place a block from your active hotbar slot on the face of the block you're looking at
*   **`1` - `9`**: Select a hotbar slot
*   **`q`**: Quit the game

## Cleaning Up

To remove the compiled object file and executable, run:

```bash
make clean
```
