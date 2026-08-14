ENTRY(_start)

SECTIONS
{
    /* 内核加载到物理 1MB (0x100000)，虚拟地址等于物理地址 (平坦映射) */
    . = 1M;

    .text ALIGN(4K) :
    {
        *(.multiboot)  /* 必须放在最前面，保证 GRUB 能识别 */
        *(.text)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata)
    }

    .data ALIGN(4K) :
    {
        *(.data)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss)
    }
}