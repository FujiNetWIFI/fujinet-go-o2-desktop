// mcs48_dasm_golden.cpp -- MAME's own MCS-48 disassembler, driven over every
// opcode, to produce core/tests/dasm_golden.h (see gen_dasm_golden.sh).
//
// Compiles MAME's src/devices/cpu/mcs48/mcs48dsm.cpp standalone: it only needs
// util::disasm_interface and util::stream_format, so a short `emu.h` shim
// stands in for the whole emulator. The I8048 variant (upi41 = i802x = false).
//
// SPDX-License-Identifier: BSD-3-Clause
// (drives MAME's mcs48dsm.cpp; copyright-holders Aaron Giles -- see COMPLIANCE.md)

#include "emu.h"
#include "mcs48dsm.h"

#include <cstdio>
#include <sstream>
#include <string>

namespace {

struct variant { unsigned pc, operand; };

// pc and operand byte: a plain page, the last byte of page 0 (the target page
// is the operand byte's), the end of the 2K bank, and above $800 (MB is not
// part of the printed address)
const variant variants[4] = { { 0x123, 0x5a }, { 0x0ff, 0x00 }, { 0x7fe, 0xff }, { 0x8a0, 0xc3 } };

struct bytes : util::disasm_interface::data_buffer
{
	unsigned base = 0;
	util::disasm_interface::u8 b[2] = { 0, 0 };
	util::disasm_interface::u8 r8(util::disasm_interface::offs_t pc) const override { return b[pc - base]; }
	util::disasm_interface::u16 r16(util::disasm_interface::offs_t) const override { return 0; }
	util::disasm_interface::u32 r32(util::disasm_interface::offs_t) const override { return 0; }
	util::disasm_interface::u64 r64(util::disasm_interface::offs_t) const override { return 0; }
};

} // namespace

int main()
{
	mcs48_disassembler dasm(false, false);

	std::printf("/* dasm_golden.h -- MAME's own disassembly of every I8048 opcode, from\n"
	            " * src/devices/cpu/mcs48/mcs48dsm.cpp compiled standalone (tools/ab/\n"
	            " * mcs48_dasm_golden.cpp, run by tools/ab/gen_dasm_golden.sh). GENERATED;\n"
	            " * dasm_test.c holds dasm48.c to it.\n"
	            " *\n"
	            " * Each opcode is disassembled at the four pc/operand pairs of\n"
	            " * dasm_variants; a one-byte opcode has one text, a two-byte opcode four.\n"
	            " * flags is MAME's result >> 28: 8 = SUPPORTED, 4 = STEP_OUT, 2 = STEP_OVER,\n"
	            " * 1 = STEP_COND. */\n");
	std::printf("static const struct { uint16_t pc; uint8_t operand; } dasm_variants[4] = {\n");
	for (const variant &v : variants)
		std::printf("    { 0x%03X, 0x%02X },\n", v.pc, v.operand);
	std::printf("};\n");
	std::printf("static const struct { uint8_t len; uint8_t flags; const char *text[4]; } dasm_golden[256] = {\n");

	for (unsigned op = 0; op < 256; op++)
	{
		std::string text[4];
		unsigned len = 0, flags = 0;
		for (int vi = 0; vi < 4; vi++)
		{
			bytes buf;
			buf.base = variants[vi].pc;
			buf.b[0] = op;
			buf.b[1] = variants[vi].operand;
			std::ostringstream os;
			util::disasm_interface::offs_t r = dasm.disassemble(os, variants[vi].pc, buf, buf);
			if ((r & util::disasm_interface::OVERINSTMASK) != 0)
			{
				std::fprintf(stderr, "opcode %02X: OVERINSTMASK set, not carried by the golden\n", op);
				return 1;
			}
			len = r & util::disasm_interface::LENGTHMASK;
			flags = (r & ~util::disasm_interface::LENGTHMASK) >> 28;
			text[vi] = os.str();
		}
		std::printf("    { %u, %u, { \"%s\"", len, flags, text[0].c_str());
		if (len > 1)
			for (int vi = 1; vi < 4; vi++)
				std::printf(", \"%s\"", text[vi].c_str());
		std::printf(" } },   /* %02X */\n", op);
	}
	std::printf("};\n");
	return 0;
}
