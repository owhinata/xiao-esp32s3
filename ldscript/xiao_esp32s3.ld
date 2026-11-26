/*
 * XIAO ESP32S3 -- application image, booted by ESP-IDF's second-stage
 * bootloader and executing from flash.
 *
 * The image is a standard ESP-IDF application image ("esptool elf2image"):
 * the bootloader (bootloader/) picks one of the two OTA partitions ("ota_0",
 * "ota_1"; otadata says which), checks the image's SHA-256, copies the RAM
 * segments into SRAM, maps the two flash segments through the cache and MMU
 * at the same virtual addresses whichever partition they are in, and jumps
 * to _start (port/startup.c).
 *
 * Address spaces:
 *
 *   SRAM0   32 KiB   0x40370000-0x40378000 (instruction bus only).  Its
 *                    ICACHE_SIZE bytes, all of it, are the instruction
 *                    cache's own memory and must stay unused.
 *   SRAM1  416 KiB   0x40378000-0x403E0000 (instruction bus)
 *                    0x3FC88000-0x3FCF0000 (data bus, same memory)
 *   flash, through the cache and MMU in 64 KiB pages:
 *                    0x42000000-0x44000000 (instruction bus)
 *                    0x3C000000-0x3E000000 (data bus)
 *
 * (SRAM2, 0x3FCF0000-0x3FD00000, which this script does not use: the
 * 32 KiB data cache, CONFIG_ESP32S3_DATA_CACHE_32KB, takes its top half,
 * 0x3FCF8000-0x3FD00000, and the heap its low half, heap/port/esp32s3/
 * memory_layout.c.)
 *
 * SRAM1 is one memory with two addresses.  This script gives its low 96 KiB
 * to code and the rest to data; the split point below must name the same
 * physical byte on both buses (instruction address - 0x6F0000 = data address).
 *
 * What the boot chain occupies at the top of SRAM1 (data-bus addresses), from
 * the bootloader's linker script (bootloader/subproject/main/ld/esp32s3/
 * bootloader.memory.ld.in) and ESP-IDF's esp_system/ld/esp32s3/memory.ld.in:
 *
 *   0x3FCD7E00-0x3FCE9704  ROM: buffers of the download mode (bootsel)
 *   0x3FCD8700-0x3FCDB700  bootloader code that is done before the
 *                          application is loaded (its "iram_seg")
 *   0x3FCDB700-0x3FCE2700  bootloader code that loads the application
 *                          ("iram_loader_seg")
 *   0x3FCE2700-0x3FCE7700  bootloader data
 *   0x3FCE7700-0x3FCEB710  stack: the ROM's start-up stack from 0x3FCEB710
 *                          down, which the bootloader and _start run on
 *   0x3FCED710-0x3FCF0000  ROM data
 *
 * The bootloader refuses to load a segment that overlaps its loader code,
 * its data or its stack, so everything loaded ends below 0x3FCDB700
 * (DRAM_END, ESP-IDF's SRAM_DRAM_END).  Once _start runs, none of this is
 * needed any more.
 *
 * The heap is ESP-IDF's (heap_caps_init(), heap/port/esp32s3/
 * memory_layout.c): it takes every region of internal RAM in that file's
 * table, minus what the image reserves for itself, as in an ESP-IDF
 * application:
 *
 *   [_iram_start, _iram_end)   the code in SRAM0 and SRAM1, on both buses
 *                              (vectors, .iram0.fast and .iram0.text;
 *                              SRAM1's unused part becomes heap through its
 *                              data-bus alias, so every byte of code in RAM
 *                              is a byte less of heap)
 *   [_data_start, _heap_start) .data, .bss and .noinit
 *   [_rtc_reserved_start, _rtc_reserved_end)  data kept in RTC FAST memory
 *   the ROM's own data (the ROM's layout table, at run time)
 *
 * The instruction cache's memory at the start of SRAM0 is not in the table
 * (its SRAM0 entry starts behind it, CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB,
 * and is empty).
 * What lies above 0x3FCE9710 (ESP-IDF's
 * APP_USABLE_DRAM_END, the bottom of the ROM's start-up stack area) is the
 * stack _start and tx_application_define() run on; it is handed to the heap
 * only from the first thread on (heap_caps_enable_nonos_stack_heaps(),
 * an ESP_PRE_APP_MAIN_HANDLER of the heap component), when nothing runs on
 * that stack any more.  The 8 KiB of RTC FAST memory are heap too, apart
 * from .rtc_reserved at their top (CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP).
 *
 * .dram0.noinit (the RAM log and the record of a fatal error) keeps its
 * contents across a reset only if nothing writes there between two runs.  The bootloader writes only its own
 * sections, its stack and the application's loaded segments, and clears only
 * its own .bss; the ROM writes its download-mode buffers when bootsel is
 * used.  .dram0.noinit is therefore kept below 0x3FCD7E00 (NOINIT_END).
 *
 * The two flash windows share one MMU table: page N of the instruction window
 * and page N of the data window are the same table entry.  Code starts on the
 * first page of the instruction window; read-only data is placed in the data
 * window on the pages that follow the ones the code occupies.  The flash
 * address of a segment and its load address must agree modulo the 64 KiB MMU
 * page: esptool puts the data segment first, right behind the 0x18-byte image
 * header and its own 0x8-byte segment header, so it starts 0x20 into a page;
 * for the code segment esptool inserts padding.  The bootloader reads the
 * application description (esp_app_desc_t) from the start of that first
 * segment.
 */

ENTRY(_start)

