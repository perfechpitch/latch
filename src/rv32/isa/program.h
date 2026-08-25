#ifndef _LATCH_PROGRAM_BINARY_
#define _LATCH_PROGRAM_BINARY_

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "base/log.h"
#include "isa/memory.h"

namespace latch {

using namespace latch;
using systeml::MemoryBase;

class Instruction;

class InstBinary {
 public:
  explicit InstBinary(uint32_t inst) : InstL(inst), InstH(0), Length(4) {}
  InstBinary(const uint8_t* dataPtr, uint32_t bytes) : InstL(0), InstH(0), Length(bytes) {
    uint32_t bs = 0;
    for (uint8_t i = 0; i < 8 && bs < bytes;) {
      InstL = InstL | (((uint64_t)(*(dataPtr + bs))) << (i * 8));
      bs++;
      i++;
    }
    for (uint8_t i = 0; i < 8 && bs < bytes;) {
      InstH = InstH | (((uint64_t)(*(dataPtr + bs))) << (i * 8));
      bs++;
      i++;
    }
  }

  InstBinary(std::vector<std::pair<uint64_t, uint64_t>> const& widthValFieldsVec, uint64_t instBitWidth) {
    uint64_t startBit = 0;
    for (auto const& widthVal : widthValFieldsVec) {
      uint64_t width = widthVal.first;
      uint64_t val = widthVal.second;
      if (startBit < 64) {
        if (startBit + width <= 64) {
          InstL = InstL | (val << startBit);
        } else {
          InstL = InstL | (val << startBit);
          InstH = InstH | (val >> (64 - startBit));
        }
      } else {
        LOGCHECK(startBit + width <= 128, "");
        InstH = InstH | (val << (startBit - 64));
      }
      startBit = startBit + width;
    }
    LOGCHECK(startBit == instBitWidth, "");
    LOGCHECK(instBitWidth % 8 == 0, "");
    Length = instBitWidth / 8;
  }

  bool IsBitEq(uint64_t bitLoc, uint32_t bitVal) {
    LOGCHECK((bitVal == 0 || bitVal == 1), "Unsupport value!");
    LOGCHECK(bitLoc < 8 * Length, "Unsupport range!");
    if (bitLoc < 64)
      return ((InstL >> (bitLoc % 64)) & 0x01) == bitVal;
    else
      return ((InstH >> (bitLoc % 64)) & 0x01) == bitVal;
  }

  InstBinary GetSubBinary(uint32_t startByte, uint32_t bytes) const {
    if (startByte == 0 && bytes == 16) {
      return InstBinary(InstL, InstH, Length);
    }

    // todo fixme: for bytes > 8, only support startByte=0, bytes=16

    LOGCHECK(bytes <= 8, "Unsupport Length!");
    LOGCHECK(bytes <= Length, "Unsupport Length!");
    LOGCHECK(startByte + bytes <= Length, "Unsupport range!");

    uint64_t instBin = 0;
    uint32_t index = startByte;
    for (uint32_t i = 0; i < bytes; i++) {
      if (index > 7)
        instBin = instBin | (((InstH >> ((index - 8) * 8)) & 0xff) << (i * 8));
      else
        instBin = instBin | (((InstL >> (index * 8)) & 0xff) << (i * 8));
      index++;
    }
    return InstBinary(instBin, 0, bytes);
  }

  uint32_t GetBinary32() { return InstL & 0xffffffff; }
  uint64_t GetBinary64() { return InstL; }

  bool BitEqual(uint8_t begin, uint8_t width, uint64_t bit) const {
    LOGCHECK(width <= 64, "");
    uint64_t raw = 0;
    uint8_t newBegin = 0;
    if (begin == 0) {
      raw = InstL;
    } else if (begin >= 64) {
      raw = InstH >> (begin - 64);
    } else {
      raw = InstL >> begin;
      raw = raw | (InstH << (64 - begin));
    }

    uint64_t lhs = raw;
    uint64_t rhs = bit;
    uint64_t mask = 0;
    for (uint8_t i = 0; i < width; i++) {
      mask = mask | ((uint64_t)1 << (i + newBegin));
    }
    lhs = lhs & mask;
    if (lhs == rhs) {
      return true;
    } else {
      return false;
    }
  }

