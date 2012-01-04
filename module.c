#include <inc/stdio.h>
#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/memlayout.h>
#include <inc/elf.h>
#include <kern/pmap.h>
#include <kern/sched.h>
#include <kern/env.h>
#include <kern/module.h>
#include <kern/symboltable.h>

// Global module structure
struct Module *modules;
uint32_t module_count;

// Pointer to the actual module data
void *module_data;

// Global module bitmap. Indicates which module slots are free / used
uint16_t module_bitmap;

// To keep the compiler happy!
extern uint32_t get_symbol_addr (char *symbol_name);
extern int insert_symbol (char *symbolName, uint32_t addr);

// Insert the given module
int
module_init (char *mod_name, void *mod_binary, uint32_t mod_binary_size)
{
    struct Module *module;
    struct Elf *elf_hdr = (struct Elf *)mod_binary;
    struct Secthdr *sh, *end_sh, *section_base;
    struct Secthdr *rel_sh, *sym_sh, *str_sh, *shstr_sh;
    uint8_t *module_base, *sh_string_table, *start_addr;
    uint32_t sh_size = 0, sh_count = 0, i = 0, j = 0, k = 0;
    uint32_t text_start = 0, rodata_addr = 0, bss_addr = 0, data_addr = 0;
    uint32_t common_block_addr = 0; // For ELF_SHN_COMMON case
    int ret, module_index, rel_offset, next_addr, sym_addr;

    // First check if this is a valid ELF. bail if its not.
    if (elf_hdr->e_magic != ELF_MAGIC) {
	return -E_INVAL;
    }

    // Initialization
    rel_sh = sym_sh = str_sh = shstr_sh = NULL;
    module_index = INVALID_MODULE_INDEX;

    // First make sure we haven't already loaded this module!
    for (i = 0; i < MAX_MODULES; i++) {
	module = (struct Module *)(MODULES + i * MAX_MD_SIZE);
	if (module->module_state == MODULE_STATE_ACTIVE &&
	    strcmp(module->module_name, mod_name) == 0) {
	    cprintf("module_init: module %s is already loaded\n", mod_name);
	    return -E_FILE_EXISTS;
	}
    }

    // Find the first free slot for this module
    for (i = 0; i < MAX_MODULES; i++) {
	if ((module_bitmap & (1 << i)) == 0) {
	    module_index = i;
	    module_bitmap |= (1 << i);
	    break;
	}
    }

    // Were we able to find one?
    if (module_index == INVALID_MODULE_INDEX) {
	// We ran out of module space. Bail.
	cprintf("module_init: can't load module %s due to lack of space\n",
		mod_name);
	return -E_NO_MEM;
    }

    // Get the descriptor for this module;
    module = (struct Module *)(MODULES + module_index * sizeof(struct Module));

    // Get a pointer to the location where this module will be actually loaded
    module_base = (uint8_t *)MODULE_DATA + module_index * PGSIZE;

    // Record the start of section headers
    section_base = (struct Secthdr *)((uint8_t *)elf_hdr + elf_hdr->e_shoff);

    // Record the location of section header string table
    shstr_sh = (struct Secthdr *)(&section_base[elf_hdr->e_shstrndx]);
    sh_string_table = (uint8_t *)elf_hdr + shstr_sh->sh_offset;

    // Do the basic initialization of the module
    strcpy(module->module_name, mod_name);
    module->module_state = MODULE_STATE_INIT;
    module->module_index = module_index;
    module->module_base = (uint32_t)module_base;

    // Walk through all the sections and load it to memory
    sh = (struct Secthdr *)((uint8_t *)elf_hdr + elf_hdr->e_shoff);
    end_sh= sh + elf_hdr->e_shnum;

    for (; sh < end_sh; sh++) {

	// Load only if required
	if (sh->sh_flags & SHF_ALLOC && sh->sh_size != 0) {
	    if (sh->sh_addr == 0) {
		sh->sh_addr = (uint32_t)module_base + sh_size;

		if (sh->sh_type == ELF_SHT_NOBITS) {
		    memset((void *)sh->sh_addr, 0, sh->sh_size);
		} else {
		    memmove((void *)sh->sh_addr, 
			    (uint8_t *)mod_binary + sh->sh_offset,
			    sh->sh_size);
		}

		sh_size += sh->sh_size;

		// Update bookkeeping info
		module->sections[sh_count].sh_start = sh->sh_addr;
		module->sections[sh_count].sh_size = sh->sh_size;
		module->sections[sh_count].sh_type = sh->sh_type;
		module->sections[sh_count].sh_offset = sh->sh_name;
		sh_count++;
	    }
	}

	//
	// See if this is the symbol table, string table or relocatable
	// section and store the pointer. This will be used in the second
	// pass.
	//
	if (sh->sh_type == ELF_SHT_REL) {
	    rel_sh = sh;
	} else if (sh->sh_type == ELF_SHT_SYMTAB) {
	    sym_sh = sh;
	} else if (sh->sh_type == ELF_SHT_STRTAB) {
	    str_sh = sh;
	}
    }

    // 
    // Make a second pass over the section headers and update the
    // section names in our module structure
    //
    for (i = 0; i < sh_count; i++) {
	sh = (struct Secthdr *)((uint8_t *)elf_hdr + elf_hdr->e_shoff);
	end_sh= sh + elf_hdr->e_shnum;

	for (; sh < end_sh; sh++) {
	    if (sh->sh_name == module->sections[i].sh_offset) {
		// Get the name from the string table
		j = 0;
		start_addr = sh_string_table + module->sections[i].sh_offset;
		while (start_addr[j] != 0) {
		    module->sections[i].sh_name[j] = start_addr[j];
		    j++;
		}
		module->sections[i].sh_name[j] = 0;
	    }
	}
    }

    //
    // Now walk through the .rel.txt section and update the .text section
    // with proper addresses. First get a pointer to the .text, .rodata,
    // .bss and .data sections
    //
    for (i = 0; i < sh_count; i++) {
	if (strcmp(module->sections[i].sh_name, ".text") == 0) {
	    text_start = module->sections[i].sh_start;
	} else if (strcmp(module->sections[i].sh_name, ".rodata") == 0) {
	    rodata_addr = module->sections[i].sh_start;
	} else if (strcmp(module->sections[i].sh_name, ".bss") == 0) {
	    bss_addr = module->sections[i].sh_start;
	} else if (strcmp(module->sections[i].sh_name, ".data") == 0) {
	    data_addr = module->sections[i].sh_start;
	}
    }

    //
    // Make a note of the module size so far. This will be needed later on
    // to allocate memory for symols defined as SHN_COMMON
    //
    common_block_addr = text_start + sh_size;

    //
    // Walk through the symbols and store them. Look for the address
    // of init_module and cleanup_module and store it as the entry and
    // cleanup points. This is where we find out what all functions are
    // exposed by the module and copy them to the symbol table.
    //

    uint8_t *sym_section = (uint8_t *)elf_hdr + sym_sh->sh_offset;
    uint8_t *string_table = (uint8_t *)elf_hdr + str_sh->sh_offset;
    struct Symbol *symbols = (struct Symbol *)sym_section;
    int num_sym = sym_sh->sh_size / sizeof(symbols[0]);

    for (i = 0; i < num_sym; i++) {
	start_addr = string_table + symbols[i].sym_name;
	j =0;
	while (start_addr[j] != 0) {
	    module->sym_table[i].sym_name[j] = start_addr[j];
	    j++;
	}
	module->sym_table[i].sym_name[j] = 0;
	module->sym_table[i].sym_value = symbols[i].sym_value;

	if (ELF32_ST_TYPE(symbols[i].sym_info) == STT_FUNC) {
	    //
	    // This is a symbol exposed by the module and has to go in the
	    // symbol table
	    //
	    insert_symbol(module->sym_table[i].sym_name,
			  (uint32_t)((uint8_t *)text_start + symbols[i].sym_value));
	}

	if (ELF32_ST_TYPE(symbols[i].sym_info) == STT_OBJECT) {
	    //
	    // This is a symbol exposed by the module and has to go in the
	    // symbol table. First figure out the section it has to go in.
	    //
	    int sym_shndx = symbols[i].sym_shndx;
	    int section = -1;
	    struct Secthdr *s;

	    // ELF_SHN_COMMON needs to be handled differently
	    if (sym_shndx != ELF_SHN_COMMON) {
		s = &section_base[sym_shndx];
		
		for (k = 0; k < sh_count; k++) {
		    if (s->sh_addr == module->sections[k].sh_start) {
			if (strcmp(module->sections[k].sh_name, ".bss") == 0) {
			    section = SECTION_BSS;
			} else if (strcmp(module->sections[k].sh_name, ".data") == 0) {
			    section = SECTION_DATA;
			}
			break;
		    }
		}

		switch (section) {
		    case SECTION_BSS:
			insert_symbol(module->sym_table[i].sym_name,
				      (uint32_t)((uint8_t *)bss_addr + symbols[i].sym_value));
			break;

		    case SECTION_DATA:
			insert_symbol(module->sym_table[i].sym_name,
				      (uint32_t)((uint8_t *)data_addr + symbols[i].sym_value));
			break;
		}
	    } else {
		// 
		// Use the common block for this symbol and update pointer
		// accordingly. Align the address first as indicated in the
		// symbol.
		//
		uint32_t align = common_block_addr % symbols[i].sym_value;
		common_block_addr = common_block_addr - align + symbols[i].sym_value;

		insert_symbol(module->sym_table[i].sym_name,
			      (uint32_t)((uint8_t *)common_block_addr));
		common_block_addr += symbols[i].sym_size;
	    }
	}

	if (strcmp(module->sym_table[i].sym_name, "init_module") == 0) {
	    // Update the module entry point
	    module->init_routine = 
		(void *)((uint8_t *)text_start + symbols[i].sym_value);
	} else if (strcmp(module->sym_table[i].sym_name, "cleanup_module") == 0) {
	    // Update the module exit point
	    module->cleanup_routine = 
		(void *)((uint8_t *)text_start + symbols[i].sym_value);
	}
    }

    //
    // Start the actual relocation process. Walk through all the entries in
    // the .rel.txt section and apply relocation accordingly.
    //

    uint8_t *rel_section = (uint8_t *)elf_hdr + rel_sh->sh_offset;
    struct Rel *rels = (struct Rel *)rel_section;
    int num_rels = rel_sh->sh_size / sizeof(rels[0]);
    int section = 0, var_size = 0;

    for (i = 0; i < num_rels; i++) {
	// First construct the relocatable entry
	struct Symbol *sym = &symbols[ELF32_R_SYM(rels[i].rel_info)];

	start_addr = string_table + sym->sym_name;
	j =0;
	while (start_addr[j] != 0) {
	    module->rel_entry[i].sym_name[j] = start_addr[j];
	    j++;
	}
	module->rel_entry[i].sym_name[j] = 0;

	module->rel_entry[i].rel_type = ELF32_R_TYPE(rels[i].rel_info);
	module->rel_entry[i].rel_offset = rels[i].rel_offset;

	// Now apply the relocation based on the symbol type
	switch (module->rel_entry[i].rel_type) {
	    case R_386_32:
		//
		// This signifies absolute addressing. Here, the relocation
		// may have to be done either on the .rodata / .bss / .data /
		// .text section. This information can be obtained from the
		// shndx parameter of the symbol.
		//
		if (module->rel_entry[i].sym_name[0] == '\0') {
		    // .rodata / .bss / .data. Figure out which.
		    struct Secthdr *r386_32_sh = &section_base[sym->sym_shndx];
		    for (k = 0; k < sh_count; k++) {
			if (r386_32_sh->sh_addr == module->sections[k].sh_start) {
			    if (strcmp(module->sections[k].sh_name, ".rodata") == 0) {
				section = SECTION_RODATA;
			    } else if (strcmp(module->sections[k].sh_name, ".bss") == 0) {
				section = SECTION_BSS;
			    } else if (strcmp(module->sections[k].sh_name, ".data") == 0) {
				section = SECTION_DATA;
			    }
			}
		    }

		    switch (section) {
			case SECTION_RODATA:
			    rel_offset = module->rel_entry[i].rel_offset;
			    *(uint32_t *)((uint8_t *)text_start + rel_offset) = rodata_addr;

			    // Fix the rodata pointer to point to the next string
			    j = 0;
			    while (*((uint8_t *)rodata_addr + j) != '\0') {
				j++;
			    }
			    rodata_addr += (j + 1);
			    break;

			case SECTION_BSS:
			    rel_offset = module->rel_entry[i].rel_offset;
			    *(uint32_t *)((uint8_t *)text_start + rel_offset) = bss_addr;
			    break;

			case SECTION_DATA:
			    rel_offset = module->rel_entry[i].rel_offset;
			    *(uint32_t *)((uint8_t *)text_start + rel_offset) = data_addr;
			    data_addr += sym->sym_size;
			    break;
		    }
		} else {
		    // .text section
		    rel_offset = module->rel_entry[i].rel_offset;
		    sym_addr = get_symbol_addr(module->rel_entry[i].sym_name);
		    *(uint32_t *)((uint8_t *)text_start + rel_offset) = sym_addr;
		}
		break;

	    case R_386_PC32:
		// PC relative addressing
		rel_offset = module->rel_entry[i].rel_offset;
		sym_addr = get_symbol_addr(module->rel_entry[i].sym_name);
		next_addr = text_start + rel_offset + 4;
		*(uint32_t *)((uint8_t *)text_start + rel_offset) = sym_addr - next_addr;
		break;
	}
    }

    // Bookkeeping info
    module->module_size = sh_size;
    module->module_sh_count = sh_count;
    module->module_rel_count = num_rels;
    module->module_sym_count = num_sym;

    // Update the module state
    module->module_state = MODULE_STATE_ACTIVE;

    // Update the count as well
    module_count++;

    // All done.. Invoke the module
    module->init_routine(module->module_index);

    // For debugging
    if (module->module_vectors.test_api_vector) {
	module->module_vectors.test_api_vector();
    }

    return 0;
}

