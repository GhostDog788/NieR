#include <stddef.h>
#include <stdio.h>

struct Flags {
    unsigned low : 3;
    unsigned middle : 5;
    signed delta : 6;
    unsigned high : 18;
};

struct __attribute__((packed)) Packet {
    unsigned char tag;
    unsigned counter;
    struct Flags flags;
    unsigned short samples[2];
};

struct Container {
    unsigned char marker;
    size_t visits;
    struct Packet packet;
    unsigned tail;
};

static struct Container persistent = {
    0x5a, 17, {3, 0x10203040u, {5, 17, -7, 0x2abcdu}, {10, 20}}, 9
};

// All aggregates cross function boundaries by pointer, never by value. Native
// packed-field accesses retain their conservative alignment even inside the
// naturally aligned outer record. No misaligned ordinary unsigned* is formed.
__attribute__((noinline)) void mutate_packet(struct Packet *packet, unsigned seed) {
    packet->counter += seed;
    packet->flags.low = (packet->flags.low + 3) & 7;
    packet->flags.middle ^= 7;
    packet->flags.delta -= 5;
    packet->flags.high += seed;
    packet->samples[1] += packet->samples[0];
}

__attribute__((noinline)) void mutate_container(struct Container *container) {
    mutate_packet(&container->packet, 7);
    container->visits += sizeof(void *);
    container->tail += container->packet.tag;
}

int main(void) {
    // Ordinary aggregate storage copying is distinct from aggregate call ABI.
    struct Packet local = persistent.packet;
    mutate_container(&persistent);
    if (persistent.marker != 0x5a || persistent.packet.tag != 3 ||
        persistent.tail != 12 || local.counter != 0x10203040u ||
        local.flags.low != 5 || local.flags.middle != 17 ||
        local.flags.delta != -7 || local.flags.high != 0x2abcdu ||
        local.samples[1] != 20)
        return 1;
    printf("storage %zu %zu %zu %u %u %u %d %u %u\n",
           sizeof(struct Container), offsetof(struct Container, packet), persistent.visits,
           persistent.packet.counter, persistent.packet.flags.low,
           persistent.packet.flags.middle, persistent.packet.flags.delta,
           persistent.packet.flags.high, (unsigned)persistent.packet.samples[1]);
    return 0;
}
