#include "mold.h"

#include <openssl/sha.h>
#include <shared_mutex>
#include <tbb/parallel_for_each.h>

void OutputEhdr::copy_buf() {
  auto &hdr = *(ElfEhdr *)(out::buf + shdr.sh_offset);
  memset(&hdr, 0, sizeof(hdr));

  memcpy(&hdr.e_ident, "\177ELF", 4);
  hdr.e_ident[EI_CLASS] = ELFCLASS64;
  hdr.e_ident[EI_DATA] = ELFDATA2LSB;
  hdr.e_ident[EI_VERSION] = EV_CURRENT;
  hdr.e_type = config.pie ? ET_DYN : ET_EXEC;
  hdr.e_machine = EM_X86_64;
  hdr.e_version = EV_CURRENT;
  hdr.e_entry = Symbol::intern(config.entry)->get_addr();
  hdr.e_phoff = out::phdr->shdr.sh_offset;
  hdr.e_shoff = out::shdr->shdr.sh_offset;
  hdr.e_ehsize = sizeof(ElfEhdr);
  hdr.e_phentsize = sizeof(ElfPhdr);
  hdr.e_phnum = out::phdr->shdr.sh_size / sizeof(ElfPhdr);
  hdr.e_shentsize = sizeof(ElfShdr);
  hdr.e_shnum = out::shdr->shdr.sh_size / sizeof(ElfShdr);
  hdr.e_shstrndx = out::shstrtab->shndx;
}

void OutputShdr::update_shdr() {
  shdr.sh_size = sizeof(ElfShdr);
  for (OutputChunk *chunk : out::chunks)
    if (chunk->kind != OutputChunk::HEADER)
      shdr.sh_size += sizeof(ElfShdr);
}

void OutputShdr::copy_buf() {
  u8 *base = out::buf + shdr.sh_offset;

  memset(base, 0, sizeof(ElfShdr));

  auto *ptr = (ElfShdr *)(base + sizeof(ElfShdr));
  for (OutputChunk *chunk : out::chunks)
    if (chunk->kind != OutputChunk::HEADER)
      *ptr++ = chunk->shdr;
}

static u32 to_phdr_flags(OutputChunk *chunk) {
  u32 ret = PF_R;
  if (chunk->shdr.sh_flags & SHF_WRITE)
    ret |= PF_W;
  if (chunk->shdr.sh_flags & SHF_EXECINSTR)
    ret |= PF_X;
  return ret;
}