// Delete the given module
int
module_cleanup (char *mod_name)
{
    int i, module_found = 0;
    struct Module *module = (struct Module *)MODULES;
    struct Module *rmmod = NULL;

    // Get a pointer to this module
    for (i = 0; i < MAX_MODULES; i++) {
	if (strcmp(module[i].module_name, mod_name) == 0) {
	    rmmod = &module[i];
	    module_found = 1;
	}
    }

    if (!module_found) {
	cprintf("module_cleanup: cannot find module %s\n", mod_name);
	return -E_NOT_FOUND;
    }

    // Update the bitmap
    module_bitmap &= ~(1 << rmmod->module_index);

    // Call the module's cleanup routine
    rmmod->cleanup_routine(rmmod->module_index);

    // Zero out the corresponding module data section
    memset((void *)rmmod->module_base, 0, PGSIZE);

    // Zero out the module descriptor
    memset((void *)rmmod, 0, PGSIZE);

    // Update the module count
    module_count--;

    return 0;
}

// Routine to display the list of modules currently loaded
int
module_display (void)
{
    struct Module *module = (struct Module *)MODULES;
    int i;

    if (module_count == 0) {
	cprintf("\nThere are no modules loaded in kernel\n");
	return 0;
    }

    cprintf("\nTotal number of modules: %d\n\n", module_count);

    for (i = 0; i < MAX_MODULES; i++) {
	if (module[i].module_state == MODULE_STATE_ACTIVE) {
	    cprintf("Name \t\t : %s\n", module[i].module_name);
	    cprintf("Index \t\t : %d\n", module[i].module_index);
	    cprintf("Base Address \t : 0x%x\n", module[i].module_base);
	    cprintf("Size \t\t : %d bytes\n", module[i].module_size);
	    cprintf("Module Entry \t : 0x%x\n", module[i].init_routine);
	    cprintf("Module Exit \t : 0x%x\n", module[i].cleanup_routine);
	    cprintf("No of Sections \t : %d\n", module[i].module_sh_count);
	    cprintf("No of Relocations     : %d\n", module[i].module_rel_count);
	    cprintf("No of Symbols \t : %d\n", module[i].module_sym_count);
	    cprintf("\n");
	}
    }

    return 0;
}

