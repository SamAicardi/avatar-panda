// XXX: plugin ported to panda2, starting from https://github.com/panda-re/panda/blob/panda1/qemu/panda_plugins/memdump/memdump.cpp

// This needs to be defined before anything is included in order to get
// the PRIx64 macro
#define __STDC_FORMAT_MACROS

#include "panda/plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <map>
#include <list>
#include <algorithm>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>
#include <vector>
#include <iostream>


// These need to be extern "C" so that the ABI is compatible with
// QEMU/PANDA, which is written in C
extern "C" {

bool init_plugin(void *);
void uninit_plugin(void *);
int mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr, target_ulong size, void *buf);
int mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr, target_ulong size);
}

struct mem_access {
    target_ulong pc;
    target_ulong addr;
    target_ulong size;
    uint8_t *buf;
};

using namespace std;

#define GUEST_MEMORY_SIZE 0x100000 
#define MAX_SIZE 100

#define SERIAL_START_ADDRESS 0xff000
#define SERIAL_DR SERIAL_START_ADDRESS
#define SERIAL_FR SERIAL_START_ADDRESS + 0x18

uint8_t guest_memory_reads[GUEST_MEMORY_SIZE];
uint8_t guest_memory_writes[GUEST_MEMORY_SIZE];
vector<struct mem_access> serial_reads_DR;
vector<struct mem_access> serial_reads_FR;

int mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr, target_ulong size, void *buf) {
    int i;
    // populate guest_memory_writes
    for (i=0; i<size; i++){
        if (guest_memory_writes[addr+i] != 0){
            guest_memory_writes[addr+i] = ((uint8_t *)buf)[i];
        }
    }
    fprintf(stderr, "[mem_write] - pc: %x, addr: %x, size: %x, ", pc, addr, size);
    for (int i=0; i<size; i++){
        fprintf(stderr, "0x%hhx ", ((uint8_t *)buf)[i]);
    }
    fprintf(stderr, "\n");

    return 1;
}

void error(std::string message){
    cerr << "=================================================\n";
    cerr << "   [my_memdump] - ERROR: " << message << endl;
    cerr << "=================================================\n";
    exit(1);
}

/* The goal here is to dump the initial memory state, to be used then in the LLVM passes 
 * to populate the global variable guest_memory.
 * We need to take into account only the first time that the content of a memory address is read.
 * */
int mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr, target_ulong size, void *buf) {
    int i;
    struct mem_access ma;
    // populate guest_memory_reads
    if (addr == SERIAL_DR){
        fprintf(stderr, "[SERIAL_DR] - pc: %x, addr: %x, size: %x, ", pc, addr, size);
        for (int i=0; i<size; i++){
            fprintf(stderr, "0x%hhx ", ((uint8_t *)buf)[i]);
        }
        fprintf(stderr, "\n");
        if (size != 4){
            error("size != 4");
        }
        // populate the struct
        ma.pc = pc;
        ma.addr = addr;
        ma.size = size;
        ma.buf = (uint8_t *) malloc(size*sizeof(uint8_t));
        memcpy(ma.buf, buf, size);

        serial_reads_DR.push_back(ma);
        return 0;
    }
    if (addr == SERIAL_FR){
        fprintf(stderr, "[SERIAL_FR] - pc: %x, addr: %x, size: %x, ", pc, addr, size);
        for (int i=0; i<size; i++){
            fprintf(stderr, "0x%hhx ", ((uint8_t *)buf)[i]);
        }
        fprintf(stderr, "\n");
        if (size != 4){
            error("size != 4");
        }
        // populate the struct
        ma.pc = pc;
        ma.addr = addr;
        ma.size = size;
        ma.buf = (uint8_t *) malloc(size*sizeof(uint8_t));
        memcpy(ma.buf, buf, size);

        serial_reads_FR.push_back(ma);
        return 0;
    }
    for (i=0; i<size; i++){
        if (guest_memory_reads[addr+i] == 0 && guest_memory_writes[addr+i] == 0){
            guest_memory_reads[addr+i] = ((uint8_t *)buf)[i];
        }
    }
    fprintf(stderr, "[mem_read] - pc: %x, addr: %x, size: %x, ", pc, addr, size);
    for (int i=0; i<size; i++){
        fprintf(stderr, "0x%hhx ", ((uint8_t *)buf)[i]);
    }
    fprintf(stderr, "\n");

    return 1;
}