  uint64_t SliceBit(uint8_t begin, uint8_t width) const {
    LOGCHECK(width <= 64, "");
    uint64_t raw = 0;
    uint8_t newBegin = 0;
    if (begin == 0) {
      raw = InstL;
    } else if (begin >= 64) {
      raw = InstH >> (begin - 64);
    } else {
      raw = InstL >> begin;
      raw = raw | (InstH << (64 - begin));
    }

    uint64_t lhs = raw;
    uint64_t mask = 0;
    for (uint8_t i = 0; i < width; i++) {
      mask = mask | ((uint64_t)1 << (i + newBegin));
    }
    lhs = lhs & mask;
    return lhs;
  }

  std::vector<uint8_t> Convert2Vec() const {
    std::vector<uint8_t> res(Length);
    uint64_t mask = 0xff;
    for (uint8_t i = 0; i < Length; i++) {
      if (i < 8) {
        res.at(i) = (mask & (InstL >> (i * 8)));
      } else {
        res.at(i) = (mask & (InstH >> ((i - 8) * 8)));
      }
    }
    return res;
  }

  std::string ToStr() { return "L: " + std::to_string(InstL) + " H: " + std::to_string(InstH); }

 private:
  InstBinary(uint64_t instL, uint64_t instH, uint64_t len) {
    InstL = instL;
    InstH = instH;
    Length = len;
  }

