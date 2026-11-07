#include <extable.h>
#include <stddef.h>

extern struct extable_entry __ex_table_start[];
extern struct extable_entry __ex_table_end[];

uint32_t extable_lookup(uint32_t fault_eip) {
    for (struct extable_entry *e = __ex_table_start;
         e < __ex_table_end; e++) {
        if (e->fault_addr == fault_eip) return e->fixup_addr;
    }
    return 0;
}