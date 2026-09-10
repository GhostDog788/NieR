volatile unsigned external_condition;

unsigned rejected_branch(unsigned selector, unsigned *storage) {
    switch (selector) {
    case 1:
        *storage += 1;
        break;
#if __SIZEOF_POINTER__ == 8
    case 8:
        // This is not a closed straight-line arm: the extra native branch
        // must be represented by a separate, explicitly qualified contract.
        if (external_condition)
            *storage += 8;
        else
            *storage += 16;
        break;
#endif
    default:
        *storage += 2;
        break;
    }
    return *storage;
}