  uint64_t InstL = 0;
  uint64_t InstH = 0;
  uint64_t Length = 0;
};


#define EI_NIDENT 16
#define PT_LOAD 1
#define SHT_STRTAB 3

typedef struct {
  unsigned char e_ident[EI_NIDENT];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint32_t e_entry;
  uint32_t e_phoff;
  uint32_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
  uint32_t p_type;
  uint32_t p_offset;
  uint32_t p_vaddr;
  uint32_t p_paddr;
  uint32_t p_filesz;
  uint32_t p_memsz;
  uint32_t p_flags;
  uint32_t p_align;
} Elf32_Phdr;

typedef struct {
  uint32_t sh_name;
  uint32_t sh_type;
  uint32_t sh_flags;
  uint32_t sh_addr;
  uint32_t sh_offset;
  uint32_t sh_size;
  uint32_t sh_link;
  uint32_t sh_info;
  uint32_t sh_addralign;
  uint32_t sh_entsize;
} Elf32_Shdr;

typedef struct {
  uint32_t st_name;
  uint32_t st_value;
  uint32_t st_size;
  uint8_t  st_info;
  uint8_t  st_other;
  uint16_t st_shndx;
} Elf32_Sym;

typedef struct {
  uint32_t st_name;
  uint8_t  st_info;
  uint8_t  st_other;
  uint16_t st_shndx;
  uint64_t st_value;
  uint64_t st_size;
} Elf64_Sym;

typedef struct {
  unsigned char e_ident[EI_NIDENT];
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

typedef struct {
  uint32_t sh_name;
  uint32_t sh_type;
  uint64_t sh_flags;
  uint64_t sh_addr;
  uint64_t sh_offset;
  uint64_t sh_size;
  uint32_t sh_link;
  uint32_t sh_info;
  uint64_t sh_addralign;
  uint64_t sh_entsize;
} Elf64_Shdr;

typedef struct {
  uint64_t sh_addr;
  uint64_t sh_offset;
  uint64_t sh_size;
} Elf_To_Memory;

class ElfParser {
 private:
  std::ifstream file;
  bool verbose;
  bool is64Bit;
  bool isLittleEndian;
  uint64_t startAddr = 0;

  uint64_t phoff = 0;
  uint64_t shoff = 0;
  uint16_t phentsize = 0;
  uint16_t phnum = 0;
  uint16_t shentsize = 0;
  uint16_t shnum = 0;
  uint16_t shstrndx = 0;

  template <typename T>
  bool readData(uint64_t offset, T& data) {
    file.seekg(offset);
    file.read(reinterpret_cast<char*>(&data), sizeof(T));
    return !file.fail();
  }

  uint16_t adjustEndian16(uint16_t value) {
    if (!isLittleEndian) {
      return (value >> 8) | (value << 8);
    }
    return value;
  }

  uint32_t adjustEndian32(uint32_t value) {
    if (!isLittleEndian) {
      return ((value >> 24) & 0x000000FF) | ((value >> 8) & 0x0000FF00) | ((value << 8) & 0x00FF0000) |
             ((value << 24) & 0xFF000000);
    }
    return value;
  }

  uint64_t adjustEndian64(uint64_t value) {
    if (!isLittleEndian) {
      return ((value >> 56) & 0x00000000000000FF) | ((value >> 40) & 0x000000000000FF00) |
             ((value >> 24) & 0x0000000000FF0000) | ((value >> 8) & 0x00000000FF000000) |
             ((value << 8) & 0x000000FF00000000) | ((value << 24) & 0x0000FF0000000000) |
             ((value << 40) & 0x00FF000000000000) | ((value << 56) & 0xFF00000000000000);
    }
    return value;
  }

 public:
  struct LoadSegment {
    uint64_t vaddr;
    uint64_t offset;
    uint64_t filesz;
    uint64_t memsz;  // >filesz 的部分是 .bss，需清零
  };

  bool parseHeader() {
    if (!file.is_open()) return false;

    unsigned char e_ident[EI_NIDENT];
    if (!readData(0, e_ident)) return false;

    if (e_ident[0] != 0x7F || e_ident[1] != 'E' || e_ident[2] != 'L' || e_ident[3] != 'F') {
      std::cerr << "不是有效的ELF文件" << std::endl;
      return false;
    }

    if (e_ident[4] == 1) {
      is64Bit = false;
    } else if (e_ident[4] == 2) {
      is64Bit = true;
    } else {
      std::cerr << "不支持的ELF位数" << std::endl;
      return false;
    }

    if (e_ident[5] == 1) {
      isLittleEndian = true;
    } else if (e_ident[5] == 2) {
      isLittleEndian = false;
    } else {
      std::cerr << "不支持的字节序" << std::endl;
      return false;
    }

    if (is64Bit) {
      Elf64_Ehdr ehdr;
      if (!readData(0, ehdr)) return false;
      startAddr = adjustEndian64(ehdr.e_entry);
      phoff = adjustEndian64(ehdr.e_phoff);
      shoff = adjustEndian64(ehdr.e_shoff);
      phentsize = adjustEndian16(ehdr.e_phentsize);
      phnum = adjustEndian16(ehdr.e_phnum);
      shentsize = adjustEndian16(ehdr.e_shentsize);
      shnum = adjustEndian16(ehdr.e_shnum);
      shstrndx = adjustEndian16(ehdr.e_shstrndx);
    } else {
      Elf32_Ehdr ehdr;
      if (!readData(0, ehdr)) return false;
      startAddr = adjustEndian32(ehdr.e_entry);
      phoff = adjustEndian32(ehdr.e_phoff);
      shoff = adjustEndian32(ehdr.e_shoff);
      phentsize = adjustEndian16(ehdr.e_phentsize);
      phnum = adjustEndian16(ehdr.e_phnum);
      shentsize = adjustEndian16(ehdr.e_shentsize);
      shnum = adjustEndian16(ehdr.e_shnum);
      shstrndx = adjustEndian16(ehdr.e_shstrndx);
    }

    if (verbose) {
      std::cout << "ELF类型: " << (is64Bit ? "64位" : "32位") << (isLittleEndian ? "  小端" : "  大端");
      std::cout << "  入口: 0x" << std::hex << startAddr << std::dec;
      std::cout << "  程序头表数: " << phnum << "  节头表数: " << shnum << std::endl;
    }
    return true;
  }

  bool parseProgramHeaders() {
    if (!file.is_open()) return false;

    if (verbose) std::cout << "\n===== 程序头表（段） =====" << std::endl;
    for (int i = 0; i < phnum; ++i) {
      uint32_t type;
      uint64_t vaddr, offset, filesz, memsz;
      if (is64Bit) {
        Elf64_Phdr phdr;
        if (!readData(phoff + i * phentsize, phdr)) return false;
        type = adjustEndian32(phdr.p_type);
        vaddr = adjustEndian64(phdr.p_vaddr);
        offset = adjustEndian64(phdr.p_offset);
        filesz = adjustEndian64(phdr.p_filesz);
        memsz = adjustEndian64(phdr.p_memsz);
      } else {
        Elf32_Phdr phdr;
        if (!readData(phoff + i * phentsize, phdr)) return false;
        type = adjustEndian32(phdr.p_type);
        vaddr = adjustEndian32(phdr.p_vaddr);
        offset = adjustEndian32(phdr.p_offset);
        filesz = adjustEndian32(phdr.p_filesz);
        memsz = adjustEndian32(phdr.p_memsz);
      }
      if (verbose) {
        std::cout << "段 " << i << ": 类型 0x" << std::hex << type << std::dec
                  << (type == PT_LOAD ? "（可加载段）" : "") << "  虚拟地址: 0x" << std::hex << vaddr
                  << "  文件偏移: 0x" << offset << std::dec << "  文件大小: " << filesz << "  内存大小: " << memsz
                  << std::endl;
      }
    }
    return true;
  }

  std::vector<LoadSegment> parseLoadSegments() {
    std::vector<LoadSegment> segs;
    if (!file.is_open()) return segs;

    for (int i = 0; i < phnum; ++i) {
      if (is64Bit) {
        Elf64_Phdr phdr;
        if (!readData(phoff + i * phentsize, phdr)) return segs;
        if (adjustEndian32(phdr.p_type) != PT_LOAD) continue;
        segs.push_back({adjustEndian64(phdr.p_vaddr), adjustEndian64(phdr.p_offset), adjustEndian64(phdr.p_filesz),
                        adjustEndian64(phdr.p_memsz)});
      } else {
        Elf32_Phdr phdr;
        if (!readData(phoff + i * phentsize, phdr)) return segs;
        if (adjustEndian32(phdr.p_type) != PT_LOAD) continue;
        segs.push_back({adjustEndian32(phdr.p_vaddr), adjustEndian32(phdr.p_offset), adjustEndian32(phdr.p_filesz),
                        adjustEndian32(phdr.p_memsz)});
      }
    }
    return segs;
  }

  std::vector<Elf_To_Memory> parseSectionHeaders() {
    std::vector<Elf_To_Memory> mem_dump;
    if (!file.is_open()) return mem_dump;

    uint64_t strOffset = 0, strSize = 0;
    if (is64Bit) {
      Elf64_Shdr strShdr;
      if (!readData(shoff + shstrndx * shentsize, strShdr)) return mem_dump;
      strOffset = adjustEndian64(strShdr.sh_offset);
      strSize = adjustEndian64(strShdr.sh_size);
    } else {
      Elf32_Shdr strShdr;
      if (!readData(shoff + shstrndx * shentsize, strShdr)) return mem_dump;
      strOffset = adjustEndian32(strShdr.sh_offset);
      strSize = adjustEndian32(strShdr.sh_size);
    }
    std::vector<char> strTable(strSize);
    file.seekg(strOffset);
    file.read(strTable.data(), strSize);

    for (int i = 0; i < shnum; ++i) {
      uint32_t type, nameOff;
      uint64_t addr, offset, size;
      if (is64Bit) {
        Elf64_Shdr shdr;
        if (!readData(shoff + i * shentsize, shdr)) return mem_dump;
        nameOff = adjustEndian32(shdr.sh_name);
        type = adjustEndian32(shdr.sh_type);
        addr = adjustEndian64(shdr.sh_addr);
        offset = adjustEndian64(shdr.sh_offset);
        size = adjustEndian64(shdr.sh_size);
      } else {
        Elf32_Shdr shdr;
        if (!readData(shoff + i * shentsize, shdr)) return mem_dump;
        nameOff = adjustEndian32(shdr.sh_name);
        type = adjustEndian32(shdr.sh_type);
        addr = adjustEndian32(shdr.sh_addr);
        offset = adjustEndian32(shdr.sh_offset);
        size = adjustEndian32(shdr.sh_size);
      }

      const char* name = (nameOff < strSize) ? &strTable[nameOff] : "未知";
      if (verbose) {
        std::cout << "节 " << i << ": " << name << "  类型: 0x" << std::hex << type << std::dec << "  地址: 0x"
                  << std::hex << addr << "  偏移: 0x" << offset << std::dec << "  大小: " << size << " 字节"
                  << std::endl;
      }
      mem_dump.push_back({addr, offset, size});
    }
    return mem_dump;
  }

  // 按名字查符号地址(SHT_SYMTAB)。找到返回 true 并写 addr。
  //   用处:单 op 隔离要从 `cpu.elf` 里某个 `op_NNN_xxx` 函数进入,而不是从 `_start`
  //   (整份 bundle 的 `main` 顺序调用全部 op,想只跑一个就得直接落在它的入口上)。
  bool FindSymbol(const std::string& want, uint64_t& addr) {
    if (!file.is_open()) return false;
    for (int i = 0; i < shnum; ++i) {
      uint32_t type = 0, link = 0;
      uint64_t off = 0, size = 0, entsize = 0;
      if (is64Bit) {
        Elf64_Shdr sh;
        if (!readData(shoff + i * shentsize, sh)) return false;
        type = adjustEndian32(sh.sh_type); link = adjustEndian32(sh.sh_link);
        off = adjustEndian64(sh.sh_offset); size = adjustEndian64(sh.sh_size);
        entsize = adjustEndian64(sh.sh_entsize);
      } else {
        Elf32_Shdr sh;
        if (!readData(shoff + i * shentsize, sh)) return false;
        type = adjustEndian32(sh.sh_type); link = adjustEndian32(sh.sh_link);
        off = adjustEndian32(sh.sh_offset); size = adjustEndian32(sh.sh_size);
        entsize = adjustEndian32(sh.sh_entsize);
      }
      if (type != 2 /*SHT_SYMTAB*/ || entsize == 0) continue;
      // 符号名在 sh_link 指的那个字符串表里。
      uint64_t strOff = 0, strSz = 0;
      if (is64Bit) {
        Elf64_Shdr sh;
        if (!readData(shoff + link * shentsize, sh)) return false;
        strOff = adjustEndian64(sh.sh_offset); strSz = adjustEndian64(sh.sh_size);
      } else {
        Elf32_Shdr sh;
        if (!readData(shoff + link * shentsize, sh)) return false;
        strOff = adjustEndian32(sh.sh_offset); strSz = adjustEndian32(sh.sh_size);
      }
      std::vector<char> strTab(strSz + 1, 0);
      file.seekg(strOff);
      file.read(strTab.data(), strSz);
      for (uint64_t k = 0; k * entsize < size; ++k) {
        uint32_t nameOff = 0;
        uint64_t value = 0;
        if (is64Bit) {
          Elf64_Sym sym;
          if (!readData(off + k * entsize, sym)) return false;
          nameOff = adjustEndian32(sym.st_name); value = adjustEndian64(sym.st_value);
        } else {
          Elf32_Sym sym;
          if (!readData(off + k * entsize, sym)) return false;
          nameOff = adjustEndian32(sym.st_name); value = adjustEndian32(sym.st_value);
        }
        if (nameOff >= strSz) continue;
        if (want == &strTab[nameOff]) { addr = value; return true; }
      }
    }
    return false;
  }

  explicit ElfParser(const std::string& filename, bool verboseLog = false) : verbose(verboseLog) {
    file.open(filename, std::ios::binary);
    parseHeader();
  }

  ~ElfParser() {
    if (file.is_open()) {
      file.close();
    }
  }

  void SetVerbose(bool v) { verbose = v; }

  uint64_t GetStartAddr() { return startAddr; }

  int DumpToMemory(std::shared_ptr<MemoryBase> memorySystem) {
    auto segs = parseLoadSegments();
    for (auto& s : segs) {
      if (s.filesz > 0) {
        std::vector<uint8_t> buf(s.filesz);
        file.seekg(s.offset);
        file.read(reinterpret_cast<char*>(buf.data()), s.filesz);
        memorySystem->Write(buf.data(), s.vaddr, s.filesz);
      }
      if (s.memsz > s.filesz) {
        std::vector<uint8_t> zeros(s.memsz - s.filesz, 0);
        memorySystem->Write(zeros.data(), s.vaddr + s.filesz, s.memsz - s.filesz);
      }
      if (verbose) {
        std::cout << "Load segment -> addr: 0x" << std::hex << s.vaddr << std::dec << "  filesz: " << s.filesz
                  << "  memsz: " << s.memsz << std::endl;
      }
    }
    return 0;
  }
};


enum class RecordType {
  DATA = 0x00,
  END_OF_FILE = 0x01,
  EXTENDED_SEGMENT_ADDRESS = 0x02,
  START_SEGMENT_ADDRESS = 0x03,
  EXTENDED_LINEAR_ADDRESS = 0x04,
  START_LINEAR_ADDRESS = 0x05
};

enum class ParseStatus {
  SUCCESS,
  FILE_OPEN_ERROR,
  INVALID_FORMAT,
  CHECKSUM_ERROR,
  UNKNOWN_RECORD_TYPE,
  INVALID_DATA_LENGTH
};

class IhexParser {
 private:
  std::map<uint32_t, uint8_t> data;
  uint32_t start_address = 0;
  uint32_t extended_address = 0;

  uint8_t hexToByte(char high, char low) {
    uint8_t result = 0;

    if (high >= '0' && high <= '9') {
      result |= (high - '0') << 4;
    } else if (high >= 'A' && high <= 'F') {
      result |= (10 + high - 'A') << 4;
    } else if (high >= 'a' && high <= 'f') {
      result |= (10 + high - 'a') << 4;
    } else {
      return 0xFF;
    }

    if (low >= '0' && low <= '9') {
      result |= (low - '0');
    } else if (low >= 'A' && low <= 'F') {
      result |= (10 + low - 'A');
    } else if (low >= 'a' && low <= 'f') {
      result |= (10 + low - 'a');
    } else {
      return 0xFF;
    }

    return result;
  }

  uint8_t calculateChecksum(const std::vector<uint8_t>& record) {
    uint8_t sum = 0;
    for (uint8_t byte : record) {
      sum += byte;
    }
    return (~sum + 1) & 0xFF;
  }

  ParseStatus parseLine(const std::string& line) {
    if (line.empty() || line[0] != ':') {
      return ParseStatus::INVALID_FORMAT;
    }

    if (line.length() < 3) {
      return ParseStatus::INVALID_FORMAT;
    }

    uint8_t data_length = hexToByte(line[1], line[2]);
    if (data_length == 0xFF) {
      return ParseStatus::INVALID_FORMAT;
    }

    size_t expected_length = 1 + 2 + 4 + 2 + 2 * data_length + 2;
    if (line.length() != expected_length) {
      return ParseStatus::INVALID_DATA_LENGTH;
    }

    uint16_t address = 0;
    address |= static_cast<uint16_t>(hexToByte(line[3], line[4])) << 8;
    address |= static_cast<uint16_t>(hexToByte(line[5], line[6]));

    uint8_t type_byte = hexToByte(line[7], line[8]);
    if (type_byte == 0xFF) {
      return ParseStatus::INVALID_FORMAT;
    }
    if (type_byte > static_cast<uint8_t>(RecordType::START_LINEAR_ADDRESS)) {
      return ParseStatus::UNKNOWN_RECORD_TYPE;
    }
    RecordType record_type = static_cast<RecordType>(type_byte);

    std::vector<uint8_t> data_bytes;
    for (uint8_t i = 0; i < data_length; ++i) {
      size_t pos = 9 + i * 2;
      uint8_t byte = hexToByte(line[pos], line[pos + 1]);
      data_bytes.push_back(byte);
    }

    size_t checksum_pos = 9 + data_length * 2;
    uint8_t checksum = hexToByte(line[checksum_pos], line[checksum_pos + 1]);

    std::vector<uint8_t> record_for_checksum;
    record_for_checksum.push_back(data_length);
    record_for_checksum.push_back(static_cast<uint8_t>(address >> 8));
    record_for_checksum.push_back(static_cast<uint8_t>(address & 0xFF));
    record_for_checksum.push_back(type_byte);
    record_for_checksum.insert(record_for_checksum.end(), data_bytes.begin(), data_bytes.end());

    uint8_t calculated_checksum = calculateChecksum(record_for_checksum);
    if (calculated_checksum != checksum) {
      return ParseStatus::CHECKSUM_ERROR;
    }

    switch (record_type) {
      case RecordType::DATA: {
        uint32_t absolute_address = extended_address + address;
        for (uint8_t byte : data_bytes) {
          data[absolute_address++] = byte;
        }
        break;
      }

      case RecordType::END_OF_FILE:
        break;

      case RecordType::EXTENDED_SEGMENT_ADDRESS: {
        if (data_length == 2) {
          uint16_t segment = (static_cast<uint16_t>(data_bytes[0]) << 8) | data_bytes[1];
          extended_address = static_cast<uint32_t>(segment) << 4;
        }
        break;
      }

      case RecordType::START_SEGMENT_ADDRESS:
        if (data_length == 4) {
          start_address = (static_cast<uint32_t>(data_bytes[0]) << 24) | (static_cast<uint32_t>(data_bytes[1]) << 16) |
                           (static_cast<uint32_t>(data_bytes[2]) << 8) | data_bytes[3];
        }
        break;

      case RecordType::EXTENDED_LINEAR_ADDRESS: {
        if (data_length == 2) {
          uint16_t linear = (static_cast<uint16_t>(data_bytes[0]) << 8) | data_bytes[1];
          extended_address = static_cast<uint32_t>(linear) << 16;
        }
        break;
      }

      case RecordType::START_LINEAR_ADDRESS:
        if (data_length == 4) {
          start_address = (static_cast<uint32_t>(data_bytes[0]) << 24) | (static_cast<uint32_t>(data_bytes[1]) << 16) |
                           (static_cast<uint32_t>(data_bytes[2]) << 8) | data_bytes[3];
        }
        break;

      default:
        return ParseStatus::UNKNOWN_RECORD_TYPE;
    }

    return ParseStatus::SUCCESS;
  }

 public:
  IhexParser() = default;
  ~IhexParser() = default;

  const std::map<uint32_t, uint8_t>& getData() const { return data; }

  uint32_t getStartAddress() const { return start_address; }

  bool saveAsBinary(const std::string& filename) {
    if (data.empty()) {
      return false;
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
      return false;
    }

    auto it = data.begin();
    uint32_t current_address = it->first;

    while (it != data.end()) {
      while (current_address < it->first) {
        file.put(0);
        current_address++;
      }

      file.put(it->second);
      current_address++;
      ++it;
    }

    return true;
  }

  ParseStatus parseFile(const std::string& filename) {
    data.clear();
    extended_address = 0;
    start_address = 0;
    std::ifstream file(filename);
    if (!file.is_open()) {
      return ParseStatus::FILE_OPEN_ERROR;
    }
    std::string line;
    size_t line_number = 0;
    while (std::getline(file, line)) {
      line_number++;
      line.erase(std::remove_if(line.begin(), line.end(), [](char c) { return c == '\r' || c == '\n'; }), line.end());
      if (line.empty()) {
        continue;
      }
      ParseStatus status = parseLine(line);
      if (status != ParseStatus::SUCCESS) {
        return status;
      }
      if (!line.empty() && line[0] == ':' && line.length() >= 9) {
        uint8_t type_byte = hexToByte(line[7], line[8]);
        if (type_byte == static_cast<uint8_t>(RecordType::END_OF_FILE)) {
          break;
        }
      }
    }
    return ParseStatus::SUCCESS;
  }

  ParseStatus DumpToMemory(std::shared_ptr<MemoryBase> memorySystem, uint64_t offset = 0) {
    if (data.empty()) {
      return ParseStatus::FILE_OPEN_ERROR;
    }
    auto it = data.begin();
    while (it != data.end()) {
      uint64_t run_start = it->first;
      uint32_t expected = it->first;
      std::vector<uint8_t> buf;
      while (it != data.end() && it->first == expected) {
        buf.push_back(it->second);
        ++expected;
        ++it;
      }
      memorySystem->Write(buf.data(), run_start + offset, buf.size());
    }
    return ParseStatus::SUCCESS;
  }
};

class Program {
 public:
  virtual ~Program() = default;

  virtual std::shared_ptr<Instruction> MakeInst(const std::string& name, const std::vector<uint64_t>& args) {
    LOGCHECK(false, "this Program has no instruction factory (use a per-ISA Program subclass)");
    return nullptr;
  }

  struct Line {
    std::string name;
    std::vector<uint64_t> args;
  };

  static std::vector<Line> ParseText(const std::string& text) {
    std::vector<Line> out;
    std::stringstream in(text);
    std::string raw;
    while (std::getline(in, raw)) {
      std::stringstream ls(StripComment(raw));
      std::string tok;
      if (!(ls >> tok)) continue;
      Line l;
      l.name = tok;
      while (ls >> tok) l.args.push_back(ParseInt(tok));
      out.push_back(std::move(l));
    }
    return out;
  }

  void LoadText(const std::string& text) {
    for (auto& l : ParseText(text)) instructions.push_back(MakeInst(l.name, l.args));
  }
  bool LoadTextFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
      spdlog::error("Program: cannot open {}", path);
      return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    LoadText(ss.str());
    return true;
  }

  bool LoadElf(const std::string& path, std::shared_ptr<MemoryBase> mem) {
    ElfParser p(path);
    entryAddr = p.GetStartAddr();
    bool ok = (p.DumpToMemory(mem) == 0);
    hasImage = hasImage || ok;
    return ok;
  }
  bool LoadHex(const std::string& path, std::shared_ptr<MemoryBase> mem, uint64_t offset = 0) {
    IhexParser p;
    if (p.parseFile(path) != ParseStatus::SUCCESS) return false;
    entryAddr = p.getStartAddress();
    bool ok = (p.DumpToMemory(mem, offset) == ParseStatus::SUCCESS);
    hasImage = hasImage || ok;
    return ok;
  }

  // 按名字查符号地址(见 `ElfParser::FindSymbol`)。查不到返回 0。
  static uint64_t PeekSymbol(const std::string& path, const std::string& name) {
    ElfParser p(path);
    uint64_t a = 0;
    return p.FindSymbol(name, a) ? a : 0;
  }

  static uint64_t PeekEntry(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return 0;
    unsigned char magic[4] = {0, 0, 0, 0};
    f.read(reinterpret_cast<char*>(magic), 4);
    f.close();
    if (magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
      ElfParser p(path);
      return p.GetStartAddr();
    }
    if (magic[0] == ':') {
      IhexParser p;
      p.parseFile(path);
      return p.getStartAddress();
    }
    return 0;
  }

  bool Load(const std::string& path, std::shared_ptr<MemoryBase> mem) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
      spdlog::error("Program: cannot open {}", path);
      return false;
    }
    unsigned char magic[4] = {0, 0, 0, 0};
    f.read(reinterpret_cast<char*>(magic), 4);
    f.close();
    if (magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') return LoadElf(path, mem);
    if (magic[0] == ':') return LoadHex(path, mem);
    return LoadTextFile(path);
  }