bool init_plugin(void *self) {
    panda_cb pcb;

    // Need this to get EIP with our callbacks
    panda_enable_precise_pc();
    // Enable memory logging
    panda_enable_memcb();

    // initialize guest_memory
    memset(guest_memory_reads, 0, GUEST_MEMORY_SIZE);
    memset(guest_memory_writes, 0, GUEST_MEMORY_SIZE);

    pcb.virt_mem_after_read = mem_read_callback;
    panda_register_callback(self, PANDA_CB_VIRT_MEM_AFTER_READ, pcb);
    pcb.virt_mem_before_write = mem_write_callback;
    panda_register_callback(self, PANDA_CB_VIRT_MEM_BEFORE_WRITE, pcb);

    return true;
}

void uninit_plugin(void *self) {
    fprintf(stderr, "[uninit_plugin] - Initial state:\n");
    int reads_log;
    int serial_reads_log;
    uint32_t i, start_address;
    int size=0;
    struct mem_access *ma;
    bool storing_bytes = false;
    uint8_t *tmp_buf;

    reads_log = open("reads_log.bin", O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
    if(reads_log == -1) {
        printf("Couldn't write report:\n");
        perror("fopen");
        return;
    }
    serial_reads_log = open("serial_reads_log.bin", O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
    if(serial_reads_log == -1) {
        printf("Couldn't write report:\n");
        perror("fopen");
        return;
    }

    for (auto ma : serial_reads_DR){
        // write to file:
        // address(32bit) || size(32bit) || content(size*8bit)
        // XXX: by fixing the sizes we lose the adaptability of target_ulong
        if (write(serial_reads_log, &ma.addr, 4) != 4){
            fprintf(stderr, "Couldn't write ma.addr\n");
            perror("write");
        }
        if (write(serial_reads_log, &ma.size, 4) != 4){
            fprintf(stderr, "Couldn't write ma.size\n");
            perror("write");
        }
        if (write(serial_reads_log, ma.buf, ma.size) != ma.size){
            fprintf(stderr, "Couldn't write ma.size\n");
            perror("write");
        }
    }
    for (auto ma : serial_reads_FR){
        // write to file:
        // address(32bit) || size(32bit) || content(size*8bit)
        // XXX: by fixing the sizes we lose the adaptability of target_ulong
        if (write(serial_reads_log, &ma.addr, 4) != 4){
            fprintf(stderr, "Couldn't write ma.addr\n");
            perror("write");
        }
        if (write(serial_reads_log, &ma.size, 4) != 4){
            fprintf(stderr, "Couldn't write ma.size\n");
            perror("write");
        }
        if (write(serial_reads_log, ma.buf, ma.size) != ma.size){
            fprintf(stderr, "Couldn't write ma.size\n");
            perror("write");
        }
    }
                                                


    tmp_buf = (uint8_t *) calloc(MAX_SIZE , sizeof(uint8_t));
    for (i=0; i<GUEST_MEMORY_SIZE; i++){
        if (guest_memory_reads[i] != 0 && size < MAX_SIZE){
            if (! storing_bytes){
                start_address = i;
                storing_bytes = true;
            }
            tmp_buf[size++] = guest_memory_reads[i];
        }
        else{
            if (! storing_bytes){
                continue;
            }
            // populate the mem_access structure
            ma = (struct mem_access *) malloc(sizeof(struct mem_access));
            ma->addr = start_address;
            ma->size = size;
            ma->buf = (uint8_t *) malloc(size*sizeof(uint8_t));
            memcpy(ma->buf, tmp_buf, size);
            
            // write to file
            // address(32bit) || size(32bit) || content(size*8bit)
            // XXX: by fixing the sizes we lose the adaptability of target_ulong
            if (write(reads_log, &ma->addr, 4) != 4){
                fprintf(stderr, "Couldn't write ma->addr\n");
                perror("write");
            }
            if (write(reads_log, &ma->size, 4) != 4){
                fprintf(stderr, "Couldn't write ma->addr\n");
                perror("write");
            }
            if (write(reads_log, ma->buf, ma->size) != ma->size){
                fprintf(stderr, "Couldn't write ma->addr\n");
                perror("write");
            }

            fprintf(stderr, "[mem_read] - pc: %x, addr: %x, size: %x, ", ma->pc, ma->addr, ma->size);
            for (int i=0; i<ma->size; i++){
                fprintf(stderr, "0x%hhx ", ma->buf[i]);
            }
            fprintf(stderr, "\n");

            // free the structure
            free(ma->buf);
            free(ma);
            free(tmp_buf);
            tmp_buf = (uint8_t *) calloc(MAX_SIZE , sizeof(uint8_t));
            size = 0;
            storing_bytes = false;
        }


    }
    close(reads_log);
    close(serial_reads_log);
}
