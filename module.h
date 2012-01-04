#ifndef JOS_KERN_MODULE_H
#define JOS_KERN_MODULE_H

// Defines

#define MAX_MODULES		16
#define MAX_MODULE_NAMELEN	32
#define MAX_SECTION_NAMELEN	16
#define MAX_SYM_NAMELEN		16
#define MAX_SECTIONS		10
#define MAX_MD_SIZE		4096
#define MAX_REL_ENTRIES		100
#define MAX_SYM_TABLE_SIZE	100

#define INVALID_MODULE_INDEX	-1

// Section identifiers
#define SECTION_NULL		0
#define SECTION_TEXT		1
#define SECTION_RODATA		2
#define	SECTION_BSS		3
#define	SECTION_DATA		4
#define	SECTION_REL		5
#define	SECTION_SYMTAB		6
#define	SECTION_STRTAB		7
#define	SECTION_SHSTRTAB	8

// Different API types exposed by the modules
#define MODULE_SHOW_SYSCALL	0
#define MODULE_COUNT_SYSCALL	1
#define MODULE_SHOW_TIME	2
#define MODULE_TEST_API		3

// Data Structures

enum Module_state {
    MODULE_STATE_INIT,
    MODULE_STATE_ACTIVE,
    MODULE_STATE_DELETED
};

struct Section {
    char	sh_name[MAX_SECTION_NAMELEN];
    uint32_t	sh_start;
    uint32_t	sh_size;
    uint32_t	sh_type;
    uint32_t	sh_offset;
};

struct RelText {
    uint32_t	    rel_offset;
    uint32_t	    rel_type;
    char	    sym_name[MAX_SYM_NAMELEN];
};

struct SymTab {
    char	    sym_name[MAX_SYM_NAMELEN];
    uint32_t	    sym_value;
};

struct module_vectors {
    int             (*show_syscall_vector)(void);
    int             (*count_syscall_vector)(void);
    int		    (*show_time_vector)(void);
    int		    (*test_api_vector)(void);
};

struct Module {
    char		    module_name[MAX_MODULE_NAMELEN];
    uint32_t		    module_index;
    uint32_t		    module_size;
    int			    module_state;
    int			    (*init_routine)(uint32_t);
    int			    (*cleanup_routine)(uint32_t);
    uint32_t		    module_base;
    uint32_t		    module_sh_count;
    uint32_t		    module_rel_count;
    uint32_t		    module_sym_count;
    struct Section	    sections[MAX_SECTIONS];
    struct RelText	    rel_entry[MAX_REL_ENTRIES];
    struct SymTab	    sym_table[MAX_SYM_TABLE_SIZE];
    struct module_vectors   module_vectors;
};

extern struct Module *modules;
extern void *module_data;

// Function Prototypes

int module_init (char *mod_name, void *mod_binary, uint32_t mod_binary_size);
int module_cleanup (char *mod_name);
int module_display (void);
int module_invoke_insmod (void);
int module_invoke_rmmod (void);
int module_invoke_test_syscall (void);
void module_register (uint32_t mod_index, int type, void *vector);
void module_invoke_count_syscall (void);
void module_invoke_show_syscall (void);
void module_invoke_show_time (void);

#endif // !JOS_KERN_MODULE_H
