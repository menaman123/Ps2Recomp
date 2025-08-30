#include <capstone/capstone.h>
#include <rabbitizer.h>
#include "instructions/RabbitizerInstructionR5900.h"
#include "Function.h"
#include <elfio/elfio.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>

// This function takes a blob of bytes and prints the disassembled MIPS instructions.
static void disassemble_and_print(const uint8_t* code, size_t size, uint64_t base_address) {
    RabbitizerInstruction insn;
    char buffer[256];

    // Manually loop through the entire section, 4 bytes at a time.
    for (size_t offset = 0; offset < size; offset += 4) {
        // For the R5900 (PS2), Rabbitizer expects little-endian instruction words.
        uint32_t raw_data = *(reinterpret_cast<const uint32_t*>(code + offset));
        uint64_t current_address = base_address + offset;

        // Initialize the instruction struct with the raw data and address
        RabbitizerInstructionR5900_init(&insn, raw_data, current_address);
        // Decode the instruction to determine its type and properties
        RabbitizerInstructionR5900_processUniqueId(&insn);

        // Check if the instruction is valid
        if (RabbitizerInstruction_isValid(&insn)) {
            // Disassemble the instruction into the buffer
            RabbitizerInstruction_disassemble(&insn, buffer, NULL, 0, 0);
            std::cout << "0x" << std::hex << current_address
                      << ":\t" << buffer << std::dec << std::endl;
        } else {

            // Failure - print the raw data and a detailed breakdown
            std::cout << "0x" << std::hex << current_address
                      << ":\t.word   0x" << std::setw(8) << std::setfill('0') << raw_data
                      << "  // <invalid instruction> ID: " << std::dec << insn.uniqueId
                      << " (" << RabbitizerInstrId_getOpcodeName(insn.uniqueId) << ")"
                      << " | opcode: 0x" << std::hex <<RAB_INSTR_GET_opcode(&insn)
                      << " | rs: " << std::dec <<RAB_INSTR_GET_rs(&insn)
                      << " | rt: " << std::dec <<RAB_INSTR_GET_rt(&insn)
                      << " | rd: " << std::dec <<RAB_INSTR_GET_rd(&insn)
                      << " | sa: " << std::dec <<RAB_INSTR_GET_sa(&insn)
                      << " | function: 0x" << std::hex <<RAB_INSTR_GET_function(&insn)
                      << std::dec << std::endl;
        }
        // Clean up the instruction struct
        RabbitizerInstruction_destroy(&insn);
    }
}


int main(int argc, char** argv) {
    // Use the first command-line argument as the ELF file path.
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path_to_elf_file>\n";
        return 1;
    }
    std::string filePath = argv[1];

    // Load the ELF file using the ELFIO library.
    ELFIO::elfio reader;
    if (!reader.load(filePath)) {
        std::cerr << "[-] Could not load ELF file: " << filePath << "\n";
        return 1;
    }

    // Initialize the Capstone disassembler for MIPS (64-bit, little-endian).
    csh handle;
    if (cs_open(CS_ARCH_MIPS, (cs_mode)(CS_MODE_MIPS64 + CS_MODE_LITTLE_ENDIAN), &handle) != CS_ERR_OK) {
        std::cerr << "[-] Failed to initialize Capstone\n";
        return 1;
    }
    
    // We don't need detailed instruction info for this simple disassembly.
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_OFF);

    bool found_code = false;

    // Iterate through all sections in the ELF file.
    for (ELFIO::Elf_Half i = 0; i < reader.sections.size(); ++i) {
        
        const ELFIO::section* sec = reader.sections[i];
        if (sec->get_name().compare(".text") != 0){
            continue;
        }

        // Check if the section contains executable instructions.

        const char* data = sec->get_data();

        const uint8_t* code = reinterpret_cast<const uint8_t*>(data);
        ELFIO::Elf_Xword size = sec->get_size();
        ELFIO::Elf64_Addr addr = sec->get_address();

        std::cout << "\n// Disassembling section: '" << sec->get_name()
                  << "' at 0x" << std::hex << addr
                  << " (Size: " << std::dec << size << " bytes)\n";
        
        disassemble_and_print(code, static_cast<size_t>(size), static_cast<uint64_t>(addr));
        found_code = true;
    }

    if (!found_code) {
        std::cerr << "[-] No executable sections (.text) found in the ELF file.\n";
    }

    // Clean up Capstone.
    cs_close(&handle);
    return found_code ? 0 : 2;
}