std::vector<ElfPhdr> create_phdr() {
  std::vector<ElfPhdr> vec;

  auto define = [&](u32 type, u32 flags, u32 align, OutputChunk *chunk) {
    vec.push_back({});
    ElfPhdr &phdr = vec.back();
    phdr.p_type = type;
    phdr.p_flags = flags;
    phdr.p_align = std::max<u64>(align, chunk->shdr.sh_addralign);
    phdr.p_offset = chunk->shdr.sh_offset;
    phdr.p_filesz = (chunk->shdr.sh_type == SHT_NOBITS) ? 0 : chunk->shdr.sh_size;
    phdr.p_vaddr = chunk->shdr.sh_addr;
    phdr.p_paddr = chunk->shdr.sh_addr;
    phdr.p_memsz = chunk->shdr.sh_size;

    if (type == PT_LOAD)
      chunk->starts_new_ptload = true;
  };

  auto append = [&](OutputChunk *chunk) {
    ElfPhdr &phdr = vec.back();
    phdr.p_align = std::max<u64>(phdr.p_align, chunk->shdr.sh_addralign);
    phdr.p_filesz = (chunk->shdr.sh_type == SHT_NOBITS)
      ? chunk->shdr.sh_offset - phdr.p_offset
      : chunk->shdr.sh_offset + chunk->shdr.sh_size - phdr.p_offset;
    phdr.p_memsz = chunk->shdr.sh_addr + chunk->shdr.sh_size - phdr.p_vaddr;
  };

  auto is_bss = [](OutputChunk *chunk) {
    return chunk->shdr.sh_type == SHT_NOBITS && !(chunk->shdr.sh_flags & SHF_TLS);
  };

  // Create a PT_PHDR for the program header itself.
  define(PT_PHDR, PF_R, 8, out::phdr);

  // Create a PT_INTERP.
  if (out::interp)
    define(PT_INTERP, PF_R, 1, out::interp);

  // Create a PT_NOTE for each group of SHF_NOTE sections with the same
  // alignment requirement.
  for (int i = 0, end = out::chunks.size(); i < end;) {
    OutputChunk *first = out::chunks[i++];
    if (first->shdr.sh_type != SHT_NOTE)
      continue;

    u32 flags = to_phdr_flags(first);
    u32 alignment = first->shdr.sh_addralign;
    define(PT_NOTE, flags, alignment, first);

    while (i < end && out::chunks[i]->shdr.sh_type == SHT_NOTE &&
           to_phdr_flags(out::chunks[i]) == flags &&
           out::chunks[i]->shdr.sh_addralign == alignment)
      append(out::chunks[i++]);
  }

  // Create PT_LOAD segments.
  for (int i = 0, end = out::chunks.size(); i < end;) {
    OutputChunk *first = out::chunks[i++];
    if (!(first->shdr.sh_flags & SHF_ALLOC))
      break;

    u32 flags = to_phdr_flags(first);
    define(PT_LOAD, flags, PAGE_SIZE, first);

    if (!is_bss(first))
      while (i < end && !is_bss(out::chunks[i]) &&
             to_phdr_flags(out::chunks[i]) == flags)
        append(out::chunks[i++]);

    while (i < end && is_bss(out::chunks[i]) &&
           to_phdr_flags(out::chunks[i]) == flags)
      append(out::chunks[i++]);
  }

  // Create a PT_TLS.
  for (int i = 0; i < out::chunks.size(); i++) {
    if (!(out::chunks[i]->shdr.sh_flags & SHF_TLS))
      continue;

    define(PT_TLS, to_phdr_flags(out::chunks[i]), 1, out::chunks[i]);
    i++;
    while (i < out::chunks.size() && (out::chunks[i]->shdr.sh_flags & SHF_TLS))
      append(out::chunks[i++]);
  }

  // Add PT_DYNAMIC
  if (out::dynamic)
    define(PT_DYNAMIC, PF_R | PF_W, out::dynamic->shdr.sh_addralign, out::dynamic);

  // Add PT_GNU_STACK, which is a marker segment that doesn't really
  // contain any segments. If exists, the runtime turn on the No Exeecute
  // bit for stack pages.
  vec.push_back({});
  vec.back().p_type = PT_GNU_STACK;
  vec.back().p_flags = PF_R | PF_W;

  return vec;
}

void OutputPhdr::update_shdr() {
  shdr.sh_size = create_phdr().size() * sizeof(ElfPhdr);
}

void OutputPhdr::copy_buf() {
  write_vector(out::buf + shdr.sh_offset, create_phdr());
}

void InterpSection::copy_buf() {
  write_string(out::buf + shdr.sh_offset, config.dynamic_linker);
}

void RelDynSection::update_shdr() {
  shdr.sh_link = out::dynsym->shndx;

  int n = 0;

  for (Symbol *sym : out::got->got_syms)
    if (sym->is_imported || (config.pie && sym->is_relative()))
      n++;

  n += out::got->tlsgd_syms.size() * 2;
  n += out::copyrel->symbols.size();

  if (out::got->tlsld_idx != -1)
    n++;

  for (ObjectFile *file : out::objs) {
    file->reldyn_offset = n * sizeof(ElfRela);
    n += file->num_dynrel;
  }

  shdr.sh_size = n * sizeof(ElfRela);
}

void RelDynSection::copy_buf() {
  ElfRela *rel = (ElfRela *)(out::buf + shdr.sh_offset);

  for (Symbol *sym : out::got->got_syms) {
    if (sym->is_imported)
      *rel++ = {sym->get_got_addr(), R_X86_64_GLOB_DAT, sym->dynsym_idx, 0};
    else if (config.pie && sym->is_relative())
      *rel++ = {sym->get_got_addr(), R_X86_64_RELATIVE, 0, (i64)sym->get_addr()};
  }

  for (Symbol *sym : out::got->tlsgd_syms) {
    *rel++ = {sym->get_tlsgd_addr(), R_X86_64_DTPMOD64, sym->dynsym_idx, 0};
    *rel++ = {sym->get_tlsgd_addr() + GOT_SIZE, R_X86_64_DTPOFF64, sym->dynsym_idx, 0};
  }

  if (out::got->tlsld_idx != -1)
    *rel++ = {out::got->get_tlsld_addr(), R_X86_64_DTPMOD64, 0, 0};

  for (Symbol *sym : out::got->gottpoff_syms)
    if (sym->is_imported)
      *rel++ = {sym->get_gottpoff_addr(), R_X86_64_TPOFF32, sym->dynsym_idx, 0};

  for (Symbol *sym : out::copyrel->symbols)
    *rel++ = {sym->get_addr(), R_X86_64_COPY, sym->dynsym_idx, 0};
}

void StrtabSection::update_shdr() {
  shdr.sh_size = 1;
  for (ObjectFile *file : out::objs) {
    file->strtab_offset = shdr.sh_size;
    shdr.sh_size += file->strtab_size;
  }
}

void ShstrtabSection::update_shdr() {
  shdr.sh_size = 1;
  for (OutputChunk *chunk : out::chunks) {
    if (!chunk->name.empty()) {
      chunk->shdr.sh_name = shdr.sh_size;
      shdr.sh_size += chunk->name.size() + 1;
    }
  }
}

void ShstrtabSection::copy_buf() {
  u8 *base = out::buf + shdr.sh_offset;
  base[0] = '\0';

  int i = 1;
  for (OutputChunk *chunk : out::chunks) {
    if (!chunk->name.empty()) {
      write_string(base + i, chunk->name);
      i += chunk->name.size() + 1;
    }
  }
}

u32 DynstrSection::add_string(std::string_view str) {
  u32 ret = shdr.sh_size;
  shdr.sh_size += str.size() + 1;
  contents.push_back(str);
  return ret;
}

u32 DynstrSection::find_string(std::string_view str) {
  u32 i = 1;
  for (std::string_view s : contents) {
    if (s == str)
      return i;
    i += s.size() + 1;
  }
  unreachable();
}

void DynstrSection::copy_buf() {
  u8 *base = out::buf + shdr.sh_offset;
  base[0] = '\0';

  int i = 1;
  for (std::string_view s : contents) {
    write_string(base + i, s);
    i += s.size() + 1;
  }
}

void SymtabSection::update_shdr() {
  shdr.sh_size = sizeof(ElfSym);

  for (ObjectFile *file : out::objs) {
    file->local_symtab_offset = shdr.sh_size;
    shdr.sh_size += file->local_symtab_size;
  }

  for (ObjectFile *file : out::objs) {
    file->global_symtab_offset = shdr.sh_size;
    shdr.sh_size += file->global_symtab_size;
  }

  shdr.sh_info = out::objs[0]->global_symtab_offset / sizeof(ElfSym);
  shdr.sh_link = out::strtab->shndx;

  static Counter counter("symtab");
  counter.inc(shdr.sh_size / sizeof(ElfSym));
}

void SymtabSection::copy_buf() {
  memset(out::buf + shdr.sh_offset, 0, sizeof(ElfSym));
  out::buf[out::strtab->shdr.sh_offset] = '\0';

  tbb::parallel_for_each(out::objs, [](ObjectFile *file) { file->write_symtab(); });
}

