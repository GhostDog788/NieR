int native_backend_failure(void) {
    __asm__ volatile("sela_invalid_native_instruction");
    return 0;
}
