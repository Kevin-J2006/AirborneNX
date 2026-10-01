.section .text.so_unresolved_stub, "ax", %progbits
.global so_unresolved_stub
.type so_unresolved_stub, %function
so_unresolved_stub:
    mov x0, x16
    b so_unresolved_handler

// ---------------------------------------------------------------------------
// GL call serialization thunks.
//
// Mesa/nouveau on Switch shares one GPU command stream between contexts and
// does not lock it, while the engine drives a render context and a loader
// context from different threads. Every GL/EGL import is routed through one of
// these thunks, which take a global lock around the real call.
//
// gl_thunks_start + 8*n is the entry for slot n; gl_real_table[n] holds the
// real function. Up to four stack-passed arguments are forwarded.
// ---------------------------------------------------------------------------
.section .text.gl_thunks, "ax", %progbits
.global gl_thunks_start
.type gl_thunks_start, %function
.balign 8
gl_thunks_start:
.set gl_thunk_index, 0
.rept 256
    mov x16, #gl_thunk_index
    b gl_thunk_common
    .set gl_thunk_index, gl_thunk_index + 1
.endr

gl_thunk_common:
    sub sp, sp, #0x100
    stp x29, x30, [sp, #0x20]
    stp x0, x1, [sp, #0x30]
    stp x2, x3, [sp, #0x40]
    stp x4, x5, [sp, #0x50]
    stp x6, x7, [sp, #0x60]
    stp x16, x8, [sp, #0x70]
    stp q0, q1, [sp, #0x80]
    stp q2, q3, [sp, #0xA0]
    stp q4, q5, [sp, #0xC0]
    stp q6, q7, [sp, #0xE0]
    // forward the caller's stack arguments
    ldp x9, x10, [sp, #0x100]
    stp x9, x10, [sp]
    ldp x9, x10, [sp, #0x110]
    stp x9, x10, [sp, #0x10]

    bl gl_lock_enter

    ldp x0, x1, [sp, #0x30]
    ldp x2, x3, [sp, #0x40]
    ldp x4, x5, [sp, #0x50]
    ldp x6, x7, [sp, #0x60]
    ldp x16, x8, [sp, #0x70]
    ldp q0, q1, [sp, #0x80]
    ldp q2, q3, [sp, #0xA0]
    ldp q4, q5, [sp, #0xC0]
    ldp q6, q7, [sp, #0xE0]
    adrp x17, gl_real_table
    add x17, x17, :lo12:gl_real_table
    ldr x17, [x17, x16, lsl #3]
    blr x17

    str x0, [sp, #0x30]
    str q0, [sp, #0x80]
    bl gl_lock_leave
    ldr x0, [sp, #0x30]
    ldr q0, [sp, #0x80]

    ldp x29, x30, [sp, #0x20]
    add sp, sp, #0x100
    ret
