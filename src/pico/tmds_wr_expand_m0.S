// wr_expand_line for the Cortex-M0+ (ARMv6-M Thumb). tmds_wr.h defines the token format and
// tmds_wr.c's wr_expand_line_c is the reference this must match: scanout_init compares the two
// on synthetic lines at boot and uses the C version if they differ.
//
// void wr_expand_line(const uint8_t *tok, uint32_t nbytes, uint32_t *out, uint32_t width,
//                     const uint32_t *table)
//
// r0 token pointer   r1/r2/r3 lane 0/1/2 output   r4 table   r5-r7 scratch
// r8 token end       r9 lane-0 end (= lane-1 start)
//
// A literal word costs 12 instructions plus the loop branch; a run word costs 8. Writes are always
// bounded by the lane-0 end. Literal offsets are not masked: a corrupt one still reads SRAM (the
// table sits well inside it), never flash or peripherals.

    .syntax unified
    .cpu cortex-m0plus
    .thumb

    .section .time_critical.wr_expand_line, "ax", %progbits
    .global wr_expand_line
    .type wr_expand_line, %function
    .thumb_func
    .align 2
wr_expand_line:
    push {r4-r7, lr}
    mov r4, r8
    mov r5, r9
    push {r4, r5}
    ldr r4, [sp, #28]          // table: the fifth argument, above the 28 bytes just pushed
    movs r5, #1
    bics r1, r5                // whole tokens only
    adds r1, r0
    mov r8, r1                 // token end
    lsls r3, r3, #1            // lane stride in bytes: width / 2 words * 4
    mov r1, r2                 // lane 0
    adds r2, r1, r3            // lane 1
    mov r9, r2                 // lane-0 end
    adds r3, r2, r3            // lane 2

.Ltoken:
    mov r5, r9
    cmp r1, r5
    bhs .Lpad
    mov r5, r8
    cmp r0, r5
    bhs .Lpad
    ldrh r5, [r0]
    adds r0, #2
    lsls r6, r5, #16
    bmi .Lliteral

    // RUN: bits 14..9 = count - 1, bits 8..0 = pair id
    lsls r6, r5, #23
    lsrs r6, r6, #19           // id << 4: byte offset of the entry
    adds r6, r4
    lsls r7, r5, #17
    lsrs r7, r7, #26
    adds r7, #1                // count
    mov r5, r9
    subs r5, r1
    lsrs r5, r5, #2            // words left on the line (at least one)
    cmp r7, r5
    bls 1f
    mov r7, r5
1:
    ldr r5, [r6, #0]
    ldr r4, [r6, #8]           // the table register is borrowed for lane 2's word
    ldr r6, [r6, #4]
.Lrun:
    str r5, [r1]
    str r6, [r2]
    str r4, [r3]
    adds r1, #4
    adds r2, #4
    adds r3, #4
    subs r7, #1
    bne .Lrun
    ldr r4, [sp, #28]
    b .Ltoken

.Lliteral:
    lsls r7, r5, #17
    lsrs r7, r7, #17           // n
    mov r5, r9
    subs r5, r1
    lsrs r5, r5, #2
    cmp r7, r5
    bls 2f
    mov r7, r5                 // no more words than the line has left
2:
    mov r5, r8
    subs r5, r0
    lsrs r5, r5, #1
    cmp r7, r5
    bls 3f
    mov r7, r5                 // no more ids than the stream holds
3:
    cmp r7, #0
    beq .Ltoken
.Lword:
    ldrh r5, [r0]
    adds r0, #2
    adds r5, r4
    ldr r6, [r5, #0]
    str r6, [r1]
    ldr r6, [r5, #4]
    str r6, [r2]
    ldr r6, [r5, #8]
    str r6, [r3]
    adds r1, #4
    adds r2, #4
    adds r3, #4
    subs r7, #1
    bne .Lword
    b .Ltoken

.Lpad:                          // a short or empty stream: the rest of the line is id 0
    ldr r5, [r4, #0]
    ldr r6, [r4, #4]
    ldr r7, [r4, #8]
    mov r4, r9
.Lpadword:
    cmp r1, r4
    bhs .Ldone
    str r5, [r1]
    str r6, [r2]
    str r7, [r3]
    adds r1, #4
    adds r2, #4
    adds r3, #4
    b .Lpadword

.Ldone:
    pop {r4, r5}
    mov r8, r4
    mov r9, r5
    pop {r4-r7, pc}
    .size wr_expand_line, . - wr_expand_line
