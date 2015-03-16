/**
 * Copyright (C) 2013 Tomi Valkeinen
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301, USA.
 */

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include "rwmem.h"

static void print_field(const struct reg_desc *reg, const struct field_desc *fd,
		uint64_t value)
{
	uint64_t fv = (value & fd->mask) >> fd->shift;

	if (fd->name)
		printf("\t%-*s ", reg->max_field_name_len, fd->name);
	else
		printf("\t");

	if (fd->width == 1)
		printf("   %-2d = ", fd->shift);
	else
		printf("%2d:%-2d = ",
				fd->shift + fd->width - 1, fd->shift);

	printf("%-#*" PRIx64, reg->width / 4 + 2, fv);

	if (rwmem_opts.show_defval)
		printf(" (%0#" PRIx64 ")", fd->defval);

	if (rwmem_opts.show_comments && fd->comment)
		printf(" # %s", fd->comment);

	puts("");
}

static void readwriteprint(const struct addr *addr,
		const struct reg_desc *reg,
		const struct field_desc *field,
		uint64_t userval,
		bool write, bool write_only)
{
	if (reg->name)
		printf("%s ", reg->name);

	printf("%#" PRIx64 " ", addr->paddr);
	if (reg->address != addr->paddr)
		printf("(+%#" PRIx64 ") ", reg->address);

	uint64_t oldval = 0, newval = 0;

	if (!write_only) {
		oldval = readmem(addr->vaddr, reg->width);

		printf("= %0#*" PRIx64 " ", reg->width / 4 + 2, oldval);

		newval = oldval;
	}

	if (write) {
		uint64_t v;

		if (field) {
			v = oldval;
			v &= ~field->mask;
			v |= userval << field->shift;
		} else {
			v = userval;
		}

		printf(":= %0#*" PRIx64 " ", reg->width / 4 + 2, v);

		fflush(stdout);

		writemem(addr->vaddr, reg->width, v);

		newval = v;
	}

	if (write && !write_only) {
		newval = readmem(addr->vaddr, reg->width);

		printf("-> %0#*" PRIx64 " ", reg->width / 4 + 2, newval);
	}

	if (rwmem_opts.show_comments && reg->comment)
		printf(" # %s", reg->comment);

	printf("\n");

	if (field) {
		print_field(reg, field, newval);
	} else {
		for (unsigned i = 0; i < reg->num_fields; ++i) {
			const struct field_desc *fd = &reg->fields[i];

			print_field(reg, fd, newval);
		}
	}
}

static void parse_op(const struct rwmem_opts_arg *arg, struct rwmem_op *op,
	const char *regfile)
{
	/* Parse address */

	struct reg_desc *reg = parse_address(arg->address, regfile);
	op->reg = reg;

	/* Parse range */

	uint64_t range;

	if (arg->range) {
		char *endptr;

		range = strtoull(arg->range, &endptr, 0);
		if (*endptr != 0)
			myerr("Invalid range '%s'", arg->range);

		if (!arg->range_is_offset)
			range = range - reg->address;
	} else {
		range = reg->width / 8;
	}

	op->range = range;

	/* Parse field */

	if (arg->field) {
		struct field_desc *field = parse_field(arg->field, reg);

		if (field) {
			if (field->shift >= rwmem_opts.regsize ||
				(field->width + field->shift) > rwmem_opts.regsize)
			myerr("Field bits higher than register size");
		}

		op->field = field;
	}

	/* Parse value */

	if (arg->value) {
		uint64_t userval = parse_value(arg->value);

		if (userval >= (1ULL << rwmem_opts.regsize))
			myerr("Value does not fit into the register size");

		if (op->field && (userval & (~op->field->mask >> op->field->shift)))
			myerr("Value does not fit into the field");

		op->value = userval;
		op->write = true;
	}
}

static void do_op(int fd, uint64_t base, const struct rwmem_op *op)
{
	const unsigned pagesize = sysconf(_SC_PAGESIZE);
	const unsigned pagemask = pagesize - 1;

	uint64_t paddr = base + op->reg->address;
	off_t pa_offset = paddr & ~pagemask;
	size_t len = op->range + paddr - pa_offset;

	/*
	printf("range %#" PRIx64 " paddr %#" PRIx64 " pa_offset 0x%lx, len 0x%zx\n",
		op->range, paddr, pa_offset, len);
	*/

	void *mmap_base = mmap(0, len,
			op->write ? PROT_WRITE : PROT_READ,
			MAP_SHARED, fd, pa_offset);

	if (mmap_base == MAP_FAILED)
		myerr2("failed to mmap");

	void *vaddr = (uint8_t* )mmap_base + (paddr & pagemask);

	uint64_t end_paddr = paddr + op->range;

	/* HACK */
	struct reg_desc reg = *op->reg;

	while (paddr < end_paddr) {

		const struct addr addr = {
			.paddr = paddr,
			.vaddr = vaddr,
		};

		readwriteprint(&addr, &reg, op->field, op->value,
			op->write, rwmem_opts.write_only);

		paddr += reg.width / 8;
		vaddr += reg.width / 8;

		/* HACK */
		struct reg_desc new_reg = {
			.address = reg.address + reg.width / 8,
			.width = reg.width
		};

		reg = new_reg;
	}

	if (munmap(mmap_base, pagesize) == -1)
		myerr2("failed to munmap");
}

int main(int argc, char **argv)
{
	uint64_t base;
	const char *regfile;

	parse_cmdline(argc, argv);

	/* Parse base */

	base = 0;
	regfile = NULL;

	if (rwmem_opts.base)
		parse_base(rwmem_opts.aliasfile, rwmem_opts.base, &base, &regfile);

	if (rwmem_opts.regfile)
		regfile = rwmem_opts.regfile;

	int num_ops = rwmem_opts.num_args;

	bool read_only = true;

	struct rwmem_op *ops = malloc(sizeof(struct rwmem_op) * num_ops);
	memset(ops, 0, sizeof(struct rwmem_op) * num_ops);
	for (int i = 0; i < num_ops; ++i) {
		const struct rwmem_opts_arg *arg = &rwmem_opts.args[i];
		struct rwmem_op *op = &ops[i];

		parse_op(arg, op, regfile);

		if (op->write)
			read_only = false;
	}

	/* Open the file and mmap */

	int fd = open(rwmem_opts.filename,
			(read_only ? O_RDONLY : O_RDWR) | O_SYNC);

	if (fd == -1)
		myerr2("Failed to open file '%s'", rwmem_opts.filename);

	for (int i = 0; i < num_ops; ++i) {
		struct rwmem_op *op = &ops[i];

		do_op(fd, base, op);
	}

	close(fd);

	return 0;
}
