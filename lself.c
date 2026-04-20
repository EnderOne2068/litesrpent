/* lself.c -- ELF64 and PE32+ binary writers for cross-compilation.
 *
 * ls_write_elf: writes a minimal static ELF64 executable.
 * ls_write_pe:  writes a minimal PE32+ .exe.
 *
 * These are used by the AOT compiler to produce executables directly
 * without invoking a linker, and by the "compile to .elf on Windows"
 * emulation feature.
 */
#include "lscore.h"
#include "lseval.h"
#include <string.h>

/* ============================================================
 *  ELF64 structures (defined inline -- no elf.h needed)
 * ============================================================ */
#pragma pack(push, 1)

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

/* PE structures */
typedef struct {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} PE_FileHeader;

typedef struct {
    uint16_t Magic;
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint64_t SizeOfStackReserve;
    uint64_t SizeOfStackCommit;
    uint64_t SizeOfHeapReserve;
    uint64_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
} PE64_OptionalHeader;

typedef struct {
    char     Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} PE_SectionHeader;

#pragma pack(pop)

/* ---- ELF writer ---- */
int ls_write_elf(ls_state_t *L, const char *path,
                 const uint8_t *code, size_t code_len,
                 uint64_t entry_point) {
    FILE *f = fopen(path, "wb");
    if (!f) { ls_error(L, "cannot open %s for writing", path); return -1; }

    uint64_t base = 0x400000;
    uint64_t entry = base + sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
    if (entry_point) entry = entry_point;

    /* ELF header */
    Elf64_Ehdr eh; memset(&eh, 0, sizeof eh);
    eh.e_ident[0] = 0x7f; eh.e_ident[1] = 'E'; eh.e_ident[2] = 'L'; eh.e_ident[3] = 'F';
    eh.e_ident[4] = 2;  /* ELFCLASS64 */
    eh.e_ident[5] = 1;  /* ELFDATA2LSB */
    eh.e_ident[6] = 1;  /* EV_CURRENT */
    eh.e_ident[7] = 0;  /* ELFOSABI_NONE */
    eh.e_type     = 2;  /* ET_EXEC */
    eh.e_machine  = 62; /* EM_X86_64 */
    eh.e_version  = 1;
    eh.e_entry    = entry;
    eh.e_phoff    = sizeof(Elf64_Ehdr);
    eh.e_shoff    = 0;
    eh.e_ehsize   = sizeof(Elf64_Ehdr);
    eh.e_phentsize= sizeof(Elf64_Phdr);
    eh.e_phnum    = 1;
    eh.e_shnum    = 0;

    /* Program header: single LOAD segment */
    Elf64_Phdr ph; memset(&ph, 0, sizeof ph);
    ph.p_type   = 1;    /* PT_LOAD */
    ph.p_flags  = 5;    /* PF_R | PF_X */
    ph.p_offset = 0;
    ph.p_vaddr  = base;
    ph.p_paddr  = base;
    ph.p_filesz = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr) + code_len;
    ph.p_memsz  = ph.p_filesz;
    ph.p_align  = 0x200000;

    fwrite(&eh, 1, sizeof eh, f);
    fwrite(&ph, 1, sizeof ph, f);
    fwrite(code, 1, code_len, f);
    fclose(f);
    return 0;
}