static std::vector<u64> create_dynamic_section() {
  std::vector<u64> vec;

  auto define = [&](u64 tag, u64 val) {
    vec.push_back(tag);
    vec.push_back(val);
  };

  for (SharedFile *file : out::dsos)
    define(DT_NEEDED, out::dynstr->find_string(file->soname));

  define(DT_RUNPATH, out::dynstr->find_string(config.rpaths));
  define(DT_RELA, out::reldyn->shdr.sh_addr);
  define(DT_RELASZ, out::reldyn->shdr.sh_size);
  define(DT_RELAENT, sizeof(ElfRela));
  define(DT_JMPREL, out::relplt->shdr.sh_addr);
  define(DT_PLTRELSZ, out::relplt->shdr.sh_size);
  define(DT_PLTGOT, out::gotplt->shdr.sh_addr);
  define(DT_PLTREL, DT_RELA);
  define(DT_SYMTAB, out::dynsym->shdr.sh_addr);
  define(DT_SYMENT, sizeof(ElfSym));
  define(DT_STRTAB, out::dynstr->shdr.sh_addr);
  define(DT_STRSZ, out::dynstr->shdr.sh_size);
  define(DT_HASH, out::hash->shdr.sh_addr);
  define(DT_INIT_ARRAY, out::__init_array_start->value);
  define(DT_INIT_ARRAYSZ, out::__init_array_end->value - out::__init_array_start->value);
  define(DT_FINI_ARRAY, out::__fini_array_start->value);
  define(DT_FINI_ARRAYSZ, out::__fini_array_end->value - out::__fini_array_start->value);
  define(DT_VERSYM, out::versym->shdr.sh_addr);
  define(DT_VERNEED, out::verneed->shdr.sh_addr);
  define(DT_VERNEEDNUM, out::verneed->shdr.sh_info);
  define(DT_DEBUG, 0);

  auto find = [](std::string_view name) -> OutputChunk * {
    for (OutputChunk *chunk : out::chunks)
      if (chunk->name == name)
        return chunk;
    return nullptr;
  };

  if (OutputChunk *chunk = find(".init"))
    define(DT_INIT, chunk->shdr.sh_addr);
  if (OutputChunk *chunk = find(".fini"))
    define(DT_FINI, chunk->shdr.sh_addr);

  u32 flags = 0;
  u32 flags1 = 0;

  if (config.pie)
    flags1 |= DF_1_PIE;

  if (config.z_now) {
    flags |= DF_BIND_NOW;
    flags1 |= DF_1_NOW;
  }

  if (flags)
    define(DT_FLAGS, flags);
  if (flags1)
    define(DT_FLAGS_1, flags1);

  define(DT_NULL, 0);
  return vec;
}

void DynamicSection::update_shdr() {
  shdr.sh_size = create_dynamic_section().size() * 8;
  shdr.sh_link = out::dynstr->shndx;
}

void DynamicSection::copy_buf() {
  write_vector(out::buf + shdr.sh_offset, create_dynamic_section());
}

static std::string_view get_output_name(std::string_view name) {
  static std::string_view common_names[] = {
    ".text.", ".data.rel.ro.", ".data.", ".rodata.", ".bss.rel.ro.",
    ".bss.", ".init_array.", ".fini_array.", ".tbss.", ".tdata.",
  };

  for (std::string_view s1 : common_names) {
    std::string_view s2 = s1.substr(0, s1.size() - 1);
    if (name.starts_with(s1) || name == s2)
      return s2;
  }
  return name;
}

OutputSection *
OutputSection::get_instance(std::string_view name, u32 type, u64 flags) {
  if (name == ".eh_frame" && type == SHT_X86_64_UNWIND)
    type = SHT_PROGBITS;

  name = get_output_name(name);
  flags = flags & ~(u64)SHF_GROUP;

  auto find = [&]() -> OutputSection * {
    for (OutputSection *osec : OutputSection::instances)
      if (name == osec->name && type == osec->shdr.sh_type &&
          flags == (osec->shdr.sh_flags & ~SHF_GROUP))
        return osec;
    return nullptr;
  };

  // Search for an exiting output section.
  static std::shared_mutex mu;
  {
    std::shared_lock lock(mu);
    if (OutputSection *osec = find())
      return osec;
  }

  // Create a new output section.
  std::unique_lock lock(mu);
  if (OutputSection *osec = find())
    return osec;
  return new OutputSection(name, type, flags);
}

void OutputSection::copy_buf() {
  if (shdr.sh_type == SHT_NOBITS)
    return;

  int num_members = members.size();

  tbb::parallel_for(0, num_members, [&](int i) {
    if (members[i]->shdr.sh_type != SHT_NOBITS) {
      // Copy section contents to an output file
      members[i]->copy_buf();

      // Zero-clear trailing padding
      u64 this_end = members[i]->offset + members[i]->shdr.sh_size;
      u64 next_start = (i == num_members - 1) ? shdr.sh_size : members[i + 1]->offset;
      memset(out::buf + shdr.sh_offset + this_end, 0, next_start - this_end);
    }
  });
}

