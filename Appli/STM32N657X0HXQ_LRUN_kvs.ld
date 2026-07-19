/*
******************************************************************************
** STM32N657X0HXQ_LRUN_kvs.ld
**
** Custom linker script for the KVS WebRTC / HW_Crypto build.
**
** Memory layout (AXISRAM 0x34000000-0x343BFFFF = 3792K total):
**   ROM  (code+rodata) : 0x34000400, 0x109000 (~1051K used, ~8K headroom)
**   RAM  (data+bss)    : 0x34109400, to 0x34200000 (~980K used, ~8K headroom)
**   AXISRAM3-6 0x34200000-0x343BFFF8: RESERVED for NPU activation pools —
**     network.c bakes these addresses in (npuRAM3-6).  The app MUST NOT
**     extend past 0x34200000 or inference corrupts live app memory.
**     (2026-07-19: RAM previously spanned SRAM3-6 with the stack at
**     0x34300400 — inference would have scribbled over heap + ISR stack.
**     xAppContext (~1MB) and the AI ec-blob staging array moved to PSRAM
**     to make the app fit in AXISRAM1+2.  Margins are thin: link fails
**     loudly on overflow — if that happens, move more cold .bss to
**     .psram_bss rather than growing past 0x34200000.)
**   EXTRAM (PSRAM APS256XX via XSPI1) : 0x90000000, 16MB (.psram_bss NOLOAD)
**
** .psram_bss is placed in EXTRAM with NOLOAD so its allocation does NOT
** create a load image in ROM or RAM.  PSRAM is initialised at runtime by
** MediaPort_EnsurePsram()/AppMediaSourcePort_Init() — .psram_bss content is
** NOT zeroed at startup; every symbol there must be fully written before
** first read (xAppContext is memset after MediaPort_EnsurePsram()).
******************************************************************************
*/

ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);
_sstack = _estack - _Min_Stack_Size;

_Min_Heap_Size  = 0x200;
_Min_Stack_Size = 0x800;

MEMORY
{
  ROM    (xrw) : ORIGIN = 0x34000400, LENGTH = 0x109000  /* AXISRAM1(+2 start) */
  RAM    (xrw) : ORIGIN = 0x34109400, LENGTH = 0xF6C00   /* ends 0x34200000 = start of NPU-reserved SRAM3 */
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
