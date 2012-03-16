#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/param.h>

#include <elf.h>
#include <link.h>

#include "hijack.h"
#include "error.h"
#include "misc.h"
#include "hijack_ptrace.h"
#include "map.h"
#include "hijack_elf.h"

struct rtld_loadable {
    union {
        void *ptr;
        unsigned char *buf;
        ElfW(Phdr) *phdr;
    } phdr;

    unsigned long vaddr;
    unsigned long addr;
    unsigned long limit;
    unsigned long offset;

    struct rtld_loadable *next;
};

struct rtld_aux {
    char *path;
    int fd;
    void *lmap; /* short for "local map" */
    struct stat sb;
    
    union {
        void *ptr;
        unsigned char *buf;
        ElfW(Ehdr) *ehdr;
    } ehdr;

    union {
        void *ptr;
        unsigned char *buf;
        ElfW(Phdr) *phdr;
    } phdr;

    ElfW(Phdr) *phdyn;
    ElfW(Phdr) *phtls;
    ElfW(Phdr) *phinterp;

    unsigned long phdr_vaddr;
    unsigned long phsize;
    unsigned long stack_flags;
    unsigned long relro_page;
    unsigned long relro_size;

    unsigned long base_addr;
    unsigned long base_vaddr;
    unsigned long base_offset;
    unsigned long base_vlimit;
    unsigned long mapsize;
    unsigned long mapping;

    /* Used for storing auxiliary info (struct Struct_Obj_entry */
    unsigned long auxmap;

    struct rtld_loadable *loadables;
};

void rtld_add_loadable(HIJACK *hijack, struct rtld_aux *aux, ElfW(Phdr) *phdr) {
    struct rtld_loadable *loadable;

    if ((aux->loadables)) {
        for (loadable = aux->loadables; loadable->next != NULL; loadable = loadable->next)
            ;

        loadable->next = _hijack_malloc(hijack, sizeof(struct rtld_loadable));
        if (!(loadable->next))
            return;

        loadable = loadable->next;
    } else {
        aux->loadables = loadable = _hijack_malloc(hijack, sizeof(struct rtld_loadable));
        if (!(loadable))
            return;
    }

    loadable->phdr.phdr = phdr;
}

int rtld_load_headers(HIJACK *hijack, struct rtld_aux *aux) {
    unsigned long i;

    aux->lmap = mmap(NULL, aux->sb.st_size, PROT_READ|PROT_WRITE, MAP_PRIVATE, aux->fd, 0);
    if (aux->lmap == MAP_FAILED) {
        perror("[-] rtld_load_headers: mmap");
        return -1;
    }

    aux->ehdr.ehdr = aux->lmap;
    aux->phdr.phdr = aux->lmap + aux->ehdr.ehdr->e_phoff;

    for (i=0; i < aux->ehdr.ehdr->e_phnum; i++) {
        switch (aux->phdr.phdr[i].p_type) {
            case PT_INTERP:
                aux->phinterp = aux->phdr.phdr + i;
                break;
            case PT_PHDR:
                aux->phdr_vaddr = aux->phdr.phdr[i].p_vaddr;
                aux->phsize = aux->phdr.phdr[i].p_memsz;
                break;
            case PT_DYNAMIC:
                aux->phdyn = aux->phdr.phdr + i;
                break;
            case PT_LOAD:
                rtld_add_loadable(hijack, aux, aux->phdr.phdr + i);
                break;
            case PT_TLS:
                aux->phtls = aux->phdr.phdr + i;
                break;
            case PT_GNU_RELRO:
                aux->relro_page = aux->phdr.phdr[i].p_vaddr;
                aux->relro_size = aux->phdr.phdr[i].p_memsz;
                break;
        }
    }

    return 0;
}

/*
 * Actually load the shared object
 * Logic taken from freebsd/9-stable/libexec/rtld-elf/map_object.c
 */
