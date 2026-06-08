.section ".text.boot"
.global _start
_start:
    la sp, _end
    /*
     * OpenSBI / bootloader passes:
     *   a0 = hartid
     *   a1 = dtb_pa
     *
     * setup_vm() is a C call, so a0/a1 may be clobbered.
     * Save them in callee-saved registers.
     */
    mv s0, a0
    mv s1, a1

    /*
     * Build page tables and enable satp.
     * After this, both low identity mapping and high-half mapping exist.
     */
    mv a0, s1//setup_vm need to see dtb for permission control
    call setup_vm

    /*
     * Jump from low physical execution to high virtual execution.
     */
    la t0, 1f
    li t1, 0xffffffc000000000
    add t0, t0, t1
    jr t0

.align 2
1:
    /*
     * Now PC is high VA.
     * From this point, all kernel pointers should be high virtual addresses.
     */
    la sp, _end

    /*
     * Set trap vector after high jump.
     * If you set stvec before VM and later drop identity mapping,
     * traps may jump to an unmapped low address.
     */
    la a2, handle_exception
    csrw stvec, a2

    /*
     * Clear BSS using high virtual addresses.
     */
    la a3, __bss_start
    la a4, __bss_stop
    ble a4, a3, clear_bss_done

clear_bss:
    sd zero, (a3)
    add a3, a3, 8
    blt a3, a4, clear_bss

clear_bss_done:
    /*
     * Now that we are safely executing in high VA,
     * remove temporary VA = PA mapping.
     */
    call drop_identity_map

    /*
     * Restore start_kernel arguments.
     */
    mv a0, s0
    mv a1, s1

    tail start_kernel

.global handle_exception
.align 2 # align to 2^2 = 4 bytes, RISC-V instruction size is 4 bytes
/*
CPU prepares on trap entry:
  sepc         = address of trapped supervisor instruction
  scause       = trap cause
  stval        = extra fault info
  sstatus.SPP  = 0/1              // previous mode was U-mode/S-mode
  sstatus.SPIE = old sstatus.SIE
  sstatus.SIE  = 0              // Supervisor Interrupt Enable, if 0, all interrupt cannot trigger
  pc           = stvec
  privilege    = S-mode         // still trap into supervisor handler
*/
/*
If trap entered from U-mode, before trap happened:
  sscratch = kernel_sp
  sp       = user_sp
  privilege= U-mode
*/
/*
If trap entered from S-mode:, before trap happened:
  sscratch = 0
  sp       = kernel_sp
  privilege= S-mode
*/
handle_exception:
    /*
     * Retrieve kernel sp from sscratch and
     * save user sp to sscratch
     */
    csrrw sp, sscratch, sp #csrrw rd, csr, rs1: read old csr, write rs1 into that csr, put the old csr value into rd
    bnez sp, save_context # check if sscratch is non-zero before swap
    # sscratch is non-zero: kernel stack ready, likely trap came from user mode
    # sscratch is zero: likely already in kernel mode, so there is no separate user→kernel stack switch to do here

    # before swap: sp = kernel_sp
    #              sscratch = 0
    # in the case of kernel to kernel mode, read back to kernel stack pointer
    csrr sp, sscratch # read sscratch into sp, which is the kernel_sp

#now sp is kernel_sp
#saves the current trap context into a trap frame on the stack
save_context:
    /* Save context */
    addi sp, sp, -8 * 35 #move stack pointer downward, 280 bytes, [sp + 0]  ... [sp + 8*34] is area for save
    #GPR
    # store double
    sd ra,  8 *  0(sp)
    sd gp,  8 *  2(sp)
    sd tp,  8 *  3(sp)
    sd t0,  8 *  4(sp)
    sd t1,  8 *  5(sp)
    sd t2,  8 *  6(sp)
    sd s0,  8 *  7(sp)
    sd s1,  8 *  8(sp)
    sd a0,  8 *  9(sp)
    sd a1,  8 * 10(sp)
    sd a2,  8 * 11(sp)
    sd a3,  8 * 12(sp)
    sd a4,  8 * 13(sp)
    sd a5,  8 * 14(sp)
    sd a6,  8 * 15(sp)
    sd a7,  8 * 16(sp)
    sd s2,  8 * 17(sp)
    sd s3,  8 * 18(sp)
    sd s4,  8 * 19(sp)
    sd s5,  8 * 20(sp)
    sd s6,  8 * 21(sp)
    sd s7,  8 * 22(sp)
    sd s8,  8 * 23(sp)
    sd s9,  8 * 24(sp)
    sd s10, 8 * 25(sp)
    sd s11, 8 * 26(sp)
    sd t3,  8 * 27(sp)
    sd t4,  8 * 28(sp)
    sd t5,  8 * 29(sp)
    sd t6,  8 * 30(sp)
    # trap related CSRs
    #csr read
    csrr s0, sscratch # trap from U-mode -> sscratch now holds user_sp/ trap from S-mode -> sscratch now holds kernel_sp
    csrr s1, sepc # trapped instruction pc
    csrr s2, sstatus # SPP = 0 -> previous mode was U/ SPP = 1 -> previous mode was S, SIE = 0, SPIE = old SIE
    csrr s3, scause
    csrr s4, stval
    sd s0,  8 *  1(sp)
    sd s1,  8 * 31(sp)
    sd s2,  8 * 32(sp)
    sd s3,  8 * 33(sp)
    sd s4,  8 * 34(sp)
    # clears sscratch to 0, mark that we are in kernel trap handling
    csrw sscratch, zero 
    # move: addi a0, sp, 0
    move a0, sp # passes the base address of that saved frame, argument placed at a0
    call do_trap # call the function, sp alreay placed in a0