void GotSection::add_got_symbol(Symbol *sym) {
  assert(sym->got_idx == -1);
  sym->got_idx = shdr.sh_size / GOT_SIZE;
  shdr.sh_size += GOT_SIZE;
  got_syms.push_back(sym);
}

void GotSection::add_gottpoff_symbol(Symbol *sym) {
  assert(sym->gottpoff_idx == -1);
  sym->gottpoff_idx = shdr.sh_size / GOT_SIZE;
  shdr.sh_size += GOT_SIZE;
  gottpoff_syms.push_back(sym);
}

void GotSection::add_tlsgd_symbol(Symbol *sym) {
  assert(sym->tlsgd_idx == -1);
  sym->tlsgd_idx = shdr.sh_size / GOT_SIZE;
  shdr.sh_size += GOT_SIZE * 2;
  tlsgd_syms.push_back(sym);
}

void GotSection::add_tlsld() {
  if (tlsld_idx != -1)
    return;
  tlsld_idx = shdr.sh_size / GOT_SIZE;
  shdr.sh_size += GOT_SIZE * 2;
}

void GotSection::copy_buf() {
  u64 *buf = (u64 *)(out::buf + shdr.sh_offset);
  memset(buf, 0, shdr.sh_size);

  for (Symbol *sym : got_syms)
    if (!sym->is_imported)
      buf[sym->got_idx] = sym->get_addr();

  for (Symbol *sym : gottpoff_syms)
    if (!sym->is_imported)
      buf[sym->gottpoff_idx] = sym->get_addr() - out::tls_end;
}

void GotPltSection::copy_buf() {
  u64 *buf = (u64 *)(out::buf + shdr.sh_offset);

  buf[0] = out::dynamic ? out::dynamic->shdr.sh_addr : 0;
  buf[1] = 0;
  buf[2] = 0;

  for (Symbol *sym : out::plt->symbols)
    if (sym->gotplt_idx != -1)
      buf[sym->gotplt_idx] = sym->get_plt_addr() + 6;
}

void PltSection::add_symbol(Symbol *sym) {
  assert(sym->plt_idx == -1);
  sym->plt_idx = shdr.sh_size / PLT_SIZE;
  shdr.sh_size += PLT_SIZE;
  symbols.push_back(sym);

  if (sym->got_idx == -1) {
    sym->gotplt_idx = out::gotplt->shdr.sh_size / GOT_SIZE;
    out::gotplt->shdr.sh_size += GOT_SIZE;

    sym->has_relplt = true;
    out::relplt->shdr.sh_size += sizeof(ElfRela);

    out::dynsym->add_symbol(sym);
  }
}

void PltSection::copy_buf() {
  u8 *buf = out::buf + shdr.sh_offset;

  const u8 plt0[] = {
    0xff, 0x35, 0, 0, 0, 0, // pushq GOTPLT+8(%rip)
    0xff, 0x25, 0, 0, 0, 0, // jmp *GOTPLT+16(%rip)
    0x0f, 0x1f, 0x40, 0x00, // nop
  };

  memcpy(buf, plt0, sizeof(plt0));
  *(u32 *)(buf + 2) = out::gotplt->shdr.sh_addr - shdr.sh_addr + 2;
  *(u32 *)(buf + 8) = out::gotplt->shdr.sh_addr - shdr.sh_addr + 4;

  int relplt_idx = 0;

  for (Symbol *sym : symbols) {
    u8 *ent = buf + sym->plt_idx * PLT_SIZE;

    if (sym->gotplt_idx != -1) {
      const u8 data[] = {
        0xff, 0x25, 0, 0, 0, 0, // jmp   *foo@GOTPLT
        0x68, 0,    0, 0, 0,    // push  $index_in_relplt
        0xe9, 0,    0, 0, 0,    // jmp   PLT[0]
      };

      memcpy(ent, data, sizeof(data));
      *(u32 *)(ent + 2) = sym->get_gotplt_addr() - sym->get_plt_addr() - 6;
      *(u32 *)(ent + 7) = relplt_idx++;
      *(u32 *)(ent + 12) = shdr.sh_addr - sym->get_plt_addr() - 16;
    } else {
      const u8 data[] = {
        0xff, 0x25, 0,    0,    0,    0,                   // jmp   *foo@GOT
        0x66, 0x66, 0x66, 0x0f, 0x1f, 0x84, 0, 0, 0, 0, 0, // nop
      };

      memcpy(ent, data, sizeof(data));
      *(u32 *)(ent + 2) = sym->get_got_addr() - sym->get_plt_addr() - 6;
    }
  }
}

