// RUN: %cheri_purecap_cc1 -emit-obj %s -o %t.o
// RUN: ld.lld --fatal-warnings -shared -o %t.so %t.o -verbose-cap-relocs 2>&1 | FileCheck %s -check-prefix LINKER-MSG
// LINKER-MSG: warning: .global_sizes section contains unresolved values -> making writable because it references unresolved symbol errno
// LINKER-MSG-NEXT: Write .global_sizes: IS = {{.+}}global_sizes-undef.c.tmp.o:(.global_sizes)
// LINKER-MSG-NEXT: warning: Could not find .global_size for <undefined> (in GOT) errno
// LINKER-MSG-NEXT: Writing size 0x0 for <undefined> (in GOT) errno

// RUN: llvm-readelf -r -program-headers %t.so | FileCheck %s -check-prefix PHDRS
// RUN: llvm-objdump -s --section=.global_sizes %t.so | FileCheck %s -check-prefix DUMP

extern char* __progname;
extern char** environ;

char* getprogname() {
  return __progname;
}

char* getenv(void) {
  return environ[0];
}


// .global_sizes should be read-only:
// PHDRS-LABEL: Section to Segment mapping:
// PHDRS-NEXT:  Segment Sections...
// PHDRS-NEXT:   00
// PHDRS-NEXT:   01     .global_sizes .MIPS.abiflags .MIPS.options .dynsym .hash .dynamic .dynstr
// PHDRS-NEXT:   02     .text
// PHDRS-NEXT:   03     .data .got
// PHDRS-NEXT:   04     .dynamic
// PHDRS-NEXT:   05

// DUMP:      Contents of section .global_sizes:
// DUMP-NEXT: 0190 00000000 000000{{1|2}}0 00000000 000000{{1|2}}0 ...............
