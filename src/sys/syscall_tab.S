.section .data

.global syscall_table
syscall_table:
    .quad syscall_debug       // 0
    .quad syscall_mmap        // 1
    .quad syscall_openat      // 2
    .quad syscall_close       // 3
    .quad syscall_read        // 4
    .quad syscall_write       // 5
    .quad syscall_seek        // 6
    .quad syscall_set_fs_base // 7
    .quad syscall_set_gs_base // 8
    .quad syscall_stat        // 9
    .quad syscall_fcntl       // 10
    .quad syscall_dup3        // 11
    .quad syscall_ioctl       // 12
    .quad syscall_fork        // 13
    .quad syscall_exec        // 14
    .quad syscall_getpid      // 15
    .quad syscall_waitpid     // 16
    .quad syscall_exit        // 17
    .quad syscall_getcwd      // 18
    .quad syscall_chdir       // 19
    .quad syscall_munmap      // 20
    .quad syscall_pipe        // 21
    .quad syscall_readlinkat  // 22
    .quad syscall_linkat      // 23
    .quad syscall_unlinkat    // 24
    .quad syscall_readdir     // 25
    .quad syscall_uname       // 26
    .quad syscall_futex_wait  // 27
    .quad syscall_futex_wake  // 28
    .quad syscall_mkdirat     // 29
    .quad syscall_fchmodat    // 30
    .quad syscall_sleep       // 31
    .quad syscall_ppoll       // 32
    .quad syscall_umask       // 33
    .quad syscall_mprotect    // 34
    .quad syscall_getclock    // 35
    .quad syscall_socket      // 36
    .quad syscall_bind        // 37
    .quad syscall_connect     // 38
    .quad syscall_listen      // 39
    .quad syscall_accept      // 40
    .quad syscall_getpeername // 41
    .quad syscall_recvmsg     // 42
    .quad syscall_new_thread  // 43
    .quad syscall_exit_thread // 44
    .quad syscall_sendmsg     // 45
    .quad syscall_socketpair  // 46
    .quad syscall_getsockopt  // 47
    .quad syscall_setsockopt  // 48
    .quad syscall_getsockname // 49
syscall_table_end:

.global syscall_count
syscall_count:
    .quad (syscall_table_end - syscall_table) / 8