ICACHE_SIZE     = 0x8000;                       /* 32 KiB; port/startup.c programs the cache with it */
IRAM_ORG        = 0x40370000 + ICACHE_SIZE;
IRAM_END        = 0x40390000;                   /* split point, instruction bus */
DRAM_ORG        = IRAM_END - 0x6F0000;          /* split point, data bus = 0x3FCA0000 */
DRAM_END        = 0x3FCDB700;                   /* start of the bootloader's loader code */
NOINIT_END      = 0x3FCD7E00;                   /* start of the ROM's download-mode buffers */
MMU_PAGE        = 0x10000;
IMAGE_HEADERS   = 0x20;                         /* image header + first segment header */

MEMORY
{
    iram_seg (RX) : org = 0x40378000, len = 0x40390000 - 0x40378000
    dram_seg (RW) : org = 0x3FCA0000, len = 0x3FCDB700 - 0x3FCA0000
    irom_seg (RX) : org = 0x42000000, len = 0x800000
    drom_seg (R)  : org = 0x3C000000, len = 0x800000
    /*
     * The top 24 bytes of RTC FAST memory (0x600FE000-0x60100000), ESP-IDF's
     * rtc_reserved_seg without the bootloader's part
     * (CONFIG_BOOTLOADER_RESERVE_RTC_SIZE is 0): the RTC timer's record.
     */
    rtc_reserved_seg (RW) : org = 0x60100000 - 0x18, len = 0x18
}

ASSERT(ORIGIN(iram_seg) == IRAM_ORG && ORIGIN(iram_seg) + LENGTH(iram_seg) == IRAM_END,
       "iram_seg does not match IRAM_ORG / IRAM_END")
ASSERT(ORIGIN(dram_seg) == DRAM_ORG && ORIGIN(dram_seg) + LENGTH(dram_seg) == DRAM_END,
       "dram_seg does not start at the data-bus alias of IRAM_END")

_icache_size = ABSOLUTE(ICACHE_SIZE);

