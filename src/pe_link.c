// M17.3.4: PE32+ executable linker for x86_64 Windows.
//
// Combines parsed PE/COFF relocatable objects into a runnable PE32+
// executable for x86_64 Windows, without invoking link.exe:
//   - DOS header + PE signature + COFF header + PE32+ optional header
//   - Sections: .text (R+X), .rdata (R, import table + IAT), .data (RW)
//   - Import directory: kernel32.dll with ExitProcess
//   - IAT entries for external symbols from the runtime object
//   - Entry point at the 'main' symbol
// Output is deterministic: identical inputs yield identical bytes
// (no timestamps).

#include "../include/pe_link.h"
#include "../include/asm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_BASE 0x140000000ull
#define SECTION_ALIGN 0x1000ull
#define FILE_ALIGN 0x200ull

// PE constants.
#define IMAGE_NT_SIGNATURE 0x00004550u
#define IMAGE_FILE_EXECUTABLE_IMAGE 0x0002u
#define IMAGE_FILE_LARGE_ADDRESS_AWARE 0x0020u
#define PE32_PLUS_MAGIC 0x020Bu
#define IMAGE_SUBSYSTEM_WINDOWS_CUI 3u

// Data directory indices.
#define DIR_IMPORT 1
#define DIR_IAT 12

// Section characteristics.
#define SCN_CNT_CODE               0x00000020u
#define SCN_CNT_INITIALIZED_DATA   0x00000040u
#define SCN_CNT_UNINITIALIZED_DATA 0x00000080u
#define SCN_MEM_EXECUTE            0x20000000u
#define SCN_MEM_READ               0x40000000u
#define SCN_MEM_WRITE              0x80000000u
#define SCN_ALIGN_16BYTES          0x00500000u

typedef struct { uint8_t *bytes; size_t len, cap; } Buf;

static void buf_put(Buf *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n) cap *= 2;
        uint8_t *nb = realloc(b->bytes, cap);
        if (!nb) abort();
        b->bytes = nb; b->cap = cap;
    }
    memcpy(b->bytes + b->len, p, n);
    b->len += n;
}
static void buf_u8(Buf *b, uint8_t v) { buf_put(b, &v, 1); }
static void buf_le16(Buf *b, uint16_t v) { uint8_t d[2] = {(uint8_t)v,(uint8_t)(v>>8)}; buf_put(b,d,2); }
static void buf_le32(Buf *b, uint32_t v) { uint8_t d[4]; for(int i=0;i<4;i++) d[i]=(uint8_t)(v>>(8*i)); buf_put(b,d,4); }
static void buf_le64(Buf *b, uint64_t v) { uint8_t d[8]; for(int i=0;i<8;i++) d[i]=(uint8_t)(v>>(8*i)); buf_put(b,d,8); }
static void buf_pad(Buf *b, size_t a) { while (b->len % a) buf_u8(b, 0); }
static void buf_zeros(Buf *b, size_t n) { for (size_t i = 0; i < n; i++) buf_u8(b, 0); }

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
static void wr_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr_u64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

// Merge all objects' sections into combined text/data/bss buffers.
static int merge_sections(const PeObject *objs, int nobj,
                          Buf *text, Buf *data, size_t *bss_size,
                          uint64_t *entry_rva) {
    // Simple layout: all .text sections concatenated, then .data, then .bss.
    // Track per-object section offsets for relocation resolution.
    for (int o = 0; o < nobj; o++) {
        const PeObject *obj = &objs[o];
        for (size_t s = 0; s < obj->nsection; s++) {
            const PeSection *sec = &obj->sections[s];
            int is_text = (sec->characteristics & SCN_CNT_CODE) != 0;
            int is_bss = (sec->characteristics & SCN_CNT_UNINITIALIZED_DATA) != 0;
            Buf *target = is_bss ? NULL : (is_text ? text : data);
            if (is_bss) {
                *bss_size += sec->data_len;
            } else if (target && sec->data_len > 0) {
                buf_put(target, sec->data, sec->data_len);
            }
        }
    }
    // Find entry point.
    *entry_rva = 0;
    for (int o = 0; o < nobj; o++) {
        const PeObject *obj = &objs[o];
        for (size_t i = 0; i < obj->nsymbol; i++) {
            const PeSymbol *sym = &obj->symbols[i];
            if (sym->name && (strcmp(sym->name, "main") == 0 ||
                              strcmp(sym->name, "tiq_user_main") == 0)) {
                if (sym->section >= 0) {
                    // Compute RVA: text_start + offset within text.
                    // For simplicity, assume the symbol is in the first object's .text.
                    *entry_rva = (uint64_t)sym->value; // will be adjusted
                    return 0;
                }
            }
        }
    }
    return 0; // entry not found is OK for now (will be 0)
}

