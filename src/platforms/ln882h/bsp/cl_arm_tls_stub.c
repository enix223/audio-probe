#ifdef CONFIG_IOT_PLATFORM_LN882H

// The Ubuntu arm-none-eabi newlib multilib does not provide
// __aeabi_read_tp, while libstdc++ may reference it from the exception/TLS
// path. The recorder never throws and runs one scheduler thread, so a small
// static thread pointer is sufficient to satisfy the reference. The official
// ARM toolchain provides a strong definition; this weak stub is then ignored.
__attribute__((weak)) void *__aeabi_read_tp(void) {
  static unsigned char tls_block[256];
  return tls_block;
}

#endif  // CONFIG_IOT_PLATFORM_LN882H
