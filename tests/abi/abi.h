#ifndef NIER_TEST_NATIVE_ABI_H
#define NIER_TEST_NATIVE_ABI_H

typedef struct { int first, second; } Pair;
typedef struct { double amount; int tag; } Mixed;
typedef struct { double first, second; long tag; } Large;

typedef Pair (*PairCallback)(Pair, int);
typedef Mixed (*MixedCallback)(Mixed, int);
typedef Large (*LargeCallback)(Large, int);

Pair pair_transform(Pair value, int delta);
Mixed mixed_transform(Mixed value, int delta);
Large large_transform(Large value, int delta);
Pair pair_indirect(PairCallback callback, Pair value, int delta);
Mixed mixed_indirect(MixedCallback callback, Mixed value, int delta);
Large large_indirect(LargeCallback callback, Large value, int delta);

Pair pair_register_edge(int a, int b, int c, int d, int e, Pair value, int tail);
Pair pair_stack_edge(int a, int b, int c, int d, int e, int f, Pair value, int tail);
Mixed mixed_stack_edge(int a, int b, int c, int d, int e, int f,
                       double x0, double x1, double x2, double x3,
                       double x4, double x5, double x6, double x7,
                       Mixed value, int tail);

Pair native_pair_bridge(PairCallback callback, Pair value, int delta);
Mixed native_mixed_bridge(MixedCallback callback, Mixed value, int delta);
Large native_large_bridge(LargeCallback callback, Large value, int delta);
double scalar_variadic(int count, ...);
double aggregate_variadic(int prefix, ...);

#endif