/* ---- PE writer ---- */
int ls_write_pe(ls_state_t *L, const char *path,
                const uint8_t *code, size_t code_len) {
    FILE *f = fopen(path, "wb");
    if (!f) { ls_error(L, "cannot open %s for writing", path); return -1; }

    uint32_t file_align = 0x200;
    uint32_t sect_align = 0x1000;
    uint64_t image_base = 0x140000000ULL;
    uint32_t header_size = 0x200; /* aligned */
    uint32_t code_raw = (uint32_t)((code_len + file_align - 1) & ~(file_align - 1));

    /* DOS stub */
    uint8_t dos[128]; memset(dos, 0, sizeof dos);
    dos[0] = 'M'; dos[1] = 'Z';
    *(uint32_t*)(dos + 0x3c) = 128; /* e_lfanew -> PE sig */
    fwrite(dos, 1, 128, f);

    /* PE signature */
    uint32_t pe_sig = 0x00004550; /* "PE\0\0" */
    fwrite(&pe_sig, 4, 1, f);

    /* COFF file header */
    PE_FileHeader fh; memset(&fh, 0, sizeof fh);
    fh.Machine = 0x8664; /* AMD64 */
    fh.NumberOfSections = 1;
    fh.SizeOfOptionalHeader = sizeof(PE64_OptionalHeader) + 16*8; /* data directories */
    fh.Characteristics = 0x22; /* EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE */
    fwrite(&fh, 1, sizeof fh, f);

    /* Optional header (PE32+) */
    PE64_OptionalHeader oh; memset(&oh, 0, sizeof oh);
    oh.Magic = 0x20b; /* PE32+ */
    oh.SizeOfCode = code_raw;
    oh.AddressOfEntryPoint = sect_align;
    oh.BaseOfCode = sect_align;
    oh.ImageBase = image_base;
    oh.SectionAlignment = sect_align;
    oh.FileAlignment = file_align;
    oh.MajorOperatingSystemVersion = 6;
    oh.MajorSubsystemVersion = 6;
    oh.SizeOfImage = sect_align + ((code_raw + sect_align - 1) & ~(sect_align - 1));
    oh.SizeOfHeaders = header_size;
    oh.Subsystem = 3; /* IMAGE_SUBSYSTEM_WINDOWS_CUI */
    oh.SizeOfStackReserve = 0x100000;
    oh.SizeOfStackCommit = 0x1000;
    oh.SizeOfHeapReserve = 0x100000;
    oh.SizeOfHeapCommit = 0x1000;
    oh.NumberOfRvaAndSizes = 16;
    fwrite(&oh, 1, sizeof oh, f);

    /* 16 data directory entries (all zero) */
    uint8_t dd[16*8]; memset(dd, 0, sizeof dd);
    fwrite(dd, 1, sizeof dd, f);

    /* Section header: .text */
    PE_SectionHeader sh; memset(&sh, 0, sizeof sh);
    memcpy(sh.Name, ".text\0\0\0", 8);
    sh.VirtualSize = (uint32_t)code_len;
    sh.VirtualAddress = sect_align;
    sh.SizeOfRawData = code_raw;
    sh.PointerToRawData = header_size;
    sh.Characteristics = 0x60000020; /* CODE | EXECUTE | READ */
    fwrite(&sh, 1, sizeof sh, f);

    /* Pad to header_size */
    long pos = ftell(f);
    if (pos < (long)header_size) {
        uint8_t pad[512]; memset(pad, 0, sizeof pad);
        fwrite(pad, 1, header_size - pos, f);
    }

    /* Code section */
    fwrite(code, 1, code_len, f);
    /* Pad to file_align */
    if (code_len < code_raw) {
        uint8_t pad[512]; memset(pad, 0, sizeof pad);
        size_t rem = code_raw - code_len;
        while (rem > 0) { size_t w = rem > 512 ? 512 : rem; fwrite(pad, 1, w, f); rem -= w; }
    }

    fclose(f);
    return 0;
}

/* ---- Lisp API ---- */
static ls_value_t bi_write_elf(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 2) { ls_error(L, "write-elf: need path and code-vector"); return ls_nil_v(); }
    ls_string_t *path = ls_string_p(a[0]);
    if (!path) { ls_error(L, "write-elf: path must be string"); return ls_nil_v(); }
    ls_vector_t *code = ls_vector_p(a[1]);
    if (!code) { ls_error(L, "write-elf: code must be vector of bytes"); return ls_nil_v(); }
    uint8_t *bytes = (uint8_t*)malloc(code->len);
    for (size_t i = 0; i < code->len; i++)
        bytes[i] = (uint8_t)(code->data[i].tag == LS_T_FIXNUM ? code->data[i].u.fixnum : 0);
    uint64_t entry = n >= 3 ? (uint64_t)a[2].u.fixnum : 0;
    int res = ls_write_elf(L, path->chars, bytes, code->len, entry);
    free(bytes);
    return res == 0 ? ls_t_v() : ls_nil_v();
}

static ls_value_t bi_write_pe(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 2) { ls_error(L, "write-pe: need path and code-vector"); return ls_nil_v(); }
    ls_string_t *path = ls_string_p(a[0]);
    ls_vector_t *code = ls_vector_p(a[1]);
    if (!path || !code) { ls_error(L, "write-pe: bad args"); return ls_nil_v(); }
    uint8_t *bytes = (uint8_t*)malloc(code->len);
    for (size_t i = 0; i < code->len; i++)
        bytes[i] = (uint8_t)(code->data[i].tag == LS_T_FIXNUM ? code->data[i].u.fixnum : 0);
    int res = ls_write_pe(L, path->chars, bytes, code->len);
    free(bytes);
    return res == 0 ? ls_t_v() : ls_nil_v();
}

void ls_init_elf(ls_state_t *L) {
    ls_defun(L, "LITESRPENT-SYSTEM", "WRITE-ELF", bi_write_elf, 2, 3);
    ls_defun(L, "LITESRPENT-SYSTEM", "WRITE-PE", bi_write_pe, 2, 2);
}
