void *volatile escaped_block;

unsigned rejected_escape(unsigned selector, unsigned *storage) {
    switch (selector) {
    case 1:
        *storage += 1;
        break;
#if __SIZEOF_POINTER__ == 8
    case 8:
    extra:
        // GNU C permits taking this address. Removing its native block from
        // the other domain cannot be justified by the closed-arm proof.
        escaped_block = &&extra;
        *storage += 8;
        break;
#endif
    default:
        *storage += 2;
        break;
    }
    return *storage;
}