void RelPltSection::update_shdr() {
  shdr.sh_link = out::dynsym->shndx;
}

void RelPltSection::copy_buf() {
  ElfRela *buf = (ElfRela *)(out::buf + shdr.sh_offset);
  memset(buf, 0, shdr.sh_size);

  int relplt_idx = 0;

  for (Symbol *sym : out::plt->symbols) {
    if (!sym->has_relplt)
      continue;

    ElfRela &rel = buf[relplt_idx++];
    memset(&rel, 0, sizeof(rel));
    rel.r_sym = sym->dynsym_idx;
    rel.r_offset = sym->get_gotplt_addr();

    if (sym->st_type == STT_GNU_IFUNC) {
      rel.r_type = R_X86_64_IRELATIVE;
      rel.r_addend = sym->get_addr();
    } else {
      rel.r_type = R_X86_64_JUMP_SLOT;
    }
  }
}

void DynsymSection::add_symbol(Symbol *sym) {
  if (sym->dynsym_idx != -1)
    return;
  sym->dynsym_idx = -2;
  symbols.push_back(sym);
  name_indices.push_back(out::dynstr->add_string(sym->name));
}

void DynsymSection::sort_symbols() {
  auto first_global = std::stable_partition(
    symbols.begin(), symbols.end(),
    [](Symbol *sym) { return sym->esym->st_bind == STB_LOCAL; });

  shdr.sh_info = first_global - symbols.begin() + 1;

  int i = 1;
  for (Symbol *sym : symbols)
    sym->dynsym_idx = i++;
}

void DynsymSection::update_shdr() {
  shdr.sh_link = out::dynstr->shndx;
  shdr.sh_size = sizeof(ElfSym) * (symbols.size() + 1);
}

void DynsymSection::copy_buf() {
  u8 *base = out::buf + shdr.sh_offset;
  memset(base, 0, sizeof(ElfSym));

  for (int i = 0; i < symbols.size(); i++) {
    Symbol &sym = *symbols[i];

    ElfSym &esym = *(ElfSym *)(base + sym.dynsym_idx * sizeof(ElfSym));
    memset(&esym, 0, sizeof(esym));
    esym.st_name = name_indices[i];
    esym.st_type = sym.st_type;
    esym.st_bind = sym.esym->st_bind;
    esym.st_size = sym.esym->st_size;

    if (sym.has_copyrel) {
      esym.st_shndx = out::copyrel->shndx;
      esym.st_value = sym.get_addr();
    } else if (sym.is_imported || sym.esym->is_undef()) {
      esym.st_shndx = SHN_UNDEF;
    } else if (!sym.input_section) {
      esym.st_shndx = SHN_ABS;
      esym.st_value = sym.get_addr();
    } else if (sym.st_type == STT_TLS) {
      esym.st_shndx = sym.input_section->output_section->shndx;
      esym.st_value = sym.get_addr() - out::tls_begin;
    } else {
      esym.st_shndx = sym.input_section->output_section->shndx;
      esym.st_value = sym.get_addr();
    }
  }
}

void HashSection::update_shdr() {
  int header_size = 8;
  int num_slots = out::dynsym->symbols.size() + 1;
  shdr.sh_size = header_size + num_slots * 8;
  shdr.sh_link = out::dynsym->shndx;
}

void HashSection::copy_buf() {
  u8 *base = out::buf + shdr.sh_offset;
  memset(base, 0, shdr.sh_size);

  int num_slots = out::dynsym->symbols.size() + 1;
  u32 *hdr = (u32 *)base;
  u32 *buckets = (u32 *)(base + 8);
  u32 *chains = buckets + num_slots;

  hdr[0] = hdr[1] = num_slots;

  for (Symbol *sym : out::dynsym->symbols) {
    u32 i = elf_hash(sym->name) % num_slots;
    chains[sym->dynsym_idx] = buckets[i];
    buckets[i] = sym->dynsym_idx;
  }
}

MergedSection *
MergedSection::get_instance(std::string_view name, u32 type, u64 flags) {
  name = get_output_name(name);
  flags = flags & ~(u64)SHF_MERGE & ~(u64)SHF_STRINGS;

  auto find = [&]() -> MergedSection * {
    for (MergedSection *osec : MergedSection::instances)
      if (name == osec->name && flags == osec->shdr.sh_flags &&
          type == osec->shdr.sh_type)
        return osec;
    return nullptr;
  };

  // Search for an exiting output section.
  static std::shared_mutex mu;
  {
    std::shared_lock lock(mu);
    if (MergedSection *osec = find())
      return osec;
  }

  // Create a new output section.
  std::unique_lock lock(mu);
  if (MergedSection *osec = find())
    return osec;

  auto *osec = new MergedSection(name, flags, type);
  MergedSection::instances.push_back(osec);
  return osec;
}

void MergedSection::copy_buf() {
  u8 *base = out::buf + shdr.sh_offset;

  map.for_each_value([&](const StringPiece &piece) {
    if (MergeableSection *m = piece.isec)
      memcpy(base + m->offset + piece.output_offset, piece.data, piece.size);
  });
}

void EhFrameSection::finalize_contents() {
  for (int i = 0; i < members.size(); i++) {
    InputSection &isec = *members[i];
    if (isec.shdr.sh_type == SHT_NOBITS || isec.shdr.sh_size == 0)
      return;

    contents[i].resize(isec.shdr.sh_size);
    u8 *buf = contents[i].data();

    isec.copy_contents(buf);
    isec.apply_reloc_alloc(buf);
  }
}

void EhFrameSection::copy_buf() {
  u8 *base = out::buf + shdr.sh_offset;
  u64 offset = 0;

  for (std::span<u8> buf : contents) {
    memcpy(base + offset, buf.data(), buf.size());
    offset += contents.size();
  }
}

void CopyrelSection::add_symbol(Symbol *sym) {
  assert(sym->is_imported);
  if (sym->has_copyrel)
    return;

  shdr.sh_size = align_to(shdr.sh_size, shdr.sh_addralign);
  sym->value = shdr.sh_size;
  sym->has_copyrel = true;
  shdr.sh_size += sym->esym->st_size;
  symbols.push_back(sym);
  out::dynsym->add_symbol(sym);
}

void VersymSection::update_shdr() {
  shdr.sh_size = contents.size() * sizeof(contents[0]);
  shdr.sh_link = out::dynsym->shndx;
}

void VersymSection::copy_buf() {
  write_vector(out::buf + shdr.sh_offset, contents);
}

void VerneedSection::update_shdr() {
  shdr.sh_size = contents.size() * sizeof(contents[0]);
  shdr.sh_link = out::dynstr->shndx;
}

void VerneedSection::copy_buf() {
  write_vector(out::buf + shdr.sh_offset, contents);
}

void BuildIdSection::copy_buf() {
  u32 *base = (u32 *)(out::buf + shdr.sh_offset);
  memset(base, 0, shdr.sh_size);
  base[0] = 4;                // Name size
  base[1] = SHA256_SIZE;      // Hash size
  base[2] = NT_GNU_BUILD_ID;  // Type
  memcpy(base + 3, "GNU", 4); // Name string
}

void BuildIdSection::write_buildid(u64 filesize) {
  Timer t("build_id");

  int shard_size = 1024 * 1024;
  int num_shards = filesize / shard_size + 1;
  u8 shards[num_shards][SHA256_SIZE];

  tbb::parallel_for(0, num_shards, [&](int i) {
    u8 *begin = out::buf + shard_size * i;
    u64 size = (i < num_shards - 1) ? shard_size : (filesize % shard_size);
    SHA256(begin, size, shards[i]);
  });

  SHA256((u8 *)shards, sizeof(shards), out::buf + shdr.sh_offset + 16);
}
