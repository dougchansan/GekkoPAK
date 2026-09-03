.syntax unified
.arm
.global _start
.type _start,%function

.equ MB,          0x0FFFC000
.equ MAGIC,       0x4B504B47
.equ VERSION,     0x00010000
.equ STATE_REQ,   1
.equ STATE_RESP,  2
.equ PASS_MAGIC,  0x53534150
.equ FAIL_MAGIC,  0x4C494146

.equ CMD_HELLO,    1
.equ CMD_GET_CAPS, 2
.equ CMD_ALLOC,    3
.equ CMD_UPLOAD,   4
.equ CMD_SUBMIT,   5
.equ CMD_POLL,     6
.equ CMD_COLLECT,  7
.equ CMD_FREE,     8
.equ CMD_COMPLETE, 9

/* mailbox offsets */
.equ O_MAGIC,    0x00
.equ O_VERSION,  0x04
.equ O_SEQ,      0x08
.equ O_CMD,      0x0C
.equ O_STATE,    0x10
.equ O_RESULT,   0x14
.equ O_ARG0,     0x18
.equ O_ARG1,     0x1C
.equ O_ARG2,     0x20
.equ O_ARG3,     0x24
.equ O_OUT0,     0x28
.equ O_OUT1,     0x2C
.equ O_OUT2,     0x30
.equ O_OUT3,     0x34
.equ O_PLEN,     0x38
.equ O_PAYLOAD,  0x40
.equ O_SUMMARY,  0x100

_start:
    ldr r10, =MB
    ldr r0, =MAGIC
    str r0, [r10, #O_MAGIC]
    ldr r0, =VERSION
    str r0, [r10, #O_VERSION]
    mov r8, #0

    /* HELLO */
    mov r0, #CMD_HELLO
    mov r1, #0
    mov r2, #0
    mov r3, #0
    mov r4, #0
    bl issue
    ldr r0, [r10, #O_OUT0]
    str r0, [r10, #(O_SUMMARY + 4)]

    /* GET_CAPS */
    mov r0, #CMD_GET_CAPS
    mov r1, #0
    mov r2, #0
    mov r3, #0
    mov r4, #0
    bl issue
    ldr r0, [r10, #O_OUT0]
    str r0, [r10, #(O_SUMMARY + 8)]
    ldr r0, [r10, #O_OUT1]
    str r0, [r10, #(O_SUMMARY + 12)]

    /* ALLOC 4096 bytes */
    mov r0, #CMD_ALLOC
    mov r1, #4096
    mov r2, #0
    mov r3, #0
    mov r4, #0
    bl issue
    ldr r7, [r10, #O_OUT0]
    str r7, [r10, #(O_SUMMARY + 16)]

    /* Put 16 bytes of real guest data in the mailbox payload area. */
    ldr r0, =0x11223344
    str r0, [r10, #(O_PAYLOAD + 0)]
    ldr r0, =0x55667788
    str r0, [r10, #(O_PAYLOAD + 4)]
    ldr r0, =0xAABBCCDD
    str r0, [r10, #(O_PAYLOAD + 8)]
    ldr r0, =0x0BADF00D
    str r0, [r10, #(O_PAYLOAD + 12)]

    /* UPLOAD(handle, offset=0), payload_len=16 */
    mov r0, #16
    str r0, [r10, #O_PLEN]
    mov r0, #CMD_UPLOAD
    mov r1, r7
    mov r2, #0
    mov r3, #0
    mov r4, #0
    bl issue_keep_plen
    mov r0, #0
    str r0, [r10, #O_PLEN]

    /* SUBMIT(handle, DSP kernel=1, 250000 ops, 3500 us ARM11 baseline) */
    mov r0, #CMD_SUBMIT
    mov r1, r7
    mov r2, #1
    ldr r3, =250000
    ldr r4, =3500
    bl issue
    ldr r6, [r10, #O_OUT0]
    str r6, [r10, #(O_SUMMARY + 20)]

poll_again:
    mov r0, #CMD_POLL
    mov r1, r6
    mov r2, #0
    mov r3, #0
    mov r4, #0
    bl issue
    ldr r0, [r10, #O_OUT0]
    cmp r0, #0
    beq poll_again

    /* COLLECT(job) */
    mov r0, #CMD_COLLECT
    mov r1, r6
    mov r2, #0
    mov r3, #0
    mov r4, #0
    bl issue
    ldr r0, [r10, #O_OUT0]
    str r0, [r10, #(O_SUMMARY + 24)]
    ldr r0, [r10, #O_OUT1]
    str r0, [r10, #(O_SUMMARY + 28)]
    ldr r0, [r10, #O_OUT2]
    str r0, [r10, #(O_SUMMARY + 32)]

    /* FREE allocation */
    mov r0, #CMD_FREE
    mov r1, r7
    mov r2, #0
    mov r3, #0
    mov r4, #0
    bl issue

    ldr r0, =PASS_MAGIC
    str r0, [r10, #O_SUMMARY]

    /* COMPLETE tells the host to print/terminate. */
    mov r0, #CMD_COMPLETE
    mov r1, #0
    mov r2, #0
    mov r3, #0
    mov r4, #0
    bl issue

finished:
    b finished

/* r0 cmd, r1-r4 args. Clears payload length. */
issue:
    mov r5, #0
    str r5, [r10, #O_PLEN]
issue_keep_plen:
    str r0, [r10, #O_CMD]
    str r1, [r10, #O_ARG0]
    str r2, [r10, #O_ARG1]
    str r3, [r10, #O_ARG2]
    str r4, [r10, #O_ARG3]
    mov r5, #0
    str r5, [r10, #O_RESULT]
    add r8, r8, #1
    str r8, [r10, #O_SEQ]
    mov r5, #STATE_REQ
    str r5, [r10, #O_STATE]
wait_response:
    ldr r5, [r10, #O_STATE]
    cmp r5, #STATE_RESP
    bne wait_response
    ldr r5, [r10, #O_RESULT]
    cmp r5, #0
    bne failed
    bx lr

failed:
    ldr r0, =FAIL_MAGIC
    str r0, [r10, #O_SUMMARY]
fail_spin:
    b fail_spin

.size _start, .-_start
