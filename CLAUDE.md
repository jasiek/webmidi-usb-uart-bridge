# Instructions for agents

## What is this project?

Software implementation running on Raspberry Pi Pico 1 (RP2040) which does the following:
- exposes itself as a USB MIDI device to the host computer, so that an iOS device will let us use it
- tunnels UART over the MIDI protocol at various speeds, and uses a command set to drive lines, flush, etc
- the hardware will allow us to attach a low-speed USB device which can be driven with the CDC/ACM class
- allow for bidirectional communication
- allow speeds of up to 115200 in this iteration
- needs to package binary data as 7-bit (SysEx)

## How to do it?
- Store the software project in the root directory, and the hardware description will reside in hardware/
- Use PlatformIO for managing the project, use https://www.raspberrypi.com/products/raspberry-pi-pico/ as the board.
- Use Pico-PIO-USB for the low-speed USB-UART device
- Create a test which attaches to a MIDI device and does loopback tests at various speeds
- If you come across something unexpected you've learned, put it in FINDINGS.md as a list.
- Ask the user questions and record them as decisions in DECISIONS.md
- Use subagents for research, writing commits, summaries, etc to maximize useful context window.
- Commit frequently, keep logically connected parts in a commit and only when the project builds (and test pass if there are tests).