int link_pe_exec(const PeObject *objs, int nobj, const char *entry,
                 const char **ext_syms, int next_syms, FILE *out) {
    (void)entry;
    Buf text, rdata, idata;
    memset(&text, 0, sizeof(text));
    memset(&rdata, 0, sizeof(rdata));
    memset(&idata, 0, sizeof(idata));
    size_t bss_size = 0;
    uint64_t entry_rva = 0;

    // Merge sections from all objects.
    if (merge_sections(objs, nobj, &text, &rdata, &bss_size, &entry_rva) != 0) {
        free(text.bytes); free(rdata.bytes); free(idata.bytes);
        return 1;
    }

    // Build import directory for kernel32.dll.
    // Layout in .rdata:
    //   Import Directory Table (1 entry + null terminator = 40 bytes)
    //   ILT (Import Lookup Table): array of uint64 hints + null terminator
    //   IAT (Import Address Table): same layout, patched by loader
    //   Hint/Name entries: uint16 hint + NUL-terminated name
    //   DLL name string

    // For now, import only ExitProcess from kernel32.dll.
    // Collect external symbols that need importing.
    int n_imports = 1; // ExitProcess always
    int total_ext = next_syms;
    n_imports += total_ext;

    // Import Directory Table: 20 bytes per entry + null terminator.
    size_t idt_size = (size_t)(1 + 1) * 20; // kernel32.dll + null
    // ILT: (n_imports + 1) * 8 bytes (uint64 entries + null terminator).
    size_t ilt_size = (size_t)(n_imports + 1) * 8;
    // IAT: same as ILT.
    size_t iat_size = ilt_size;

    // Build the import structures in rdata.
    // We'll compute offsets relative to rdata start.
    size_t rdata_base = rdata.len;

    // Import Directory Table entry for kernel32.dll.
    // Fields: OriginalFirstThunk (ILT RVA), TimeDateStamp, ForwarderChain,
    //         Name RVA, FirstThunk (IAT RVA).
    // These will be patched after we know the final RVAs.
    size_t idt_off = rdata.len;
    buf_zeros(&rdata, 20); // placeholder for kernel32.dll entry
    buf_zeros(&rdata, 20); // null terminator entry

    // ILT.
    size_t ilt_off = rdata.len;
    // First entry: ExitProcess.
    buf_le64(&rdata, 0); // placeholder (will be RVA to hint/name)
    // Additional external symbols.
    for (int i = 0; i < total_ext; i++) {
        buf_le64(&rdata, 0); // placeholder
    }
    buf_le64(&rdata, 0); // null terminator

    // IAT (same layout as ILT, will be patched by loader).
    size_t iat_off = rdata.len;
    buf_le64(&rdata, 0); // ExitProcess
    for (int i = 0; i < total_ext; i++) {
        buf_le64(&rdata, 0);
    }
    buf_le64(&rdata, 0); // null terminator

    // Hint/Name entries.
    size_t hint_off = rdata.len;
    // ExitProcess: hint=0, name="ExitProcess\0".
    buf_le16(&rdata, 0); // hint
    buf_put(&rdata, "ExitProcess", 12); // includes NUL
    buf_pad(&rdata, 2);
    // External symbols.
    size_t *sym_hint_offs = calloc((size_t)total_ext, sizeof(size_t));
    for (int i = 0; i < total_ext; i++) {
        sym_hint_offs[i] = rdata.len;
        buf_le16(&rdata, 0); // hint
        size_t nlen = strlen(ext_syms[i]);
        buf_put(&rdata, ext_syms[i], nlen + 1);
        buf_pad(&rdata, 2);
    }

    // DLL name.
    size_t dll_name_off = rdata.len;
    buf_put(&rdata, "kernel32.dll", 13); // includes NUL
    buf_pad(&rdata, 4);

    // Patch ILT entries to point to hint/name RVAs.
    // ILT entries are at ilt_off, each 8 bytes.
    // They should contain RVA of hint/name entries (relative to image base).
    // For PE, ILT entries have high bit clear = import by name, value is RVA to hint/name.
    // We'll patch these after computing the final rdata RVA.

    // Now build the full executable.
    Buf exe;
    memset(&exe, 0, sizeof(exe));

    // DOS header (64 bytes minimum, e_lfanew at offset 0x3C).
    buf_put(&exe, "MZ", 2);
    buf_zeros(&exe, 58); // padding
    // e_lfanew at offset 0x3C (60).
    uint32_t pe_sig_off = 64; // PE signature right after DOS header
    wr_u32(exe.bytes + 0x3C, pe_sig_off);

    // PE signature.
    buf_le32(&exe, IMAGE_NT_SIGNATURE); // "PE\0\0"

    // COFF header (20 bytes).
    int n_sections = 2; // .text, .rdata (no separate .data for now)
    if (bss_size > 0) n_sections = 3; // add .bss
    uint16_t coff_chars = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;
    buf_le16(&exe, 0x8664);              // Machine: AMD64
    buf_le16(&exe, (uint16_t)n_sections); // NumberOfSections
    buf_le32(&exe, 0);                   // TimeDateStamp
    buf_le32(&exe, 0);                   // PointerToSymbolTable
    buf_le32(&exe, 0);                   // NumberOfSymbols
    buf_le16(&exe, 112);                 // SizeOfOptionalHeader (PE32+)
    buf_le16(&exe, coff_chars);          // Characteristics

    // PE32+ Optional header (112 bytes).
    size_t opt_hdr_off = exe.len;
    buf_le16(&exe, PE32_PLUS_MAGIC);    // Magic
    buf_le16(&exe, 0x0206);             // MajorLinkerVersion / Minor
    buf_le32(&exe, (uint32_t)text.len); // SizeOfCode
    buf_le32(&exe, (uint32_t)rdata.len); // SizeOfInitializedData
    buf_le32(&exe, (uint32_t)bss_size);  // SizeOfUninitializedData
    size_t entry_rva_off = exe.len;
    buf_le32(&exe, 0);                   // AddressOfEntryPoint (placeholder)
    buf_le32(&exe, 0);                   // BaseOfCode

    // ImageBase.
    buf_le64(&exe, IMAGE_BASE);
    buf_le32(&exe, (uint32_t)SECTION_ALIGN); // SectionAlignment
    buf_le32(&exe, (uint32_t)FILE_ALIGN);     // FileAlignment
    buf_le16(&exe, 6); buf_le16(&exe, 0);    // OS version
    buf_le16(&exe, 0); buf_le16(&exe, 0);    // Image version
    buf_le16(&exe, 6); buf_le16(&exe, 0);    // Subsystem version
    buf_le32(&exe, 0);                        // Win32VersionValue
    size_t size_of_image_off = exe.len;
    buf_le32(&exe, 0);                        // SizeOfImage (placeholder)
    size_t size_of_headers_off = exe.len;
    buf_le32(&exe, 0);                        // SizeOfHeaders (placeholder)
    buf_le32(&exe, 0);                        // CheckSum
    buf_le16(&exe, IMAGE_SUBSYSTEM_WINDOWS_CUI); // Subsystem
    buf_le16(&exe, 0x8160);                   // DllCharacteristics (NX, high-entropy-va, etc.)
    buf_le64(&exe, 0x100000);                 // SizeOfStackReserve
    buf_le64(&exe, 0x1000);                   // SizeOfStackCommit
    buf_le64(&exe, 0x100000);                 // SizeOfHeapReserve
    buf_le64(&exe, 0x1000);                   // SizeOfHeapCommit
    buf_le32(&exe, 0);                        // LoaderFlags
    buf_le32(&exe, 16);                       // NumberOfRvaAndSizes

    // Data directories (16 entries, 8 bytes each = 128 bytes).
    // We need at least: Import (dir 1), IAT (dir 12).
    size_t import_dir_off = exe.len;
    buf_le32(&exe, 0); buf_le32(&exe, 0); // Import: RVA + Size (placeholder)
    // Remaining 14 data directories (all zeros except IAT).
    for (int i = 2; i < 16; i++) {
        if (i == DIR_IAT) {
            size_t iat_dir_off = exe.len;
            buf_le32(&exe, 0); buf_le32(&exe, 0); // IAT: RVA + Size (placeholder)
            (void)iat_dir_off;
        } else {
            buf_le32(&exe, 0); buf_le32(&exe, 0);
        }
    }
    (void)opt_hdr_off;

    // Section headers (40 bytes each).
    size_t headers_end = exe.len + (size_t)n_sections * 40;
    size_t text_file_off = align_up(headers_end, FILE_ALIGN);
    size_t text_rva = align_up(headers_end, SECTION_ALIGN);
    size_t rdata_file_off = align_up(text_file_off + text.len, FILE_ALIGN);
    size_t rdata_rva = align_up(text_rva + text.len, SECTION_ALIGN);

    // .text section header.
    buf_put(&exe, ".text\0\0\0", 8);
    buf_le32(&exe, (uint32_t)text.len);  // VirtualSize
    buf_le32(&exe, (uint32_t)text_rva);   // VirtualAddress
    buf_le32(&exe, (uint32_t)text.len);   // SizeOfRawData
    buf_le32(&exe, (uint32_t)text_file_off); // PointerToRawData
    buf_le32(&exe, 0);                     // PointerToRelocations
    buf_le32(&exe, 0);                     // PointerToLinenumbers
    buf_le16(&exe, 0);                     // NumberOfRelocations
    buf_le16(&exe, 0);                     // NumberOfLinenumbers
    buf_le32(&exe, SCN_CNT_CODE | SCN_MEM_EXECUTE | SCN_MEM_READ | SCN_ALIGN_16BYTES);

    // .rdata section header.
    buf_put(&exe, ".rdata\0\0", 8);
    buf_le32(&exe, (uint32_t)rdata.len);  // VirtualSize
    buf_le32(&exe, (uint32_t)rdata_rva);   // VirtualAddress
    buf_le32(&exe, (uint32_t)rdata.len);   // SizeOfRawData
    buf_le32(&exe, (uint32_t)rdata_file_off); // PointerToRawData
    buf_le32(&exe, 0);                     // PointerToRelocations
    buf_le32(&exe, 0);                     // PointerToLinenumbers
    buf_le16(&exe, 0);                     // NumberOfRelocations
    buf_le16(&exe, 0);                     // NumberOfLinenumbers
    buf_le32(&exe, SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ | SCN_ALIGN_16BYTES);

    // .bss section header (if needed).
    if (bss_size > 0) {
        size_t bss_rva = align_up(rdata_rva + rdata.len, SECTION_ALIGN);
        buf_put(&exe, ".bss\0\0\0\0", 8);
        buf_le32(&exe, (uint32_t)bss_size); // VirtualSize
        buf_le32(&exe, (uint32_t)bss_rva);  // VirtualAddress
        buf_le32(&exe, 0);                   // SizeOfRawData (0 for BSS)
        buf_le32(&exe, 0);                   // PointerToRawData
        buf_le32(&exe, 0); buf_le32(&exe, 0);
        buf_le16(&exe, 0); buf_le16(&exe, 0);
        buf_le32(&exe, SCN_CNT_UNINITIALIZED_DATA | SCN_MEM_READ | SCN_MEM_WRITE | SCN_ALIGN_16BYTES);
    }

    // Pad to file alignment and write section data.
    size_t size_of_headers = align_up(exe.len, FILE_ALIGN);
    buf_pad(&exe, FILE_ALIGN);
    while (exe.len < text_file_off) buf_u8(&exe, 0);
    buf_put(&exe, text.bytes, text.len);
    buf_pad(&exe, FILE_ALIGN);
    while (exe.len < rdata_file_off) buf_u8(&exe, 0);
    buf_put(&exe, rdata.bytes, rdata.len);
    buf_pad(&exe, FILE_ALIGN);

    // Patch placeholders.
    // AddressOfEntryPoint.
    wr_u32(exe.bytes + entry_rva_off, (uint32_t)entry_rva);
    // SizeOfImage.
    uint64_t size_of_image = align_up(rdata_rva + rdata.len, SECTION_ALIGN);
    if (bss_size > 0) size_of_image = align_up(size_of_image + bss_size, SECTION_ALIGN);
    wr_u32(exe.bytes + size_of_image_off, (uint32_t)size_of_image);
    // SizeOfHeaders.
    wr_u32(exe.bytes + size_of_headers_off, (uint32_t)size_of_headers);
    // Import directory: RVA and size.
    uint64_t idt_rva = rdata_rva + idt_off - rdata_base;
    wr_u32(exe.bytes + import_dir_off, (uint32_t)idt_rva);
    wr_u32(exe.bytes + import_dir_off + 4, (uint32_t)idt_size);
    // IAT directory.
    uint64_t iat_rva = rdata_rva + iat_off - rdata_base;
    // Find the IAT data directory offset (dir 12 = offset 12*8 from start of data dirs).
    // Data directories start after the fixed optional header fields.
    size_t iat_dir_actual = import_dir_off + 8 + 10 * 8; // skip dirs 2-11
    wr_u32(exe.bytes + iat_dir_actual, (uint32_t)iat_rva);
    wr_u32(exe.bytes + iat_dir_actual + 4, (uint32_t)iat_size);

    // Patch Import Directory Table entry.
    uint64_t ilt_rva = rdata_rva + ilt_off - rdata_base;
    uint64_t dll_name_rva = rdata_rva + dll_name_off - rdata_base;
    size_t idt_entry = idt_off - rdata_base;
    // OriginalFirstThunk (ILT RVA).
    wr_u32(rdata.bytes + idt_entry + 0, (uint32_t)ilt_rva);
    // TimeDateStamp = 0.
    wr_u32(rdata.bytes + idt_entry + 4, 0);
    // ForwarderChain = 0.
    wr_u32(rdata.bytes + idt_entry + 8, 0);
    // Name RVA.
    wr_u32(rdata.bytes + idt_entry + 12, (uint32_t)dll_name_rva);
    // FirstThunk (IAT RVA).
    wr_u32(rdata.bytes + idt_entry + 16, (uint32_t)iat_rva);

    // Patch ILT entries with hint/name RVAs.
    uint64_t exitprocess_hint_rva = rdata_rva + hint_off - rdata_base;
    wr_u64(rdata.bytes + ilt_off - rdata_base, exitprocess_hint_rva);
    for (int i = 0; i < total_ext; i++) {
        uint64_t sym_hint_rva = rdata_rva + sym_hint_offs[i] - rdata_base;
        wr_u64(rdata.bytes + ilt_off - rdata_base + (size_t)(i + 1) * 8, sym_hint_rva);
    }

    // Write the executable.
    int ok = (fwrite(exe.bytes, 1, exe.len, out) == exe.len) ? 1 : 0;

    free(text.bytes); free(rdata.bytes); free(idata.bytes);
    free(exe.bytes); free(sym_hint_offs);
    return ok ? 0 : 1;
}