void rtld_create_maps(HIJACK *hijack, struct rtld_aux *aux) {
    struct rtld_loadable *first_loadable, *last_loadable, *loadable;
    int err;
    char *bss;
    unsigned long bss_vaddr, bss_addr, bss_page, bss_vlimit, nclear;

    /* Grab first and last loadable PHDRs */
    first_loadable = aux->loadables;
    for (last_loadable = aux->loadables; last_loadable->next != NULL; last_loadable = last_loadable->next)
        ;

    /* Create one large mapping to hold the whole shared object */
    aux->base_offset = trunc_page(first_loadable->phdr.phdr->p_offset);
    aux->base_vaddr = trunc_page(first_loadable->phdr.phdr->p_vaddr);
    aux->base_vlimit = round_page(last_loadable->phdr.phdr->p_vaddr + last_loadable->phdr.phdr->p_memsz);
    aux->mapsize = aux->base_vlimit - aux->base_vaddr;
    aux->base_addr = (unsigned long)NULL;

    aux->mapping = MapMemory(hijack, aux->base_addr, aux->mapsize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_SHARED);

    if (IsFlagSet(hijack, F_DEBUG)) {
        fprintf(stderr, "map[0x%016lx]:\n", aux->mapping);
        fprintf(stderr, "    mapsize\t= %lu\n", aux->mapsize);
        fprintf(stderr, "    limit\t= %lu\n", aux->base_vlimit);
    }

    /* Do the math for all the PHDRs */
    for (loadable = first_loadable; loadable != NULL; loadable = loadable->next) {
        loadable->offset = trunc_page(loadable->phdr.phdr->p_offset);
        loadable->vaddr = trunc_page(loadable->phdr.phdr->p_vaddr);
        loadable->limit = round_page(loadable->phdr.phdr->p_vaddr + loadable->phdr.phdr->p_filesz);
        loadable->addr = aux->mapping + (loadable->vaddr - aux->base_vaddr);

        if (loadable->phdr.phdr->p_filesz != loadable->phdr.phdr->p_memsz) {
            /* BSS */
            bss_vaddr = loadable->phdr.phdr->p_vaddr + loadable->phdr.phdr->p_filesz;
            bss_addr = aux->mapping + (bss_vaddr - aux->base_vaddr);
            bss_page = aux->mapping + (trunc_page(bss_vaddr) - aux->base_vaddr);
            nclear = loadable->limit - bss_vaddr;

            if (nclear > 0) {
                bss = _hijack_malloc(hijack, nclear);
                if (!(bss))
                    return;
                err = WriteData(hijack, bss_addr, bss, nclear);
                free(bss);

                if (IsFlagSet(hijack, F_DEBUG) && IsFlagSet(hijack, F_DEBUG_VERBOSE)) {
                    fprintf(stderr, "Wrote BSS to 0x%016lx. Length %lu.\n", bss_addr, nclear);
                }
            }
        } else {
            err = WriteData(hijack, loadable->addr, aux->lmap + loadable->offset, loadable->phdr.phdr->p_memsz);
            if (IsFlagSet(hijack, F_DEBUG) && IsFlagSet(hijack, F_DEBUG_VERBOSE)) {
                fprintf(stderr, "Wrote to 0x%016lx. Length %lu. From offset %lu.\n", loadable->addr, loadable->phdr.phdr->p_memsz, loadable->offset);
            }
        }
    }
}

void rtld_hook_into_rtld(HIJACK *hijack, struct rtld_aux *aux)
{
    struct Struct_Obj_Entry soe;
    
    memset(&soe, 0x00, sizeof(struct Struct_Obj_Entry));

    soe.phsize = aux->ehdr.ehdr->e_phnum * sizeof(ElfW(Phdr));
    soe.mapbase = aux->mapping;
    soe.mapsize = aux->mapsize;
    soe.textsize = round_page(aux->loadables->phdr.phdr->p_vaddr + aux->loadables->phdr.phdr->p_memsz) - aux->base_vaddr;
    soe.vaddrbase = aux->base_vaddr;
    soe.relocbase = aux->mapping - aux->base_vaddr;
    soe.dynamic = (ElfW(Dyn) *)(soe.relocbase + aux->phdyn->p_vaddr);
    if (aux->ehdr.ehdr->e_entry)
        soe.entry = soe.relocbase + aux->ehdr.ehdr->e_entry;
    if (aux->phdr_vaddr) {
        soe.phdr = (ElfW(Phdr) *)(soe.relocbase + aux->phdr_vaddr);
    } else {
        soe.phdr = _hijack_malloc(hijack, soe.phsize);
        if (!(soe.phdr))
            return;

        memcpy(soe.phdr, aux->ehdr.ptr + aux->ehdr.ehdr->e_phoff, soe.phsize);
        soe.phdr_alloc = true;
    }
    if ((aux->phinterp))
        soe.interp = soe.relocbase + aux->phinterp->p_vaddr;
    if ((aux->phtls)) {
        /* TODO: Figure this part out */
    }
    soe.stack_flags = PROT_READ | PROT_WRITE | PROT_EXEC;
    soe.relro_page = soe.relocbase + trunc_page(soe.relro_page);
    soe.relro_size = round_page(soe.relro_size);

    /* Create auxiliary mapping and write the Struct_Obj_Entry */
    aux->auxmap = MapMemory(hijack, (unsigned long)NULL, getpagesize(), PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_SHARED);
    WriteData(hijack, aux->auxmap, &soe, sizeof(struct Struct_Obj_Entry));
}

EXPORTED_SYM int load_library(HIJACK *hijack, char *path)
{
    struct rtld_aux aux;
    memset(&aux, 0x00, sizeof(struct rtld_aux));

    aux.path = strdup(path);
    stat(aux.path, &(aux.sb));

    aux.fd = open(aux.path, O_RDONLY);
    if (aux.fd < 0)
        return -1;

    if (rtld_load_headers(hijack, &aux) == -1)
        return -1;

    rtld_create_maps(hijack, &aux);

    rtld_hook_into_rtld(hijack, &aux);

    return 0;
}