// Routine to invoke the insmod environment
int
module_invoke_insmod (void)
{
    //
    // Create the insmod environment and yield. It will eventually get
    // scheduled to run.
    //
    ENV_CREATE(user_insmod);

    sched_yield();
}

// Routine to invoke the rmmod environment
int
module_invoke_rmmod (void)
{
    //
    // Create the insmod environment and yield. It will eventually get
    // scheduled to run.
    //
    ENV_CREATE(user_rmmod);

    sched_yield();
}

// Routine to invoke test environment which generates syscalls
int
module_invoke_test_syscall (void)
{
    ENV_CREATE(user_test_syscall);

    sched_yield();
}

// Registration functions exposed to the individual modules
void
module_register (uint32_t mod_index, int type, void *vector)
{
    struct Module *modules = (struct Module *)MODULES;

    cprintf("module_register: module %d type %d vector 0x%x\n", 
	    mod_index, type, vector);

    if (type == MODULE_SHOW_SYSCALL) {
	modules[mod_index].module_vectors.show_syscall_vector = vector;
    } else if (type == MODULE_COUNT_SYSCALL) {
	modules[mod_index].module_vectors.count_syscall_vector = vector;
    } else if (type == MODULE_SHOW_TIME) {
	modules[mod_index].module_vectors.show_time_vector = vector;
    } else if (type == MODULE_TEST_API) {
	modules[mod_index].module_vectors.test_api_vector = vector;
    }
}

