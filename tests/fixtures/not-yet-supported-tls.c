/* Valid Clang C, intentionally outside this partial Sela checkpoint.
 * Move this to positive coverage when TLS storage/lowering is implemented. */
_Thread_local int sela_tls_probe;

int main(void) {
    return ++sela_tls_probe == 1 ? 0 : 1;
}
