/*
******************************************************************************
** STM32N657X0HXQ_LRUN_kvs.ld
**
** Custom linker script for the KVS WebRTC / HW_Crypto build.
**
** Memory layout (AXISRAM 0x34000000-0x343BFFFF = 3792K total):
**   ROM  (code+rodata) : 0x34000400, 1536K  (~1.4MB used, 100K headroom)
**   RAM  (data+bss)    : 0x34180400, 2304K  (extends through SRAM5+SRAM6)
**   EXTRAM (PSRAM via XSPI2) : 0x90000000, 16MB  (for .psram_bss NOLOAD)
**
** .psram_bss is placed in EXTRAM with NOLOAD so its 4MB allocation does NOT
** create a load image in ROM or RAM.  The PSRAM must be initialised by the
** bootloader (ExtMemLoader) before the application runs.
******************************************************************************
*/

ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);
_sstack = _estack - _Min_Stack_Size;

_Min_Heap_Size  = 0x200;
_Min_Stack_Size = 0x800;

MEMORY
{
  ROM    (xrw) : ORIGIN = 0x34000400, LENGTH = 1536K
  RAM    (xrw) : ORIGIN = 0x34180400, LENGTH = 0x23FC00  /* ends at 0x343C0000 = end of SRAM6_AXI */
  EXTRAM (rw)  : ORIGIN = 0x90000000, LENGTH = 16M
}

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >ROM

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    *(.RamFunc)
    *(.RamFunc*)
    KEEP (*(.init))
    KEEP (*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >ROM

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >ROM

  .ARM.extab :
  {
    . = ALIGN(4);
    *(.ARM.extab* .gnu.linkonce.armextab.*)
    . = ALIGN(4);
  } >ROM

  .ARM :
  {
    . = ALIGN(4);
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
    . = ALIGN(4);
  } >ROM

  .preinit_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
    . = ALIGN(4);
  } >ROM

  .init_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
    . = ALIGN(4);
  } >ROM

  .fini_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
    . = ALIGN(4);
  } >ROM

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> ROM

  .noncacheable :
  {
    . = ALIGN(8);
    __snoncacheable = .;
    KEEP(*(.noncacheable))
    . = ALIGN(8);
    __enoncacheable = .;
  } >RAM

  .gnu.sgstubs :
  {
    _sNSCVeneer = .;
    . = ALIGN(4);
    *(.gnu.sgstubs*)
    . = ALIGN(4);
  } >ROM
  _eNSCVeneer = .;

  . = ALIGN(4);
  .bss :
  {
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /* Large encoder DMA buffer: placed in external PSRAM, no load image */
  .psram_bss (NOLOAD) :
  {
    . = ALIGN(32);
    _spsram_bss = .;
    *(.psram_bss)
    *(.psram_bss*)
    . = ALIGN(32);
    _epsram_bss = .;
  } >EXTRAM

  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