// Debug routines to test the syscall module

void
module_invoke_count_syscall (void)
{
    struct Module *modules = (struct Module *)MODULES;
    int module_index = -1, i = 0;

    // Find the syscall module
    for (i = 0; i < MAX_MODULES; i++) {
	if (strcmp(modules[i].module_name, "testmod_syscall.o") == 0) {
	    module_index = modules[i].module_index;
	    break;
	}
    }

    if (module_index == -1) {
	// Module not present. Nothing much we can do.
	return;
    }

    // Invoke the module
    if (modules[i].module_vectors.count_syscall_vector) {
	modules[i].module_vectors.count_syscall_vector();
    }
}

void
module_invoke_show_syscall (void)
{
    struct Module *modules = (struct Module *)MODULES;
    int module_index = -1, i = 0;

    // Find the syscall module
    for (i = 0; i < MAX_MODULES; i++) {
	if (strcmp(modules[i].module_name, "testmod_syscall.o") == 0) {
	    module_index = modules[i].module_index;
	    break;
	}
    }

    if (module_index == -1) {
	// Module not present. Nothing much we can do.
	return;
    }

    // Invoke the module
    if (modules[i].module_vectors.show_syscall_vector) {
	modules[i].module_vectors.show_syscall_vector();
    }
}

// Debug routines to test the show_time module

void
module_invoke_show_time (void)
{
    struct Module *modules = (struct Module *)MODULES;
    int module_index = -1, i = 0;

    // Find the show_time module
    for (i = 0; i < MAX_MODULES; i++) {
	if (strcmp(modules[i].module_name, "testmod_showtime.o") == 0) {
	    module_index = modules[i].module_index;
	    break;
	}
    }

    if (module_index == -1) {
	// Module not present. Nothing much we can do.
	return;
    }

    // Invoke the module
    if (modules[i].module_vectors.show_time_vector) {
	modules[i].module_vectors.show_time_vector();
    }
}

// End of File
