#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build a Ghidra-importable ARM ELF (Thumb) from the STM32F103C8T6 firmware HEX.

Inputs : 野草电调焦固件V1.001.hex  (Intel HEX, flash at 0x08000000)
Outputs: firmware.elf  - ELF32/EM_ARM/ET_EXEC, entry = Reset_Handler, .symtab
         firmware.bin  - raw flash image (Ghidra "Raw Binary" import at 0x08000000)
         vectors.tsv   - vector table (address, name, handler)
         seeds.tsv     - REA_GHIDRA_SEED_FILE seed list (function seeds)
         strings.txt   - printable strings with addresses
"""
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HEX = os.path.join(os.path.dirname(HERE), "野草电调焦固件V1.001.hex")
OUT_ELF = os.path.join(HERE, "firmware.elf")
OUT_BIN = os.path.join(HERE, "firmware.bin")
OUT_VECTORS = os.path.join(HERE, "vectors.tsv")
OUT_SEEDS = os.path.join(HERE, "seeds.tsv")
OUT_STRINGS = os.path.join(HERE, "strings.txt")

# ---------------------------------------------------------------- decode HEX
def decode_intel_hex(path):
    mem = {}
    base = 0
    start = None
    with open(path, "r", encoding="ascii") as handle:
        for lineno, raw in enumerate(handle, 1):
            line = raw.strip()
            if not line:
                continue
            if not line.startswith(":"):
                raise SystemExit("line %d is not a record" % lineno)
            b = bytes.fromhex(line[1:])
            count, addr, rtype = b[0], (b[1] << 8) | b[2], b[3]
            data = b[4:4 + count]
            if sum(b) & 0xFF:
                raise SystemExit("bad checksum on line %d" % lineno)
            if rtype == 0x00:
                for i, byte in enumerate(data):
                    mem[base + addr + i] = byte
            elif rtype == 0x01:
                pass
            elif rtype == 0x02:
                base = ((data[0] << 8) | data[1]) << 4
            elif rtype == 0x04:
                base = ((data[0] << 8) | data[1]) << 16
            elif rtype == 0x05:
                start = (((data[0] << 8) | data[1]) << 16) | ((data[2] << 8) | data[3])
    if not mem:
        raise SystemExit("no data records")
    return mem, start


mem, declared_start = decode_intel_hex(HEX)
lo, hi = min(mem), max(mem)
blob = bytes(mem[a] for a in range(lo, hi + 1))
print("flash 0x%08x-0x%08x (%d bytes) sha256=%s"
      % (lo, hi, len(blob), hashlib.sha256(blob).hexdigest()))

# ------------------------------------------------- vector table (STM32F103)
SYSTEM_NAMES = {
    1: "Reset_Handler", 2: "NMI_Handler", 3: "HardFault_Handler",
    4: "MemManage_Handler", 5: "BusFault_Handler", 6: "UsageFault_Handler",
    11: "SVC_Handler", 12: "DebugMon_Handler", 14: "PendSV_Handler",
    15: "SysTick_Handler",
}
# STM32F103C8 (medium density): IRQ0..IRQ42
IRQ_NAMES = [
    "WWDG", "PVD", "TAMPER", "RTC", "FLASH", "RCC", "EXTI0", "EXTI1", "EXTI2",
    "EXTI3", "EXTI4", "DMA1_Channel1", "DMA1_Channel2", "DMA1_Channel3",
    "DMA1_Channel4", "DMA1_Channel5", "DMA1_Channel6", "DMA1_Channel7",
    "ADC1_2", "USB_HP_CAN1_TX", "USB_LP_CAN1_RX0", "CAN1_RX1", "CAN1_SCE",
    "EXTI9_5", "TIM1_BRK", "TIM1_UP", "TIM1_TRG_COM", "TIM1_CC", "TIM2",
    "TIM3", "TIM4", "I2C1_EV", "I2C1_ER", "I2C2_EV", "I2C2_ER", "SPI1",
    "SPI2", "USART1", "USART2", "USART3", "EXTI15_10", "RTCAlarm",
    "USBWakeUp",
]
VECTOR_COUNT = 59
VECTOR_BYTES = VECTOR_COUNT * 4
vectors = []
for index in range(VECTOR_COUNT):
    off = index * 4
    word = int.from_bytes(blob[off:off + 4], "little")
    if index == 0:
        name = "_initial_sp"
    elif index in SYSTEM_NAMES:
        name = SYSTEM_NAMES[index]
    elif index >= 16:
        irq = index - 16
        name = "%s_IRQHandler" % IRQ_NAMES[irq] if irq < len(IRQ_NAMES) else "IRQ%d" % irq
    else:
        name = "Reserved_%d" % index
    vectors.append((index, name, word, 0x08000000 + off))

sp = vectors[0][2]
reset = vectors[1][2]
code_start = 0x08000000 + VECTOR_BYTES
print("initial SP = 0x%08x | Reset = 0x%08x (%s) | code starts 0x%08x"
      % (sp, reset, "Thumb" if reset & 1 else "ARM", code_start))

# ------------------------------------------------------- write raw + tables
with open(OUT_BIN, "wb") as handle:
    handle.write(blob)

with open(OUT_VECTORS, "w", encoding="utf-8") as handle:
    handle.write("# address\tname\tvector\tmode\n")
    for index, name, word, addr in vectors:
        mode = "-" if index == 0 else ("null" if word == 0 else ("thumb" if word & 1 else "arm"))
        handle.write("0x%08x\t%s\t0x%08x\t%s\n" % (addr, name, word, mode))

default_handler = None
for index, name, word, addr in vectors:
    if index >= 16 and word:
        default_handler = word
seeds = []
seen = set()
for index, name, word, addr in vectors:
    if index == 0 or not word:
        continue
    target = word & ~1
    if target in seen:
        continue
    seen.add(target)
    seeds.append((target, name))
with open(OUT_SEEDS, "w", encoding="utf-8") as handle:
    handle.write("# REA_GHIDRA_SEED_FILE format: address<TAB>function[<TAB>name]\n")
    for target, name in seeds:
        handle.write("0x%08x\tfunction\t%s\n" % (target, name))

strings = []
i = 0
while i < len(blob):
    j = i
    while j < len(blob) and 0x20 <= blob[j] < 0x7F:
        j += 1
    if j - i >= 5:
        strings.append((lo + i, blob[i:j].decode("ascii")))
    i = j if j > i else i + 1
with open(OUT_STRINGS, "w", encoding="utf-8") as handle:
    for addr, text in strings:
        handle.write("0x%08x  %s\n" % (addr, text))

# --------------------------------------------------------------- ELF writer
SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB = 1, 2, 3
SHF_ALLOC, SHF_EXECINSTR = 0x2, 0x4
STB_GLOBAL, STT_FUNC, STT_OBJECT = 1, 2, 1

text_addr = code_start
text_off = 0x1000 + VECTOR_BYTES
text_size = len(blob) - VECTOR_BYTES

strtab = bytearray(b"\0")
def add_str(name):
    global strtab
    off = len(strtab)
    strtab.extend(name.encode("utf-8") + b"\0")
    return off

sym_entries = [b"\0" * 16]
names = []
for target, name in seeds:
    st_name = add_str(name)
    sym_entries.append(struct.pack("<IIIBBH", st_name, target, 0,
                                   (STB_GLOBAL << 4) | STT_FUNC, 0, 2))
    names.append(name)
vt_name_off = add_str("vector_table")
sym_entries.append(struct.pack("<IIIBBH", vt_name_off, 0x08000000, VECTOR_BYTES,
                               (STB_GLOBAL << 4) | STT_OBJECT, 0, 1))

symtab = b"".join(sym_entries)
shstrtab = bytearray(b"\0")
def add_shstr(name):
    global shstrtab
    off = len(shstrtab)
    shstrtab.extend(name.encode("utf-8") + b"\0")
    return off

sh_names = {
    ".vectors": add_shstr(".vectors"),
    ".text": add_shstr(".text"),
    ".symtab": add_shstr(".symtab"),
    ".strtab": add_shstr(".strtab"),
    ".shstrtab": add_shstr(".shstrtab"),
}

symtab_off = text_off + text_size
strtab_off = symtab_off + len(symtab)
shstrtab_off = strtab_off + len(strtab)
shoff = shstrtab_off + len(shstrtab)
shoff += (4 - shoff % 4) % 4

ehsize, phentsize, shentsize = 52, 32, 40
ehdr = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
ehdr += struct.pack("<HHIIIIIHHHHHH", 2, 40, 1, reset & ~1, ehsize, shoff,
                    0x05000000, ehsize, phentsize, 1, shentsize, 6, 5)
phdr = struct.pack("<IIIIIIII", 1, 0x1000, lo, lo, len(blob), len(blob), 5, 0x1000)

shdrs = [b"\0" * 40]
shdrs.append(struct.pack("<IIIIIIIIII", sh_names[".vectors"], SHT_PROGBITS, SHF_ALLOC,
                          0x08000000, 0x1000, VECTOR_BYTES, 0, 0, 4, 0))
shdrs.append(struct.pack("<IIIIIIIIII", sh_names[".text"], SHT_PROGBITS,
                          SHF_ALLOC | SHF_EXECINSTR, text_addr, text_off, text_size,
                          0, 0, 4, 0))
shdrs.append(struct.pack("<IIIIIIIIII", sh_names[".symtab"], SHT_SYMTAB, 0, 0,
                          symtab_off, len(symtab), 4, 1, 4, 16))
shdrs.append(struct.pack("<IIIIIIIIII", sh_names[".strtab"], SHT_STRTAB, 0, 0,
                          strtab_off, len(strtab), 0, 0, 1, 0))
shdrs.append(struct.pack("<IIIIIIIIII", sh_names[".shstrtab"], SHT_STRTAB, 0, 0,
                          shstrtab_off, len(shstrtab), 0, 0, 1, 0))

elf = bytearray()
elf += ehdr + phdr
elf += b"\0" * (0x1000 - len(ehdr) - len(phdr))
elf += blob
elf += symtab
elf += bytes(strtab)
elf += bytes(shstrtab)
elf += b"\0" * ((4 - len(elf) % 4) % 4)
elf += b"".join(shdrs)
assert len(elf) == shoff + len(shdrs) * shentsize, (len(elf), shoff)

with open(OUT_ELF, "wb") as handle:
    handle.write(elf)

print("wrote %s (%d bytes, entry 0x%08x, %d symbols)"
      % (OUT_ELF, len(elf), reset & ~1, len(sym_entries) - 1))
print("wrote %s, %s, %s, %s" % (OUT_BIN, OUT_VECTORS, OUT_SEEDS, OUT_STRINGS))
