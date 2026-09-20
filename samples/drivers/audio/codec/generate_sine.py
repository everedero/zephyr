#!/usr/bin/env python3
"""
Generate a 440 Hz sine wave lookup table sampled at 48 kHz
Output format: uint8_t array for use in Zephyr DAC samples
The data is stored as 16-bit little-endian samples (2 bytes per sample)
"""

import math
import sys

# Parameters
FREQUENCY = 440  # Hz (A4 note)
SAMPLE_RATE = 48000  # Hz
BIT_DEPTH = 16  # bits (will be converted to 12-bit by shifting >> 4)

# Calculate the number of samples for one complete period
# We want an exact number of samples to make a seamless loop
samples_per_period = SAMPLE_RATE / FREQUENCY
# Round to nearest integer for exact period
samples_per_period = round(samples_per_period)

print(f"// Generating {FREQUENCY} Hz sine wave at {SAMPLE_RATE} Hz sample rate")
print(f"// Samples per period: {samples_per_period}")
print(f"// Actual frequency: {SAMPLE_RATE / samples_per_period:.2f} Hz")
print()

# Generate the sine wave
wave_table = []
max_value = (1 << BIT_DEPTH) - 1  # 0xFFFF for 16-bit
mid_value = max_value // 2  # Center point (32768)

for i in range(samples_per_period):
    # Generate sine wave from -1 to +1
    phase = 2.0 * math.pi * i / samples_per_period
    sine_value = math.sin(phase)
    
    # Scale to 16-bit unsigned range (0 to 65535)
    # Center at 32768 (0x8000)
    sample_value = int(mid_value + (sine_value * mid_value))
    
    # Clamp to valid range
    sample_value = max(0, min(max_value, sample_value))
    
    # Split into low and high bytes (little-endian)
    low_byte = sample_value & 0xFF
    high_byte = (sample_value >> 8) & 0xFF
    
    wave_table.append(low_byte)
    wave_table.append(high_byte)

# Generate the C header file content
print("/*")
print(" * SPDX-License-Identifier: Apache-2.0")
print(" *")
print(f" * 440 Hz sine wave sampled at 48 kHz")
print(f" * {samples_per_period} samples per period, {len(wave_table)} bytes total")
print(" */")
print()
print("#ifndef SINE_H_")
print("#define SINE_H_")
print()
print("/* PCM data is word-aligned for efficient access (e.g. by the codec/DMA).")
print(" */")
print(f"static uint8_t __aligned(4) wave_table[] = {{")

# Format output: 15 bytes per line for readability
bytes_per_line = 15
for i in range(0, len(wave_table), bytes_per_line):
    line_bytes = wave_table[i:i + bytes_per_line]
    hex_values = ", ".join(f"0x{b:02X}" for b in line_bytes)
    if i + bytes_per_line < len(wave_table):
        print(f"\t{hex_values},")
    else:
        print(f"\t{hex_values}")

print("};")
print()
print("#endif /* SINE_H_ */")
