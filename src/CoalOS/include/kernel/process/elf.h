#ifndef ELF_H
#define ELF_H

#include "libc/stdint.h" // or wherever you keep uint16_t, uint32_t, etc.

/* ---- Basic ELF macros ---- */
#define EI_NIDENT 16

/* Indices into e_ident[] */
#define EI_MAG0   0
#define EI_MAG1   1
#define EI_MAG2   2
#define EI_MAG3   3
#define EI_CLASS  4 // <<< ADDED
#define EI_DATA   5 // <<< ADDED
// etc...

/* ELF magic */
#define ELFMAG0 0x7F
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

// <<< ADDED Standard ELF Definitions >>>
/* e_ident[EI_CLASS] */
#define ELFCLASSNONE 0 /* Invalid class */
#define ELFCLASS32   1 /* 32-bit objects */
#define ELFCLASS64   2 /* 64-bit objects */

/* e_ident[EI_DATA] */
#define ELFDATANONE 0 /* Invalid data encoding */
#define ELFDATA2LSB 1 /* 2's complement, little endian */
#define ELFDATA2MSB 2 /* 2's complement, big endian */
// <<< END ADDED >>>


/* e_type */
#define ET_NONE   0
#define ET_REL    1
#define ET_EXEC   2
#define ET_DYN    3
// etc...

/* e_machine */
#define EM_386    3

/* e_version */
#define EV_NONE     0
#define EV_CURRENT  1

/* Program header types */
#define PT_NULL    0
#define PT_LOAD    1
// etc...

/* Program header flags */
#define PF_X       0x1
#define PF_W       0x2
#define PF_R       0x4

/* ---- ELF base types ---- */
typedef uint16_t Elf32_Half;  /* 16-bit quantity */
typedef uint32_t Elf32_Word;  /* 32-bit quantity */
typedef int32_t  Elf32_Sword; /* signed 32-bit */
typedef uint32_t Elf32_Off;   /* offset */
typedef uint32_t Elf32_Addr;  /* address */

/* ---- 32-bit ELF Header ---- */
typedef struct {
    unsigned char e_ident[EI_NIDENT]; /* ELF identification */
    Elf32_Half    e_type;            /* Object file type */
    Elf32_Half    e_machine;         /* Machine type */
    Elf32_Word    e_version;         /* Object file version */
    Elf32_Addr    e_entry;           /* Entry point address */
    Elf32_Off     e_phoff;           /* Program header offset */
    Elf32_Off     e_shoff;           /* Section header offset */
    Elf32_Word    e_flags;           /* Processor-specific flags */
    Elf32_Half    e_ehsize;          /* ELF header size */
    Elf32_Half    e_phentsize;       /* Size of program header entry */
    Elf32_Half    e_phnum;           /* Number of program header entries */
    Elf32_Half    e_shentsize;       /* Size of section header entry */
    Elf32_Half    e_shnum;           /* Number of section header entries */
    Elf32_Half    e_shstrndx;        /* Section name string table index */
} Elf32_Ehdr;

/* ---- 32-bit Program Header ---- */
typedef struct {
    Elf32_Word p_type;   /* Type of segment */
    Elf32_Off  p_offset; /* File offset where segment is located */
    Elf32_Addr p_vaddr;  /* Virtual address of segment in memory */
    Elf32_Addr p_paddr;  /* Physical address (not used on many systems) */
    Elf32_Word p_filesz; /* Size of segment in file */
    Elf32_Word p_memsz;  /* Size of segment in memory */
    Elf32_Word p_flags;  /* Segment flags */
    Elf32_Word p_align;  /* Segment alignment */
} Elf32_Phdr;

#endif /* ELF_H */