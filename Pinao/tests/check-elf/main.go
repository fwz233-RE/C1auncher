// Host-side ABI verification; no WSL or target execution required.
package main

import (
	"debug/elf"
	"fmt"
	"os"
)

func verify(path string) error {
	f, err := elf.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	if f.Class != elf.ELFCLASS32 || f.Data != elf.ELFDATA2LSB || f.Machine != elf.EM_MIPS || f.Type != elf.ET_EXEC {
		return fmt.Errorf("expected little-endian ELF32 MIPS executable")
	}
	for _, p := range f.Progs {
		if p.Type == elf.PT_INTERP || p.Type == elf.PT_DYNAMIC {
			return fmt.Errorf("expected static executable without dynamic interpreter")
		}
	}
	s := f.Section(".MIPS.abiflags")
	if s == nil {
		return fmt.Errorf("missing MIPS ABI flags")
	}
	data, err := s.Data()
	if err != nil {
		return err
	}
	// Elf_MIPS_ABIFlags_v0: version, ISA level/revision, register widths,
	// FP ABI. Val_GNU_MIPS_ABI_FP_DOUBLE is 1 (hard-float double precision).
	if len(data) != 24 || f.ByteOrder.Uint16(data[:2]) != 0 || data[2] != 32 || (data[3] != 1 && data[3] != 2) || data[4] != 1 || data[7] != 1 {
		return fmt.Errorf("expected MIPS32r1/r2, 32-bit GPRs, hard-float double ABI; flags=%x", data)
	}
	// ELF32 e_flags at byte 36: EF_MIPS_ABI_O32=0x1000, ABI mask=0xf000.
	file, err := os.Open(path)
	if err != nil {
		return err
	}
	defer file.Close()
	var flags [4]byte
	if _, err := file.ReadAt(flags[:], 36); err != nil {
		return err
	}
	if f.ByteOrder.Uint32(flags[:])&0xf000 != 0x1000 {
		return fmt.Errorf("expected o32 ABI")
	}
	return nil
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: check-elf <pinao>")
		os.Exit(2)
	}
	if err := verify(os.Args[1]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Println("Verified static ELF32 little-endian MIPS32r1/r2 o32 hard-float double ABI")
}