.global ret_from_exception
//function return, kernel_sp still points to trap frame low addr ( like max addr for trap frame)
ret_from_exception:

    mv a0, sp
    call handle_signal_if_needed

    ld s0,  8 * 32(sp) # loads sstatus from saved trap frame
    andi s0, s0, (1 << 8) # check bit 8, SPP, Supervisor Previous Privilege, 1: return to S-mode, 0: return to U-mode
    bnez s0, 1f # if SPP == 1, branch to 1, we do not need to prepare sscratch for a future user→kernel stack swap right now
    #SPP == 0
    addi s0, sp, 8 * 35 # sp now points to base (high addr) of trap frame, + 8*35 is the stack pointer after popping this frame, which is the kernel stack pointer
    //this prepares for next swap, kernel sp would point to the bottom of the stack
    csrw sscratch, s0 #saves stack pointer (now s0 holds) to sscratch, so next trap from U-mode goes to correct logic
1:
    /* Restore context */
    //load double
    ld s1,  8 * 31(sp) # sepc, where to resume
    ld s2,  8 * 32(sp) # sstatus, what mode to resume in
    csrw sepc, s1
    csrw sstatus, s2
    ld ra,  8 *  0(sp)
    ld gp,  8 *  2(sp)
    ld tp,  8 *  3(sp)
    ld t0,  8 *  4(sp)
    ld t1,  8 *  5(sp)
    ld t2,  8 *  6(sp)
    ld s0,  8 *  7(sp)
    ld s1,  8 *  8(sp)
    ld a0,  8 *  9(sp)
    ld a1,  8 * 10(sp)
    ld a2,  8 * 11(sp)
    ld a3,  8 * 12(sp)
    ld a4,  8 * 13(sp)
    ld a5,  8 * 14(sp)
    ld a6,  8 * 15(sp)
    ld a7,  8 * 16(sp)
    ld s2,  8 * 17(sp)
    ld s3,  8 * 18(sp)
    ld s4,  8 * 19(sp)
    ld s5,  8 * 20(sp)
    ld s6,  8 * 21(sp)
    ld s7,  8 * 22(sp)
    ld s8,  8 * 23(sp)
    ld s9,  8 * 24(sp)
    ld s10, 8 * 25(sp)
    ld s11, 8 * 26(sp)
    ld t3,  8 * 27(sp)
    ld t4,  8 * 28(sp)
    ld t5,  8 * 29(sp)
    ld t6,  8 * 30(sp)
    ld sp,  8 *  1(sp) # sp loaded last because we need to use it for loading other saved registers
    sret # return from trap, CPU use the sepc, sstatus that we just restore 

.global switch_to
switch_to:
    // a0 = prev
    // a1 = next
    sd ra,  8 *  0(a0)
    sd sp,  8 *  1(a0)
    sd s0,  8 *  2(a0)
    sd s1,  8 *  3(a0)
    sd s2,  8 *  4(a0)
    sd s3,  8 *  5(a0)
    sd s4,  8 *  6(a0)
    sd s5,  8 *  7(a0)
    sd s6,  8 *  8(a0)
    sd s7,  8 *  9(a0)
    sd s8,  8 * 10(a0)
    sd s9,  8 * 11(a0)
    sd s10, 8 * 12(a0)
    sd s11, 8 * 13(a0)

    mv tp, a1

    ld ra,  8 *  0(a1)
    ld sp,  8 *  1(a1)
    ld s0,  8 *  2(a1)
    ld s1,  8 *  3(a1)
    ld s2,  8 *  4(a1)
    ld s3,  8 *  5(a1)
    ld s4,  8 *  6(a1)
    ld s5,  8 *  7(a1)
    ld s6,  8 *  8(a1)
    ld s7,  8 *  9(a1)
    ld s8,  8 * 10(a1)
    ld s9,  8 * 11(a1)
    ld s10, 8 * 12(a1)
    ld s11, 8 * 13(a1)

    ret

//advance part trampoline
.global __sigreturn_trampoline_start
.global __sigreturn_trampoline_end
.align 2//riscv isntruction are 32 bits
__sigreturn_trampoline_start:
    .option push//save the current assembler options
    .option norvc//disables compressed RISC-V instructions, so the assembler will emit normal 32-bit instructions, not 16-bit compressed ones
    li a7, 11
    ecall
    .option pop//restore saved assembler options
__sigreturn_trampoline_end: