//===- Writer.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Chunks.h"
#include "Config.h"
#include "Error.h"
#include "SymbolTable.h"
#include "Writer.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/FileOutputBuffer.h"

using namespace llvm;
using namespace llvm::ELF;
using namespace llvm::object;

using namespace lld;
using namespace lld::elf2;

static const int PageSize = 4096;

namespace {
// OutputSection represents a section in an output file. It's a
// container of chunks. OutputSection and Chunk are 1:N relationship.
// Chunks cannot belong to more than one OutputSections. The writer
// creates multiple OutputSections and assign them unique,
// non-overlapping file offsets and VAs.
template <class ELFT> class OutputSection {
public:
  typedef typename llvm::object::ELFFile<ELFT>::uintX_t uintX_t;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Shdr Elf_Shdr;

  OutputSection(StringRef Name) : Name(Name), Header({}) {}
  void setVA(uintX_t);
  void setFileOffset(uintX_t);
  void addSectionChunk(SectionChunk<ELFT> *C);
  std::vector<Chunk *> &getChunks() { return Chunks; }
  void writeHeaderTo(Elf_Shdr *SHdr);

  // Returns the size of the section in the output file.
  uintX_t getSize() { return Header.sh_size; }

private:
  StringRef Name;
  Elf_Shdr Header;
  std::vector<Chunk *> Chunks;
};

// The writer writes a SymbolTable result to a file.
template <class ELFT> class Writer {
public:
  typedef typename llvm::object::ELFFile<ELFT>::uintX_t uintX_t;
  Writer(SymbolTable *T) : Symtab(T) {}
  void run();

private:
  void createSections();
  void assignAddresses();
  void openFile(StringRef OutputPath);
  void writeHeader();
  void writeSections();

  SymbolTable *Symtab;
  std::unique_ptr<llvm::FileOutputBuffer> Buffer;
  llvm::SpecificBumpPtrAllocator<OutputSection<ELFT>> CAlloc;
  std::vector<OutputSection<ELFT> *> OutputSections;

  uintX_t FileSize;
  uintX_t SizeOfHeaders;
  uintX_t SectionHeaderOff;

  std::vector<std::unique_ptr<Chunk>> Chunks;
};
} // anonymous namespace

namespace lld {
namespace elf2 {

template <class ELFT>
void writeResult(SymbolTable *Symtab) { Writer<ELFT>(Symtab).run(); }

template void writeResult<ELF32LE>(SymbolTable *);
template void writeResult<ELF32BE>(SymbolTable *);
template void writeResult<ELF64LE>(SymbolTable *);
template void writeResult<ELF64BE>(SymbolTable *);

} // namespace elf2
} // namespace lld

// The main function of the writer.
template <class ELFT> void Writer<ELFT>::run() {
  createSections();
  assignAddresses();
  openFile(Config->OutputFile);
  writeHeader();
  writeSections();
  error(Buffer->commit());
}

template <class ELFT> void OutputSection<ELFT>::setVA(uintX_t VA) {
  Header.sh_addr = VA;
  for (Chunk *C : Chunks)
    C->setVA(C->getVA() + VA);
}

template <class ELFT> void OutputSection<ELFT>::setFileOffset(uintX_t Off) {
  if (Header.sh_size == 0)
    return;
  Header.sh_offset = Off;
  for (Chunk *C : Chunks)
    C->setFileOff(C->getFileOff() + Off);
}

template <class ELFT>
void OutputSection<ELFT>::addSectionChunk(SectionChunk<ELFT> *C) {
  Chunks.push_back(C);
  uintX_t Off = Header.sh_size;
  Off = RoundUpToAlignment(Off, C->getAlign());
  C->setVA(Off);
  C->setFileOff(Off);
  Off += C->getSize();
  Header.sh_size = Off;
  Header.sh_type = C->getSectionHdr()->sh_type;
  Header.sh_flags |= C->getSectionHdr()->sh_flags;
}

template <class ELFT> void OutputSection<ELFT>::writeHeaderTo(Elf_Shdr *SHdr) {
  *SHdr = Header;
}

// Create output section objects and add them to OutputSections.
template <class ELFT> void Writer<ELFT>::createSections() {
  SmallDenseMap<StringRef, OutputSection<ELFT> *> Map;
  for (std::unique_ptr<ObjectFileBase> &FileB : Symtab->ObjectFiles) {
    auto &File = cast<ObjectFile<ELFT>>(*FileB);
    for (SectionChunk<ELFT> *C : File.getChunks()) {
      OutputSection<ELFT> *&Sec = Map[C->getSectionName()];
      if (!Sec) {
        Sec = new (CAlloc.Allocate()) OutputSection<ELFT>(C->getSectionName());
        OutputSections.push_back(Sec);
      }
      Sec->addSectionChunk(C);
    }
  }
}

