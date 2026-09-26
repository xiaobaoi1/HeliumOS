ENTRY(_start_crt0)
SECTIONS
{
    . = 0x40000000;
    .text : { *(.text) }
    .data : { *(.data) }
    .bss : { *(.bss) }
}