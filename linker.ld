ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    _kernel_start = .;

    .text ALIGN(4K) :
    {
        *(.multiboot)
        *(.text)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata)
    }

    /* 异常表：用于内核访问用户指针 fault 恢复 */
    . = ALIGN(4);
    __ex_table_start = .;
    .ex_table : { *(__ex_table) }
    __ex_table_end = .;

    .data ALIGN(4K) :
    {
        *(.data)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss)
    }

    _kernel_end = .;
}