// Visits all sections to assign incremental, non-overlapping RVAs and
// file offsets.
template <class ELFT> void Writer<ELFT>::assignAddresses() {
  SizeOfHeaders = RoundUpToAlignment(sizeof(Elf_Ehdr_Impl<ELFT>), PageSize);
  uintX_t VA = 0x1000; // The first page is kept unmapped.
  uintX_t FileOff = SizeOfHeaders;
  for (OutputSection<ELFT> *Sec : OutputSections) {
    Sec->setVA(VA);
    Sec->setFileOffset(FileOff);
    VA += RoundUpToAlignment(Sec->getSize(), PageSize);
    FileOff += RoundUpToAlignment(Sec->getSize(), 8);
  }
  // Add space for section headers.
  SectionHeaderOff = FileOff;
  FileOff += (OutputSections.size() + 1) * sizeof(Elf_Shdr_Impl<ELFT>);
  FileSize = SizeOfHeaders + RoundUpToAlignment(FileOff - SizeOfHeaders, 8);
}

template <class ELFT> void Writer<ELFT>::writeHeader() {
  uint8_t *Buf = Buffer->getBufferStart();
  auto *EHdr = reinterpret_cast<Elf_Ehdr_Impl<ELFT> *>(Buf);
  EHdr->e_ident[EI_MAG0] = 0x7F;
  EHdr->e_ident[EI_MAG1] = 0x45;
  EHdr->e_ident[EI_MAG2] = 0x4C;
  EHdr->e_ident[EI_MAG3] = 0x46;
  EHdr->e_ident[EI_CLASS] = ELFT::Is64Bits ? ELFCLASS64 : ELFCLASS32;
  EHdr->e_ident[EI_DATA] = ELFT::TargetEndianness == llvm::support::little
                               ? ELFDATA2LSB
                               : ELFDATA2MSB;
  EHdr->e_ident[EI_VERSION] = EV_CURRENT;
  EHdr->e_ident[EI_OSABI] = ELFOSABI_NONE;

  EHdr->e_type = ET_EXEC;
  auto &FirstObj = cast<ObjectFile<ELFT>>(*Symtab->ObjectFiles[0]);
  EHdr->e_machine = FirstObj.getObj()->getHeader()->e_machine;
  EHdr->e_version = EV_CURRENT;
  EHdr->e_entry = 0x401000;
  EHdr->e_phoff = sizeof(Elf_Ehdr_Impl<ELFT>);
  EHdr->e_shoff = SectionHeaderOff;
  EHdr->e_ehsize = sizeof(Elf_Ehdr_Impl<ELFT>);
  EHdr->e_phentsize = sizeof(Elf_Phdr_Impl<ELFT>);
  EHdr->e_phnum = 1;
  EHdr->e_shentsize = sizeof(Elf_Shdr_Impl<ELFT>);
  EHdr->e_shnum = OutputSections.size() + 1;
  EHdr->e_shstrndx = 0;

  auto PHdrs = reinterpret_cast<Elf_Phdr_Impl<ELFT> *>(Buf + EHdr->e_phoff);
  PHdrs->p_type = PT_LOAD;
  PHdrs->p_flags = PF_R | PF_X;
  PHdrs->p_offset = 0x0000;
  PHdrs->p_vaddr = 0x400000;
  PHdrs->p_paddr = PHdrs->p_vaddr;
  PHdrs->p_filesz = FileSize;
  PHdrs->p_memsz = FileSize;
  PHdrs->p_align = 0x4000;

  auto SHdrs = reinterpret_cast<Elf_Shdr_Impl<ELFT> *>(Buf + EHdr->e_shoff);
  // First entry is null.
  ++SHdrs;
  for (OutputSection<ELFT> *Sec : OutputSections)
    Sec->writeHeaderTo(SHdrs++);
}

template <class ELFT> void Writer<ELFT>::openFile(StringRef Path) {
  std::error_code EC = FileOutputBuffer::create(Path, FileSize, Buffer,
                                                FileOutputBuffer::F_executable);
  error(EC, Twine("failed to open ") + Path);
}

// Write section contents to a mmap'ed file.
template <class ELFT> void Writer<ELFT>::writeSections() {
  uint8_t *Buf = Buffer->getBufferStart();
  for (OutputSection<ELFT> *Sec : OutputSections) {
    for (Chunk *C : Sec->getChunks())
      C->writeTo(Buf);
  }
}