  void Append(std::shared_ptr<Instruction> inst) { instructions.push_back(inst); }
  const std::vector<std::shared_ptr<Instruction>>& GetInstructions() const { return instructions; }

  size_t Size() const { return instructions.size(); }
  bool Empty() const { return instructions.empty(); }
  void Clear() {
    instructions.clear();
    entryAddr = 0;
    hasImage = false;
  }
  bool IsValid() const {
    for (auto& inst : instructions)
      if (!inst) return false;
    return true;
  }

  uint64_t Entry() const { return entryAddr; }
  bool HasImage() const { return hasImage; }

 private:
  static std::string StripComment(const std::string& s) {
    size_t cut = s.size();
    size_t h = s.find('#');
    if (h != std::string::npos) cut = std::min(cut, h);
    size_t c = s.find("//");
    if (c != std::string::npos) cut = std::min(cut, c);
    return s.substr(0, cut);
  }

  static uint64_t ParseInt(const std::string& tok) {
    size_t pos = 0;
    uint64_t v = std::stoull(tok, &pos, 0);  // base 0：自动识别 0x / 十进制
    LOGCHECK(pos == tok.size(), "Program: invalid integer token");
    return v;
  }

  std::vector<std::shared_ptr<Instruction>> instructions;
  uint64_t entryAddr = 0;
  bool hasImage = false;
};

}  // namespace latch
#endif