SECTIONS
{
    /*
     * Exception and interrupt vectors (esp-hal-3rdparty xtensa_vectors.S).
     * VECBASE is set to _vector_table by _start; the offsets are fixed by the
     * ESP32-S3 core configuration (XCHAL_*_VECOFS in xtensa/config/core-isa.h).
     *
     * Literals belonging to the vectors go first: l32r can only reach
     * backwards, so they must sit below the code that loads them.
     */
    .iram0.vectors :
    {
        _iram_start = ABSOLUTE(.);
        . = ALIGN(4);
        *(.*Vector.literal)
        . = ALIGN(0x400);
        _vector_table = ABSOLUTE(.);
        _vector_start = .;
        . = _vector_start + 0x000;  KEEP(*(.WindowVectors.text))
        . = _vector_start + 0x180;  KEEP(*(.Level2InterruptVector.text))
        . = _vector_start + 0x1C0;  KEEP(*(.Level3InterruptVector.text))
        . = _vector_start + 0x200;  KEEP(*(.Level4InterruptVector.text))
        . = _vector_start + 0x240;  KEEP(*(.Level5InterruptVector.text))
        . = _vector_start + 0x280;  KEEP(*(.DebugExceptionVector.text))
        . = _vector_start + 0x2C0;  KEEP(*(.NMIExceptionVector.text))
        . = _vector_start + 0x300;  KEEP(*(.KernelExceptionVector.text))
        . = _vector_start + 0x340;  KEEP(*(.UserExceptionVector.text))
        . = _vector_start + 0x3C0;  KEEP(*(.DoubleExceptionVector.text))
        . = _vector_start + 0x400;
        /* Where backtraces of freshly created contexts end; the layer wants it here. */
        KEEP(*(._invalid_pc_placeholder.text))
    } > iram_seg

    /*
     * Code in RAM for speed only: the per-packet path of Wi-Fi and TCP/IP,
     * which from flash would run through the 32 KiB instruction cache and
     * still miss on almost every packet (the cache holds what is left in
     * flash of that path: the kernel, esp_timer, the sockets).  Each group
     * below names the ESP-IDF option or linker fragment that keeps the same
     * code out of flash
     * there, or says that ESP-IDF leaves it in flash.  A group is dropped by
     * deleting its lines (for the heap, see also sdkconfig.h); what it
     * leaves goes to .flash.text.  The read-only data of this code stays in
     * flash.  Every byte here is a byte less of heap (see the top).
     *
     * Nothing here runs while the cache is off: the flash driver masks
     * every interrupt up to XCHAL_EXCM_LEVEL, the level of every interrupt
     * handler here, and allows no thread switch meanwhile
     * (port/board_flash_os.c); none of it is an exception vector's or a
     * high-priority interrupt's code, nor on the fault path.  It calls code
     * in flash freely.  ldscript/check_ram_refs.py therefore does not check
     * this section, and treats a reference into it from the code that must
     * run from RAM (.iram0.vectors, the objects listed in .iram0.text) like
     * one into the flash.
     *
     * This output section comes before .iram0.text so that its patterns
     * take precedence over the generic .iram1 one there (the heap's
     * IRAM_ATTR functions).  Functions are named by their input sections
     * (-ffunction-sections), with the suffix GCC gives a clone ($isra$0,
     * $part$0, $constprop$0) where the build has one instead of the plain
     * function; a function the compiler inlines has no section of its own.
     * A name that no longer matches (another compiler, changed sources)
     * leaves its function in flash.
     */
    .iram0.fast :
    {
        . = ALIGN(4);
        /*
         * Wi-Fi, CONFIG_ESP_WIFI_IRAM_OPT (esp_wifi/linker.lf: libpp and
         * libnet80211 sections; esp_adapter.c's coex_pti_get_wrapper).
         */
        *libpp.a:(.wifi0iram .wifi0iram.* .wifiorslpiram .wifiorslpiram.*)
        *libnet80211.a:(.wifi0iram .wifi0iram.*)
        *esp_adapter.c.obj(.literal.coex_pti_get_wrapper .text.coex_pti_get_wrapper)
        /* Wi-Fi, CONFIG_ESP_WIFI_RX_IRAM_OPT (esp_wifi/linker.lf). */
        *libpp.a:(.wifirxiram .wifirxiram.* .wifislprxiram .wifislprxiram.*)
        *libnet80211.a:(.wifirxiram .wifirxiram.* .wifislprxiram .wifislprxiram.*)
        /*
         * Wi-Fi, CONFIG_ESP_WIFI_EXTRA_IRAM_OPT (esp_wifi/linker.lf; off by
         * default in ESP-IDF on this chip): the rest of the transmit path
         * and its completion, which every TCP segment and every ACK takes.
         */
        *libpp.a:(.wifiextrairam .wifiextrairam.*)
        *libnet80211.a:(.wifiextrairam .wifiextrairam.*)
        /*
         * lwIP, CONFIG_LWIP_IRAM_OPTIMIZATION (lwip/linker.lf), with the
         * helpers it lists for builds that do not inline them.
         */
        *liblwip.a:sockets.c.obj(
            .literal.tryget_socket_unconn_locked .text.tryget_socket_unconn_locked
            .literal.tryget_socket_unconn .text.tryget_socket_unconn
            .literal.done_socket .text.done_socket
            .literal.lwip_recvfrom .text.lwip_recvfrom
            .literal.lwip_recvfrom_udp_raw$constprop$0 .text.lwip_recvfrom_udp_raw$constprop$0
            .literal.lwip_recv_tcp .text.lwip_recv_tcp
            .literal.lwip_send .text.lwip_send
            .literal.lwip_sendto .text.lwip_sendto
            .literal.event_callback .text.event_callback)
        *liblwip.a:api_lib.c.obj(
            .literal.netconn_apimsg .text.netconn_apimsg
            .literal.netconn_recv_data .text.netconn_recv_data
            .literal.netconn_tcp_recvd .text.netconn_tcp_recvd
            .literal.netconn_recv_data_tcp .text.netconn_recv_data_tcp
            .literal.netconn_recv_tcp_pbuf_flags .text.netconn_recv_tcp_pbuf_flags
            .literal.netconn_recv_udp_raw_netbuf_flags .text.netconn_recv_udp_raw_netbuf_flags
            .literal.netconn_send .text.netconn_send
            .literal.netconn_write_partly .text.netconn_write_partly
            .literal.netconn_write_vectors_partly .text.netconn_write_vectors_partly)
        *liblwip.a:api_msg.c.obj(
            .literal.lwip_netconn_do_send .text.lwip_netconn_do_send
            .literal.lwip_netconn_do_write .text.lwip_netconn_do_write)
        *liblwip.a:netbuf.c.obj(
            .literal.netbuf_alloc .text.netbuf_alloc
            .literal.netbuf_free .text.netbuf_free)
        *liblwip.a:tcpip.c.obj(
            .literal.tcpip_thread .text.tcpip_thread
            .literal.tcpip_inpkt .text.tcpip_inpkt
            .literal.tcpip_input .text.tcpip_input
            .literal.tcpip_callback .text.tcpip_callback
            .literal.tcpip_try_callback .text.tcpip_try_callback
            .literal.tcpip_send_msg_wait_sem .text.tcpip_send_msg_wait_sem)
        *liblwip.a:inet_chksum.c.obj(
            .literal.inet_chksum_pseudo .text.inet_chksum_pseudo)
        *liblwip.a:etharp.c.obj(
            .literal.etharp_output_to_arp_index .text.etharp_output_to_arp_index
            .literal.etharp_output .text.etharp_output)
        *liblwip.a:ip4_addr.c.obj(
            .literal.ip4_addr_isbroadcast_u32 .text.ip4_addr_isbroadcast_u32)
        *liblwip.a:ip4.c.obj(
            .literal.ip4_route_src .text.ip4_route_src
            .literal.ip4_route .text.ip4_route
            .literal.ip4_input .text.ip4_input
            .literal.ip4_output_if .text.ip4_output_if
            .literal.ip4_output_if_opt .text.ip4_output_if_opt
            .literal.ip4_output_if_src .text.ip4_output_if_src
            .literal.ip4_output_if_opt_src .text.ip4_output_if_opt_src)
        *liblwip.a:pbuf.c.obj(
            .literal.pbuf_alloc .text.pbuf_alloc
            .literal.pbuf_add_header .text.pbuf_add_header
            .literal.pbuf_remove_header .text.pbuf_remove_header
            .literal.pbuf_free .text.pbuf_free
            .literal.pbuf_alloced_custom .text.pbuf_alloced_custom)
        *liblwip.a:udp.c.obj(
            .literal.udp_input_local_match .text.udp_input_local_match
            .literal.udp_input .text.udp_input
            .literal.udp_send .text.udp_send
            .literal.udp_sendto .text.udp_sendto
            .literal.udp_sendto_if .text.udp_sendto_if
            .literal.udp_sendto_if_src .text.udp_sendto_if_src)
        *liblwip.a:ethernet.c.obj(
            .literal.ethernet_input .text.ethernet_input
            .literal.ethernet_output .text.ethernet_output)
        *liblwip.a:sys_arch.c.obj(
            .literal.sys_mutex_lock$part$0 .text.sys_mutex_lock$part$0
            .literal.sys_mutex_unlock$part$0 .text.sys_mutex_unlock$part$0
            .literal.sys_sem_signal .text.sys_sem_signal
            .literal.sys_arch_sem_wait .text.sys_arch_sem_wait
            .literal.sys_mbox_post .text.sys_mbox_post
            .literal.sys_mbox_trypost .text.sys_mbox_trypost
            .literal.sys_arch_mbox_fetch .text.sys_arch_mbox_fetch)
        *liblwip.a:lwip_default_hooks.c.obj(
            .literal.ip4_route_src_hook .text.ip4_route_src_hook)
        /* lwIP's TCP part, CONFIG_LWIP_EXTRA_IRAM_OPTIMIZATION (lwip/linker.lf). */
        *liblwip.a:api_msg.c.obj(
            .literal.recv_udp .text.recv_udp)
        *liblwip.a:inet_chksum.c.obj(
            .literal.inet_chksum_pbuf .text.inet_chksum_pbuf
            .literal.ip_chksum_pseudo .text.ip_chksum_pseudo
            .literal.inet_chksum .text.inet_chksum
            .literal.lwip_standard_chksum .text.lwip_standard_chksum)
        *liblwip.a:pbuf.c.obj(
            .literal.pbuf_copy .text.pbuf_copy
            .literal.pbuf_copy_partial_pbuf .text.pbuf_copy_partial_pbuf
            .literal.pbuf_clone .text.pbuf_clone
            .literal.pbuf_clone$part$0 .text.pbuf_clone$part$0)
        *liblwip.a:tcp.c.obj(
            .literal.tcp_fasttmr .text.tcp_fasttmr
            .literal.tcp_tmr .text.tcp_tmr
            .literal.tcp_update_rcv_ann_wnd .text.tcp_update_rcv_ann_wnd
            .literal.tcp_recved .text.tcp_recved
            .literal.tcp_slowtmr .text.tcp_slowtmr
            .literal.tcp_process_refused_data .text.tcp_process_refused_data
            .literal.tcp_segs_free .text.tcp_segs_free
            .literal.tcp_seg_free .text.tcp_seg_free
            .literal.tcp_seg_copy .text.tcp_seg_copy
            .literal.tcp_recv_null .text.tcp_recv_null)
        *liblwip.a:tcp_in.c.obj(
            .literal.tcp_input .text.tcp_input
            .literal.tcp_input_delayed_close .text.tcp_input_delayed_close
            .literal.tcp_oos_insert_segment .text.tcp_oos_insert_segment
            .literal.tcp_receive .text.tcp_receive
            .literal.tcp_parseopt .text.tcp_parseopt
            .literal.tcp_free_acked_segments$isra$0 .text.tcp_free_acked_segments$isra$0)
        *liblwip.a:tcp_out.c.obj(
            .literal.tcp_create_segment .text.tcp_create_segment
            .literal.tcp_write .text.tcp_write
            .literal.tcp_split_unsent_seg .text.tcp_split_unsent_seg
            .literal.tcp_rexmit_rto_prepare .text.tcp_rexmit_rto_prepare
            .literal.tcp_rexmit .text.tcp_rexmit
            .literal.tcp_rexmit_fast .text.tcp_rexmit_fast
            .literal.tcp_rst .text.tcp_rst
            .literal.tcp_send_empty_ack .text.tcp_send_empty_ack
            .literal.tcp_output_alloc_header_common$constprop$0 .text.tcp_output_alloc_header_common$constprop$0)
        *liblwip.a:sys_arch.c.obj(
            .literal.sys_arch_protect .text.sys_arch_protect
            .literal.sys_arch_unprotect .text.sys_arch_unprotect
            .literal.sys_thread_tcpip .text.sys_thread_tcpip)
        *liblwip.a:memp.c.obj(
            .literal.memp_malloc .text.memp_malloc
            .literal.do_memp_malloc_pool .text.do_memp_malloc_pool)
        *liblwip.a:mem.c.obj(
            .literal.mem_malloc .text.mem_malloc)
        *liblwip.a:raw.c.obj(
            .literal.raw_input .text.raw_input)
        /*
         * lwIP: the rest of the TCP segment path, which ESP-IDF's lists
         * leave in flash: tcp_output() (every ACK and every segment sent),
         * the writer of the socket (lwip_netconn_do_writemore()), the
         * reader's window update (lwip_netconn_do_recv()), the frees of
         * pbufs and segments, the byte order, the tcpip thread's timer
         * check, and lwip_recv() (the shell's sockets receive with it).
         */
        *liblwip.a:sockets.c.obj(
            .literal.lwip_recv .text.lwip_recv)
        *liblwip.a:api_lib.c.obj(
            .literal.netconn_err .text.netconn_err)
        *liblwip.a:api_msg.c.obj(
            .literal.lwip_netconn_do_writemore$isra$0 .text.lwip_netconn_do_writemore$isra$0
            .literal.lwip_netconn_do_recv .text.lwip_netconn_do_recv
            .literal.lwip_netconn_is_err_msg .text.lwip_netconn_is_err_msg
            .literal.lwip_netconn_is_deallocated_msg .text.lwip_netconn_is_deallocated_msg)
        *liblwip.a:tcp_out.c.obj(
            .literal.tcp_output .text.tcp_output
            .literal.tcp_output_control_segment .text.tcp_output_control_segment)
        *liblwip.a:pbuf.c.obj(
            .literal.pbuf_cat .text.pbuf_cat
            .literal.pbuf_clen .text.pbuf_clen
            .literal.pbuf_realloc .text.pbuf_realloc
            .literal.pbuf_free_header .text.pbuf_free_header
            .literal.pbuf_copy_partial .text.pbuf_copy_partial
            .literal.pbuf_ref .text.pbuf_ref
            .literal.pbuf_ref$part$0 .text.pbuf_ref$part$0)
        *liblwip.a:memp.c.obj(
            .literal.memp_free .text.memp_free
            .literal.do_memp_free_pool$part$0 .text.do_memp_free_pool$part$0)
        *liblwip.a:mem.c.obj(
            .literal.mem_free .text.mem_free)
        *liblwip.a:def.c.obj(
            .literal.lwip_htons .text.lwip_htons
            .literal.lwip_htonl .text.lwip_htonl)
        *liblwip.a:err.c.obj(
            .literal.err_to_errno .text.err_to_errno)
        *liblwip.a:sys_arch.c.obj(
            .literal.sys_now .text.sys_now
            .literal.sys_arch_mbox_tryfetch .text.sys_arch_mbox_tryfetch
            .literal.sys_thread_sem_get .text.sys_thread_sem_get)
        *liblwip.a:timeouts.c.obj(
            .literal.sys_check_timeouts .text.sys_check_timeouts
            .literal.sys_timeouts_sleeptime .text.sys_timeouts_sleeptime)
        *liblwip.a:netif.c.obj(
            .literal.netif_get_by_index .text.netif_get_by_index)
        /*
         * The heap (CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH off: heap/linker.lf,
         * the HEAP_IRAM_ATTR functions, esp_libc's heap.c), with the
         * alignment lookups of DMA-capable allocations, which the Wi-Fi
         * driver makes per frame (esp_mm/linker.lf: esp_cache_msync.c).
         */
        *heap_caps.c.obj(.iram1 .iram1.*)
        *heap_caps_base.c.obj(.iram1 .iram1.*)
        *heap_align_hw.c.obj(.iram1 .iram1.*)
        *tlsf.c.obj(
            .literal.tlsf_block_size .text.tlsf_block_size
            .literal.tlsf_size .text.tlsf_size
            .literal.tlsf_alloc_overhead .text.tlsf_alloc_overhead
            .literal.tlsf_get_pool .text.tlsf_get_pool
            .literal.tlsf_malloc .text.tlsf_malloc
            .literal.tlsf_memalign_offs .text.tlsf_memalign_offs
            .literal.tlsf_free .text.tlsf_free
            .literal.tlsf_realloc .text.tlsf_realloc)
        *multi_heap.c.obj(
            .literal.multi_heap_get_allocated_size_impl .text.multi_heap_get_allocated_size_impl
            .literal.multi_heap_set_lock .text.multi_heap_set_lock
            .literal.multi_heap_malloc_impl .text.multi_heap_malloc_impl
            .literal.multi_heap_free_impl .text.multi_heap_free_impl
            .literal.multi_heap_realloc_impl .text.multi_heap_realloc_impl
            .literal.multi_heap_aligned_alloc_offs .text.multi_heap_aligned_alloc_offs
            .literal.multi_heap_internal_lock .text.multi_heap_internal_lock
            .literal.multi_heap_internal_unlock .text.multi_heap_internal_unlock
            .literal.assert_valid_block .text.assert_valid_block)
        */esp_libc/src/heap.c.obj(.literal .literal.* .text .text.*)
        *esp_cache_msync.c.obj(
            .literal.esp_cache_get_alignment .text.esp_cache_get_alignment)
        *esp_mspi_align.c.obj(
            .literal.esp_mspi_get_alignment .text.esp_mspi_get_alignment)
        . = ALIGN(4);
    } > iram_seg

    /*
     * Code that must run from RAM:
     *   - the start-up code, which reconfigures the cache that code in flash
     *     is fetched through (port/startup.c), and the ROM cache patches it
     *     calls (esp_rom_cache_esp32s2_esp32s3.c),
     *   - fault reporting (port/panic.c), which fills and seals the record of
     *     the error before it reads flash: the flash mapping may be the
     *     thing that is broken, or the cache suspended by the flash driver,
     *     and the watchdogs it stops and arms on the way (port/watchdog.c,
     *     which also feeds on every tick and in the flash driver's windows),
     *   - what Espressif marks IRAM_ATTR (.iram1), and the objects ESP-IDF
     *     keeps out of flash in an application so that they work with the
     *     cache disabled: the clock code with its analog-register access
     *     (rtc_clk.c, RTC_CLK_FUNC_IN_IRAM; regi2c_ctrl.c; rtc_time.c) and
     *     the watchdog HAL (wdt_hal_iram.c),
     *   - rtc_init.c: its o-code calibration (power-on resets of chips
     *     without the calibration in eFuse) runs the CPU from the crystal
     *     with the PLL off, where ESP-IDF first slows the flash down
     *     (esp_mspi's low-speed mode, not built here); in RAM it does not
     *     touch the flash meanwhile, nor does the stand-in it calls at the
     *     edges (mspi_timing.c),
     *   - the flash driver, which runs with the cache suspended (port/
     *     board_flash_os.c: every interrupt up to XCHAL_EXCM_LEVEL masked, so
     *     nothing else runs then): its OS functions (board_flash_os.c), the
     *     objects ESP-IDF keeps out of flash for it (spi_flash/linker.lf,
     *     esp_hal_mspi/linker.lf, esp_hal_cache/linker.lf: the API, the chip
     *     drivers, the host driver, the HAL's command part, the no-OS cache
     *     suspend, the cache HAL), the list of chip drivers it searches, and
     *     the cache flush it calls (flash_mmap.c's IRAM_ATTR functions, with
     *     the MMU driver's lookup of its mappings in esp_mmu_map.c).  Whole
     *     objects: their functions that only run with the cache on (the
     *     start-up of the chip) cost a little RAM,
     *   - ESP-IDF's MMU driver (esp_mm), which writes the MMU table with the
     *     cache frozen: its IRAM_ATTR functions (esp_mmu_map.c), the MMU HAL
     *     (mmu_hal.c; esp_hal_cache/linker.lf), the freeze itself (the
     *     stand-in esp_cache_utils.c) and the critical section around the
     *     invalidation (esp_cache_msync.c; esp_mm/linker.lf keeps the whole
     *     object out of flash, here the rest of it is in .iram0.fast),
     *   - interrupt entry/exit and context switch, with the execution profile
     *     kit they call (tx_execution_profile.c).
     * The same list appears in .dram0.data for the read-only data of these
     * objects (not for the objects named only for their IRAM_ATTR functions,
     * whose read-only data stays in flash).  Keep the two in step.
     * ldscript/check_ram_refs.py checks after every link that nothing in
     * these objects refers to the flash.  The objects named for their
     * IRAM_ATTR functions come before the generic .iram1 pattern, which
     * would take those first, and after the first pattern, under which the
     * linker's map lists the literals it gathers at the start of the
     * section.
     */
    .iram0.text :
    {
        . = ALIGN(4);
        *(.entry.literal .entry.text)
        *flash_mmap.c.obj(.iram1.literal .iram1 .iram1.*)
        *esp_mmu_map.c.obj(.iram1.literal .iram1 .iram1.*)
        *esp_cache_msync.c.obj(
            .literal.esp_cache_sync_ops_enter_critical_section .text.esp_cache_sync_ops_enter_critical_section
            .literal.esp_cache_sync_ops_exit_critical_section .text.esp_cache_sync_ops_exit_critical_section)
        EXCLUDE_FILE(*libusb_host.a:hcd_dwc.c.obj) *(.iram1.literal .iram1 .iram1.*)
        *startup.c.obj(.literal .literal.* .text .text.*)
        *panic.c.obj(.literal .literal.* .text .text.*)
        *watchdog.c.obj(.literal .literal.* .text .text.*)
        *rtc_clk.c.obj(.literal .literal.* .text .text.*)
        *regi2c_ctrl.c.obj(.literal .literal.* .text .text.*)
        *rtc_time.c.obj(.literal .literal.* .text .text.*)
        *rtc_init.c.obj(.literal .literal.* .text .text.*)
        *wdt_hal_iram.c.obj(.literal .literal.* .text .text.*)
        *esp_rom_cache_esp32s2_esp32s3.c.obj(.literal .literal.* .text .text.*)
        *mspi_timing.c.obj(.literal .literal.* .text .text.*)
        *board_flash_os.c.obj(.literal .literal.* .text .text.*)
        *esp_flash_api.c.obj(.literal .literal.* .text .text.*)
        *spi_flash_chip_drivers.c.obj(.literal .literal.* .text .text.*)
        *spi_flash_chip_generic.c.obj(.literal .literal.* .text .text.*)
        *spi_flash_chip_gd.c.obj(.literal .literal.* .text .text.*)
        *spi_flash_chip_winbond.c.obj(.literal .literal.* .text .text.*)
        *spi_flash_chip_mxic.c.obj(.literal .literal.* .text .text.*)
        *spi_flash_chip_issi.c.obj(.literal .literal.* .text .text.*)
        *spi_flash_chip_boya.c.obj(.literal .literal.* .text .text.*)
        *spi_flash_chip_th.c.obj(.literal .literal.* .text .text.*)
        *memspi_host_driver.c.obj(.literal .literal.* .text .text.*)
        *spi_flash_hal_iram.c.obj(.literal .literal.* .text .text.*)
        *spi_flash_encrypt_hal_iram.c.obj(.literal .literal.* .text .text.*)
        *spi_flash_os_func_noos.c.obj(.literal .literal.* .text .text.*)
        *cache_hal.c.obj(.literal .literal.* .text .text.*)
        *mmu_hal.c.obj(.literal .literal.* .text .text.*)
        *esp_cache_utils.c.obj(.literal .literal.* .text .text.*)
        *xtensa_context.S.obj(.literal .literal.* .text .text.*)
        *xtensa_intr_asm.S.obj(.literal .literal.* .text .text.*)
        *tx_*.S.obj(.literal .literal.* .text .text.*)
        *tx_execution_profile.c.obj(.literal .literal.* .text .text.*)
        *libxt_hal.a:(.literal .literal.* .text .text.*)
        . = ALIGN(4);
        _iram_end = ABSOLUTE(.);
    } > iram_seg

    .dram0.data :
    {
        . = ALIGN(4);
        _data_start = ABSOLUTE(.);
        *(.data .data.* .gnu.linkonce.d.*)
        *(.dram1 .dram1.*)
        *(.data1)
        *(.sdata .sdata.* .gnu.linkonce.s.*)
        *(.sdata2 .sdata2.* .gnu.linkonce.s2.*)
        /* Read-only data of the objects that run from RAM (see .iram0.text). */
        *startup.c.obj(.rodata .rodata.*)
        *panic.c.obj(.rodata .rodata.*)
        *watchdog.c.obj(.rodata .rodata.*)
        *rtc_clk.c.obj(.rodata .rodata.*)
        *regi2c_ctrl.c.obj(.rodata .rodata.*)
        *rtc_time.c.obj(.rodata .rodata.*)
        *rtc_init.c.obj(.rodata .rodata.*)
        *wdt_hal_iram.c.obj(.rodata .rodata.*)
        *esp_rom_cache_esp32s2_esp32s3.c.obj(.rodata .rodata.*)
        *mspi_timing.c.obj(.rodata .rodata.*)
        *board_flash_os.c.obj(.rodata .rodata.*)
        *esp_flash_api.c.obj(.rodata .rodata.*)
        *spi_flash_chip_drivers.c.obj(.rodata .rodata.*)
        *spi_flash_chip_generic.c.obj(.rodata .rodata.*)
        *spi_flash_chip_gd.c.obj(.rodata .rodata.*)
        *spi_flash_chip_winbond.c.obj(.rodata .rodata.*)
        *spi_flash_chip_mxic.c.obj(.rodata .rodata.*)
        *spi_flash_chip_issi.c.obj(.rodata .rodata.*)
        *spi_flash_chip_boya.c.obj(.rodata .rodata.*)
        *spi_flash_chip_th.c.obj(.rodata .rodata.*)
        *memspi_host_driver.c.obj(.rodata .rodata.*)
        *spi_flash_hal_iram.c.obj(.rodata .rodata.*)
        *spi_flash_encrypt_hal_iram.c.obj(.rodata .rodata.*)
        *spi_flash_os_func_noos.c.obj(.rodata .rodata.*)
        *cache_hal.c.obj(.rodata .rodata.*)
        *mmu_hal.c.obj(.rodata .rodata.*)
        *esp_cache_utils.c.obj(.rodata .rodata.*)
        *xtensa_context.S.obj(.rodata .rodata.*)
        *xtensa_intr_asm.S.obj(.rodata .rodata.*)
        *xtensa_vectors.S.obj(.rodata .rodata.*)
        *tx_*.S.obj(.rodata .rodata.*)
        *tx_execution_profile.c.obj(.rodata .rodata.*)
        *libxt_hal.a:(.rodata .rodata.*)
        . = ALIGN(4);
    } > dram_seg

    .dram0.bss (NOLOAD) :
    {
        . = ALIGN(8);
        _bss_start = ABSOLUTE(.);
        *(.dynsbss .sbss .sbss.* .gnu.linkonce.sb.*)
        *(.scommon .sbss2 .sbss2.* .gnu.linkonce.sb2.*)
        *(.dynbss .bss .bss.* .gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(8);
        _bss_end = ABSOLUTE(.);
    } > dram_seg

    /*
     * Not loaded and not cleared by _start: what is here keeps its contents
     * across a reset that leaves the RAM powered (the RAM log, and the record
     * of a fatal error of port/panic.c).  It has to stay below what the ROM
     * and the bootloader write (NOINIT_END, see the top).
     */
    .dram0.noinit (NOLOAD) :
    {
        . = ALIGN(8);
        *(.noinit .noinit.*)
        . = ALIGN(8);
    } > dram_seg

    ASSERT(ADDR(.dram0.noinit) + SIZEOF(.dram0.noinit) <= NOINIT_END,
           ".dram0.noinit reaches memory the ROM or the bootloader writes during a reset")

    /* The heap starts behind the image's data (see the top). */
    _heap_start = ABSOLUTE(.);

    /*
     * RTC FAST memory: only what ESP-IDF keeps at fixed addresses at its top
     * (esp_clk.c's RTC timer record).  Nothing is linked into the rest
     * (wake stubs, RTC_DATA_ATTR, RTC_NOINIT_ATTR), which is heap; the
     * reservation of ESP-IDF's .rtc.force_fast for it is therefore empty.
     */
    _rtc_force_fast_end = ABSOLUTE(0x600FE000);

    .rtc_reserved (NOLOAD) :
    {
        . = ALIGN(4);
        _rtc_reserved_start = ABSOLUTE(.);
        *(.rtc_timer_data_in_rtc_mem .rtc_timer_data_in_rtc_mem.*)
        _rtc_reserved_end = ABSOLUTE(.);
    } > rtc_reserved_seg

    /*
     * Everything else executes in place from flash.  That includes the
     * sections of the Wi-Fi libraries that .iram0.fast does not take
     * (.wifislpiram: ESP-IDF's CONFIG_ESP_WIFI_SLP_IRAM_OPT, off by
     * default).
     *
     * _instruction_reserved_start/_end here and _rodata_reserved_start/_end
     * in .flash.rodata are ESP-IDF's names for the two ranges that the
     * bootloader maps: ESP-IDF's MMU driver (esp_mmu_map_init(), port/
     * startup.c) keeps their pages out of the ones it hands out.
     */
    .flash.text :
    {
        _flash_text_start = ABSOLUTE(.);
        _instruction_reserved_start = ABSOLUTE(.);
        *(.literal .text .literal.* .text.*)
        *(.wifi0iram .wifi0iram.* .wifirxiram .wifirxiram.* .wifislprxiram .wifislprxiram.*)
        *(.wifislpiram .wifislpiram.* .wifiorslpiram .wifiorslpiram.* .wifiextrairam .wifiextrairam.*)
        /*
         * The USB host's HCD functions that esp-usb marks IRAM_ATTR
         * (hcd_dwc.c: filling a pipe's DMA descriptors and starting its
         * channel), left out of .iram0.text: they call the DWC2 HAL in
         * flash and run in the controller's interrupt handler, which is in
         * flash too, and nothing of the USB host runs while the cache is
         * off (its interrupt is masked then).  In RAM they would take
         * 1.4 KiB.
         */
        *libusb_host.a:hcd_dwc.c.obj(.iram1.literal .iram1 .iram1.*)
        *(.stub .gnu.warning .gnu.linkonce.literal.* .gnu.linkonce.t.*)
        *(.fini.literal .fini .init.literal .init)
        . = ALIGN(4);
        _flash_text_end = ABSOLUTE(.);
        _instruction_reserved_end = ABSOLUTE(.);
    } > irom_seg

    /*
     * First MMU page after the ones .flash.text occupies, in the data window,
     * 0x20 bytes in: where the first segment of the image file starts.
     */
    _flash_rodata_vma = ORIGIN(drom_seg) + ALIGN(_flash_text_end - ORIGIN(irom_seg), MMU_PAGE)
                        + IMAGE_HEADERS;

    .flash.rodata _flash_rodata_vma :
    {
        _flash_rodata_start = ABSOLUTE(.);
        _rodata_reserved_start = ABSOLUTE(.);
        /* esp_app_desc_t, first: the bootloader and esptool look for it here. */
        KEEP(*(.rodata_desc .rodata_desc.*))
        *(.rodata .rodata.* .gnu.linkonce.r.*)
        *(.rodata1)
        /* The Wi-Fi libraries' log formats, one section per level. */
        *(.rodata_wlog_error .rodata_wlog_error.* .rodata_wlog_warning .rodata_wlog_warning.*)
        *(.rodata_wlog_info .rodata_wlog_info.* .rodata_wlog_debug .rodata_wlog_debug.*)
        *(.rodata_wlog_verbose .rodata_wlog_verbose.*)
        /*
         * The socket implementations of the shell's socket layer
         * (NET_SOCKET_OFFLOAD_REGISTER(), its <zephyr/net/socket.h>), in the
         * order of their names, that is of their priority.
         */
        . = ALIGN(4);
        _net_socket_register_list_start = ABSOLUTE(.);
        KEEP(*(SORT_BY_NAME(._net_socket_register.static.*)))
        _net_socket_register_list_end = ABSOLUTE(.);
        /*
         * The regions the heap must leave alone
         * (SOC_RESERVE_MEMORY_REGION(); heap_caps_init() sorts them).
         */
        . = ALIGN(4);
        soc_reserved_memory_region_start = ABSOLUTE(.);
        KEEP(*(.reserved_memory_address))
        soc_reserved_memory_region_end = ABSOLUTE(.);
        /*
         * The names of the esp_err_t codes that esp_err_to_name() looks up,
         * as esp_common/linker.lf lays them out.  ESP-IDF's build generates
         * the entries from the components' headers; this build does not, so
         * the table is empty and esp_err_to_name() answers "ERROR" for any
         * code.
         */
        . = ALIGN(4);
        _esp_err_msg_tbl_start = ABSOLUTE(.);
        KEEP(*(SORT_BY_NAME(.esp_err_msg_tbl*)))
        _esp_err_msg_tbl_end = ABSOLUTE(.);
        /*
         * ESP-IDF's start-up hooks (esp_system's esp_sys_event_list.inc:
         * ESP_SYSTEM_INIT_FN() and friends), one table per event, in the
         * order of their priority, as esp_system/linker.lf lays them out.
         * port/startup.c runs them at the points where ESP-IDF's start-up
         * does.  Not esp_flash_spi_init.c's (ESP-IDF's init_flash, which
         * installs ESP-IDF's OS functions for the flash driver, not built
         * here): port/board_flash.c's hook, at the same priority, sets the
         * driver up instead, and --gc-sections drops the left-out one.
         */
        . = ALIGN(4);
        _esysev_initc_start = ABSOLUTE(.);
        KEEP(EXCLUDE_FILE(*esp_flash_spi_init.c.obj) *(SORT_BY_INIT_PRIORITY(.esysev_initc.*)))
        _esysev_initc_end = ABSOLUTE(.);
        _esysev_inits_start = ABSOLUTE(.);
        KEEP(*(SORT_BY_INIT_PRIORITY(.esysev_inits.*)))
        _esysev_inits_end = ABSOLUTE(.);
        _esysev_presc_start = ABSOLUTE(.);
        KEEP(*(SORT_BY_INIT_PRIORITY(.esysev_presc.*)))
        _esysev_presc_end = ABSOLUTE(.);
        _esysev_pream_start = ABSOLUTE(.);
        KEEP(*(SORT_BY_INIT_PRIORITY(.esysev_pream.*)))
        _esysev_pream_end = ABSOLUTE(.);
        . = ALIGN(4);
        __preinit_array_start = ABSOLUTE(.);
        KEEP(*(.preinit_array))
        __preinit_array_end = ABSOLUTE(.);
        /*
         * Global constructors (__libc_init_array(), called by _start).  GCC
         * for Xtensa puts them in .ctors; ESP-IDF's linker script collects
         * those into the same table.
         */
        __init_array_start = ABSOLUTE(.);
        KEEP(*(SORT(.init_array.*) .init_array))
        KEEP(*(SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.ctors))
        __init_array_end = ABSOLUTE(.);
        . = ALIGN(4);
        _flash_rodata_end = ABSOLUTE(.);
        _rodata_reserved_end = ABSOLUTE(.);
    } > drom_seg

    ASSERT(esp_app_desc == _flash_rodata_start,
           "esp_app_desc must be the first thing in .flash.rodata")

    /* Not loaded: debugging information, and the assembler's property tables. */
    .debug_aranges  0 : { *(.debug_aranges) }
    .debug_info     0 : { *(.debug_info .gnu.linkonce.wi.*) }
    .debug_abbrev   0 : { *(.debug_abbrev) }
    .debug_line     0 : { *(.debug_line .debug_line.* .debug_line_end) }
    .debug_frame    0 : { *(.debug_frame) }
    .debug_str      0 : { *(.debug_str) }
    .debug_loc      0 : { *(.debug_loc) }
    .debug_ranges   0 : { *(.debug_ranges) }
    .debug_line_str 0 : { *(.debug_line_str) }
    .debug_loclists 0 : { *(.debug_loclists) }
    .debug_rnglists 0 : { *(.debug_rnglists) }
    .xt.lit         0 : { *(.xt.lit .xt.lit.* .gnu.linkonce.p.*) }
    .xt.prop        0 : { *(.xt.prop .xt.prop.* .gnu.linkonce.prop.*) }

    /*
     * Also discarded, as in ESP-IDF without C++ exceptions: the unwind tables
     * (.eh_frame of libgcc's helpers).
     */
    /DISCARD/ :
    {
        *(.xtensa.info)
        *(.comment)
        *(.note.GNU-stack)
        *(.eh_frame .eh_frame_hdr)
    }
}
