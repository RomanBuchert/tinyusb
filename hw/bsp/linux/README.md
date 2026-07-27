# Linux Raw Gadget BSP

## Overview

The Linux Raw Gadget BSP allows TinyUSB **device** examples to run
entirely in userspace on Linux by using the Linux Raw Gadget kernel
interface.

An optional **ncurses** based text user interface (TUI) provides virtual
buttons, LEDs, UART input/output and a TinyUSB log viewer. The TUI is
entirely optional and is disabled by default.

------------------------------------------------------------------------

## Requirements

### Linux Raw Gadget

A Linux kernel with **Raw Gadget** support is required.

Some distributions already provide the required kernel module. If your
distribution does not, a DKMS implementation is available:

-   https://github.com/HiFiPhile/raw-gadget-dkms

Thanks to HiFiPhile for maintaining the DKMS package.

### ncurses (optional)

The TUI requires the ncurses development package.

#### Debian / Ubuntu

``` bash
sudo apt install libncurses-dev
```

#### Arch Linux

``` bash
sudo pacman -S ncurses
```

------------------------------------------------------------------------

## Building

### Without the TUI

``` bash
cmake -B build
cmake --build build
```

### With the optional TUI

``` bash
cmake -B build \
    -DTINYUSB_RAW_GADGET_TUI=ON

cmake --build build
```

------------------------------------------------------------------------

## Running

Load the Raw Gadget kernel module:

``` bash
sudo modprobe raw_gadget
```

Then run the desired TinyUSB example.

Depending on the Linux distribution and Raw Gadget installation,
additional device creation or configuration may be required.

------------------------------------------------------------------------

## Text User Interface

The optional TUI provides:

-   Virtual button(s)
-   Virtual LED(s)
-   UART terminal
-   TinyUSB debug log viewer

The number of virtual buttons and LEDs can be configured through

``` c
RAW_GADGET_TUI_IO_COUNT
```

The default value is

``` c
1
```

which matches the requirements of the current TinyUSB examples.

------------------------------------------------------------------------

## Keyboard Shortcuts

  Key           Function
  ------------- --------------------------
  F1            Help page
  F2            Board page
  F3            Log page
  F10           Show / hide the TUI
  Tab           Switch focus
  Cursor keys   Select virtual button
  Space         Press selected button
  T             Toggle selected button
  Enter         Send UART line
  Page Up       Scroll log up
  Page Down     Scroll log down
  Home          Jump to newest log entry

------------------------------------------------------------------------

## Logging

TinyUSB debug output can be redirected into the integrated log viewer
using

``` c
CFG_TUSB_DEBUG_PRINTF
```

No application changes are required.

------------------------------------------------------------------------

## Configuration

  -------------------------------------------------------------------------------
  Option                      Default             Description
  --------------------------- ------------------- -------------------------------
  `TINYUSB_RAW_GADGET_TUI`    `OFF`               Enable the optional ncurses TUI

  `RAW_GADGET_TUI_IO_COUNT`   `1`                 Number of virtual buttons and
                                                  LEDs (1...32)
  -------------------------------------------------------------------------------

------------------------------------------------------------------------

## Limitations

The Linux BSP emulates GPIO-like interaction only.

It does **not** emulate USB electrical characteristics or timing beyond
what is provided by the Linux Raw Gadget interface.

------------------------------------------------------------------------

## License

This BSP is distributed under the same license as TinyUSB.
